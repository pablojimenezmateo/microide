#include "workspace/WorkspaceDapManager.h"

#include <utility>

namespace microide::workspace {

namespace {

std::string JoinCommand(const std::vector<std::string>& command) {
  std::string joined;
  for (std::size_t i = 0; i < command.size(); ++i) {
    if (i > 0) {
      joined += ' ';
    }
    joined += command[i];
  }
  return joined;
}

}  // namespace

DapManager::DapManager() = default;

DapManager::~DapManager() { ShutdownAll(); }

void DapManager::SetWakeEventType(Uint32 event_type) {
  wake_event_type_ = event_type;
  for (SessionEntry& entry : sessions_) {
    entry.session->SetWakeEventType(wake_event_type_);
  }
}

void DapManager::RegisterAdapter(const std::string& type, const std::vector<std::string>& command,
                                 const platform::SubprocessSandbox& sandbox) {
  if (type.empty()) {
    return;
  }
  AdapterEntry& entry = adapters_[type];
  entry.command = command;
  entry.sandbox = sandbox;
}

void DapManager::RetainAdaptersIn(const std::unordered_set<std::string>& types) {
  for (auto it = adapters_.begin(); it != adapters_.end();) {
    if (types.contains(it->first)) {
      ++it;
    } else {
      it = adapters_.erase(it);
    }
  }
}

bool DapManager::HasAdapter(const std::string& type) const {
  return adapters_.find(type) != adapters_.end();
}

bool DapManager::HasRegisteredAdapters() const { return !adapters_.empty(); }

std::vector<std::string> DapManager::AdapterTypes() const {
  std::vector<std::string> types;
  types.reserve(adapters_.size());
  for (const auto& [type, _] : adapters_) {
    types.push_back(type);
  }
  return types;
}

int DapManager::StartSession(const LaunchConfig& config,
                             std::function<DebugSession::Callbacks(int id)> make_callbacks,
                             const std::string& cwd) {
  last_error_.clear();
  auto it = adapters_.find(config.type);
  if (it == adapters_.end()) {
    last_error_ = "no debug adapter registered for type '" + config.type + "'";
    return 0;
  }

  AdapterEntry& entry = it->second;
  if (entry.command.empty()) {
    last_error_ = "debug adapter '" + config.type + "' has no command";
    return 0;
  }

  const int id = next_session_id_++;
  SessionEntry session_entry;
  session_entry.id = id;
  session_entry.session = std::make_unique<DebugSession>();
  if (wake_event_type_ != 0) {
    session_entry.session->SetWakeEventType(wake_event_type_);
  }
  session_entry.session->SetCallbacks(make_callbacks ? make_callbacks(id)
                                                     : DebugSession::Callbacks{});
  // Make the session active before Start() so any synchronous state callback it
  // fires already sees it as the active session.
  sessions_.push_back(std::move(session_entry));
  active_session_id_ = id;
  DebugSession& session = *sessions_.back().session;
  if (!session.Start(entry.command, config, cwd, entry.sandbox)) {
    last_error_ = session.LastError();
    if (last_error_.empty()) {
      last_error_ = "debug adapter failed to start";
    }
    last_error_ += " [command: " + JoinCommand(entry.command) + "]";
    // Drop the failed entry and repoint active to the most recent survivor.
    sessions_.pop_back();
    active_session_id_ = sessions_.empty() ? 0 : sessions_.back().id;
    return 0;
  }
  return id;
}

int DapManager::StartSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                             const std::string& cwd) {
  return StartSession(
      config, [cb = std::move(callbacks)](int) mutable { return std::move(cb); }, cwd);
}

int DapManager::ReplaceActiveSession(const LaunchConfig& config,
                                     std::function<DebugSession::Callbacks(int id)> make_callbacks,
                                     const std::string& cwd) {
  ClearActiveEntry();
  return StartSession(config, std::move(make_callbacks), cwd);
}

int DapManager::ReplaceActiveSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                                     const std::string& cwd) {
  ClearActiveEntry();
  return StartSession(config, std::move(callbacks), cwd);
}

std::size_t DapManager::ActiveIndex() const {
  for (std::size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i].id == active_session_id_) {
      return i;
    }
  }
  return sessions_.size();
}

DebugSession* DapManager::ActiveSession() {
  const std::size_t index = ActiveIndex();
  return index < sessions_.size() ? sessions_[index].session.get() : nullptr;
}

const DebugSession* DapManager::ActiveSession() const {
  return const_cast<DapManager*>(this)->ActiveSession();
}

void DapManager::SetActiveSession(int id) {
  for (const SessionEntry& entry : sessions_) {
    if (entry.id == id) {
      active_session_id_ = id;
      return;
    }
  }
}

DebugSession* DapManager::SessionById(int id) {
  for (SessionEntry& entry : sessions_) {
    if (entry.id == id) {
      return entry.session.get();
    }
  }
  return nullptr;
}

void DapManager::SetSessionAttention(int id, bool attention) {
  for (SessionEntry& entry : sessions_) {
    if (entry.id == id) {
      entry.attention = attention;
      return;
    }
  }
}

std::vector<DapSessionInfo> DapManager::Sessions() const {
  std::vector<DapSessionInfo> infos;
  infos.reserve(sessions_.size());
  for (const SessionEntry& entry : sessions_) {
    DapSessionInfo info;
    info.id = entry.id;
    const LaunchConfig& config = entry.session->Config();
    info.name = !config.name.empty() ? config.name : config.type;
    info.state = entry.session->CurrentState();
    info.attention = entry.attention;
    infos.push_back(std::move(info));
  }
  return infos;
}

void DapManager::StopActiveSession() {
  if (DebugSession* session = ActiveSession(); session != nullptr) {
    session->RequestStop();
  }
}

void DapManager::ClearActiveEntry() {
  const std::size_t index = ActiveIndex();
  if (index >= sessions_.size()) {
    return;
  }
  sessions_[index].session->RequestStop();
  // ~DebugSession -> ~DapClient blocks until shutdown completes.
  sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
  active_session_id_ = sessions_.empty() ? 0 : sessions_.back().id;
}

void DapManager::ClearSession() { ClearActiveEntry(); }

std::vector<int> DapManager::PruneTerminated() {
  std::vector<int> removed;
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    DebugSession& session = *it->session;
    const DebugSession::State state = session.CurrentState();
    const bool terminal =
        state == DebugSession::State::Terminated || state == DebugSession::State::Failed;
    if (terminal && !session.Client().IsRunning()) {
      const bool was_active = it->id == active_session_id_;
      removed.push_back(it->id);
      it = sessions_.erase(it);
      if (was_active) {
        active_session_id_ = sessions_.empty() ? 0 : sessions_.back().id;
      }
    } else {
      ++it;
    }
  }
  return removed;
}

void DapManager::DrainCallbacks() {
  for (SessionEntry& entry : sessions_) {
    entry.session->DrainCallbacks();
  }
}

void DapManager::BeginShutdownAll() {
  for (SessionEntry& entry : sessions_) {
    entry.session->RequestStop();
  }
}

void DapManager::ShutdownAll() {
  BeginShutdownAll();
  sessions_.clear();
  active_session_id_ = 0;
}

}  // namespace microide::workspace
