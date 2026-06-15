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
                                const std::string& root_uri,
                                const std::string& cwd,
                                bool eager_start,
                                const util::JsonValue& initialization_options,
                                const util::JsonValue& settings) {
  auto it = servers_.find(language_id);
  if (it != servers_.end()) {
    ServerEntry& existing = it->second;
    if (existing.command == command && existing.root_uri == root_uri && existing.cwd == cwd &&
        util::SerializeJson(existing.initialization_options) ==
            util::SerializeJson(initialization_options) &&
        util::SerializeJson(existing.settings) == util::SerializeJson(settings)) {
      if (eager_start) {
        (void)GetServer(language_id);
      }
      return;
    }
    if (existing.client != nullptr) {
      existing.client->BeginShutdown();
      retiring_clients_.push_back(std::move(existing.client));
    }
  }
  ServerEntry& entry = servers_[language_id];
  entry = ServerEntry{};
  entry.command = command;
  entry.root_uri = root_uri;
  entry.cwd = cwd;
  entry.initialization_options = initialization_options;
  entry.settings = settings;
  if (eager_start) {
    (void)GetServer(language_id);
  }
}

void LspManager::BeginShutdownServersNotIn(const std::unordered_set<std::string>& language_ids) {
  for (auto it = servers_.begin(); it != servers_.end();) {
    if (language_ids.contains(it->first)) {
      ++it;
      continue;
    }
    if (it->second.client != nullptr) {
      it->second.client->BeginShutdown();
      retiring_clients_.push_back(std::move(it->second.client));
    }
    it = servers_.erase(it);
  }
  CollectRetiredClients();
}

LspClient* LspManager::GetServer(const std::string& language_id) {
  auto it = servers_.find(language_id);
  if (it == servers_.end()) {
    return nullptr;
  }

  ServerEntry& entry = it->second;
  if (entry.test_install && entry.client != nullptr) {
    return entry.client.get();
  }
  if (entry.client == nullptr) {
    util::StartupTrace::Scope trace_scope("LspManager::GetServer::InitializeServer");
    entry.last_error.clear();
    entry.client = std::make_unique<LspClient>();
    entry.client->SetWakeEventType(wake_event_type_);
    if (!entry.client->Start(entry.command, entry.root_uri, language_id, entry.cwd,
                             entry.initialization_options, entry.settings)) {
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

LspClient* LspManager::FindStartedServer(const std::string& language_id) {
  auto it = servers_.find(language_id);
  if (it == servers_.end() || it->second.client == nullptr) {
    return nullptr;
  }
  if (it->second.test_install) {
    return it->second.client.get();
  }
  return it->second.client->IsRunning() ? it->second.client.get() : nullptr;
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
  for (auto& client : retiring_clients_) {
    if (client != nullptr) {
      client->DrainCallbacks();
    }
  }
  CollectRetiredClients();
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

void LspManager::BeginShutdownAll() {
  for (auto& [_, entry] : servers_) {
    if (entry.client) {
      entry.client->BeginShutdown();
      retiring_clients_.push_back(std::move(entry.client));
    }
  }
  servers_.clear();
  CollectRetiredClients();
}

void LspManager::ShutdownAll() {
  BeginShutdownAll();
  for (auto& client : retiring_clients_) {
    if (client != nullptr) {
      client->Shutdown();
    }
  }
  retiring_clients_.clear();
}

void LspManager::InstallTestClientForTesting(const std::string& language_id,
                                              std::unique_ptr<LspClient> client) {
  ServerEntry& entry = servers_[language_id];
  entry = ServerEntry{};
  entry.client = std::move(client);
  entry.test_install = true;
}

void LspManager::CollectRetiredClients() {
  auto write_it = retiring_clients_.begin();
  for (auto read_it = retiring_clients_.begin(); read_it != retiring_clients_.end(); ++read_it) {
    if (*read_it != nullptr && (*read_it)->IsShutdownComplete()) {
      (*read_it)->Shutdown();
      continue;
    }
    if (write_it != read_it) {
      *write_it = std::move(*read_it);
    }
    ++write_it;
  }
  retiring_clients_.erase(write_it, retiring_clients_.end());
}

}  // namespace microide::workspace
