#include "workspace/WorkspaceLspManager.h"

#include "util/StartupTrace.h"

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

LspManager::LspManager() = default;

LspManager::~LspManager() { ShutdownAll(); }

void LspManager::SetWakeEventType(Uint32 event_type) {
  wake_event_type_ = event_type;
}

void LspManager::RegisterServer(const std::string& language_id,
                                 const std::vector<std::string>& command,
                                 const std::string& root_uri, bool eager_start) {
  servers_[language_id] = ServerEntry{command, root_uri, {}, nullptr};
  // Note: eager_start is currently unused; servers are started lazily on first use
  // to avoid race conditions during plugin initialization.
}

LspClient* LspManager::GetServer(const std::string& language_id) {
  auto it = servers_.find(language_id);
  if (it == servers_.end()) {
    return nullptr;
  }

  ServerEntry& entry = it->second;
  if (entry.client == nullptr) {
    util::StartupTrace::Scope trace_scope("LspManager::GetServer::InitializeServer");
    entry.last_error.clear();
    entry.client = std::make_unique<LspClient>();
    entry.client->SetWakeEventType(wake_event_type_);
    if (!entry.client->Start(entry.command, entry.root_uri, language_id)) {
      entry.last_error = entry.client->LastError();
      if (entry.last_error.empty()) {
        entry.last_error = "language server failed to start";
      }
      entry.last_error += " [command: " + JoinCommand(entry.command) + "]";
      entry.client = nullptr;
      return nullptr;
    }
  }

  if (entry.client->IsRunning()) {
    return entry.client.get();
  }
  if (entry.last_error.empty()) {
    entry.last_error = "language server process exited unexpectedly";
  }
  entry.client = nullptr;
  return nullptr;
}

bool LspManager::HasServer(const std::string& language_id) const {
  return servers_.find(language_id) != servers_.end();
}

bool LspManager::HasRegisteredServers() const {
  return !servers_.empty();
}

void LspManager::DrainCallbacks() {
  util::StartupTrace::Scope trace_scope("LspManager::DrainCallbacks");
  for (auto& [_, entry] : servers_) {
    if (entry.client && entry.client->IsRunning()) {
      entry.client->DrainCallbacks();
    }
  }
}

bool LspManager::IsServerRunning(const std::string& language_id) const {
  auto it = servers_.find(language_id);
  if (it == servers_.end()) {
    return false;
  }
  return it->second.client != nullptr && it->second.client->IsRunning();
}

std::string LspManager::LastServerError(const std::string& language_id) const {
  const auto it = servers_.find(language_id);
  if (it == servers_.end()) {
    return {};
  }
  return it->second.last_error;
}

void LspManager::ShutdownAll() {
  for (auto& [_, entry] : servers_) {
    if (entry.client) {
      entry.client->Shutdown();
      entry.client = nullptr;
    }
  }
  servers_.clear();
}

}  // namespace microide::workspace
