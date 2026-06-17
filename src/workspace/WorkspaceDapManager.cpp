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
  if (session_ != nullptr) {
    session_->SetWakeEventType(wake_event_type_);
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

bool DapManager::StartSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                              const std::string& cwd) {
  last_error_.clear();
  auto it = adapters_.find(config.type);
  if (it == adapters_.end()) {
    last_error_ = "no debug adapter registered for type '" + config.type + "'";
    return false;
  }

  // Stop and drop any previous session before starting a new one.
  ClearSession();

  AdapterEntry& entry = it->second;

  if (entry.command.empty()) {
    last_error_ = "debug adapter '" + config.type + "' has no command";
    return false;
  }

  session_ = std::make_unique<DebugSession>();
  if (wake_event_type_ != 0) {
    session_->SetWakeEventType(wake_event_type_);
  }
  session_->SetCallbacks(std::move(callbacks));
  if (!session_->Start(entry.command, config, cwd, entry.sandbox)) {
    last_error_ = session_->LastError();
    if (last_error_.empty()) {
      last_error_ = "debug adapter failed to start";
    }
    last_error_ += " [command: " + JoinCommand(entry.command) + "]";
    return false;
  }
  return true;
}

void DapManager::StopActiveSession() {
  if (session_ != nullptr) {
    session_->RequestStop();
  }
}

void DapManager::ClearSession() {
  if (session_ != nullptr) {
    session_->RequestStop();
    session_.reset();  // ~DebugSession -> ~DapClient blocks until shutdown completes
  }
}

void DapManager::DrainCallbacks() {
  if (session_ != nullptr) {
    session_->DrainCallbacks();
  }
}

void DapManager::BeginShutdownAll() {
  if (session_ != nullptr) {
    session_->RequestStop();
  }
}

void DapManager::ShutdownAll() {
  BeginShutdownAll();
  session_.reset();
}

}  // namespace microide::workspace
