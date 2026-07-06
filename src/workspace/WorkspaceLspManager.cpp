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

LspManager::ServerEntry* LspManager::ResolveEntry(const std::string& language_id) {
  auto alias_it = alias_.find(language_id);
  if (alias_it == alias_.end()) {
    return nullptr;
  }
  auto it = servers_.find(alias_it->second);
  return it == servers_.end() ? nullptr : &it->second;
}

const LspManager::ServerEntry* LspManager::ResolveEntry(const std::string& language_id) const {
  auto alias_it = alias_.find(language_id);
  if (alias_it == alias_.end()) {
    return nullptr;
  }
  auto it = servers_.find(alias_it->second);
  return it == servers_.end() ? nullptr : &it->second;
}

void LspManager::RegisterServer(const std::vector<std::string>& language_ids,
                                const std::vector<std::string>& command,
                                const std::string& root_uri,
                                const std::string& cwd,
                                bool eager_start,
                                const util::JsonValue& initialization_options,
                                const util::JsonValue& settings,
                                const platform::SubprocessSandbox& sandbox) {
  if (language_ids.empty()) {
    return;
  }
  // Canonical key is the first language id; every id aliases to it.
  const std::string& key = language_ids.front();
  auto it = servers_.find(key);
  if (it != servers_.end()) {
    ServerEntry& existing = it->second;
    if (existing.language_ids == language_ids && existing.command == command &&
        existing.root_uri == root_uri && existing.cwd == cwd &&
        util::SerializeJson(existing.initialization_options) ==
            util::SerializeJson(initialization_options) &&
        util::SerializeJson(existing.settings) == util::SerializeJson(settings)) {
      if (eager_start) {
        (void)EnsureStarted(existing);
      }
      return;
    }
    if (existing.client != nullptr) {
      existing.client->BeginShutdown();
      retiring_clients_.push_back(std::move(existing.client));
    }
  }
  ServerEntry& entry = servers_[key];
  entry = ServerEntry{};
  entry.language_ids = language_ids;
  entry.command = command;
  entry.root_uri = root_uri;
  entry.cwd = cwd;
  entry.initialization_options = initialization_options;
  entry.settings = settings;
  entry.sandbox = sandbox;
  for (const std::string& id : language_ids) {
    alias_[id] = key;
  }
  if (eager_start) {
    (void)EnsureStarted(entry);
  }
}

void LspManager::BeginShutdownServersNotIn(const std::unordered_set<std::string>& language_ids) {
  for (auto it = servers_.begin(); it != servers_.end();) {
    // Keep a server if any language id it serves is still active.
    bool keep = false;
    for (const std::string& id : it->second.language_ids) {
      if (language_ids.contains(id)) {
        keep = true;
        break;
      }
    }
    if (keep) {
      ++it;
      continue;
    }
    if (it->second.client != nullptr) {
      it->second.client->BeginShutdown();
      retiring_clients_.push_back(std::move(it->second.client));
    }
    it = servers_.erase(it);
  }
  // Drop alias entries that no longer point at a live server.
  for (auto alias_it = alias_.begin(); alias_it != alias_.end();) {
    if (servers_.find(alias_it->second) == servers_.end()) {
      alias_it = alias_.erase(alias_it);
    } else {
      ++alias_it;
    }
  }
  CollectRetiredClients();
}

LspClient* LspManager::EnsureStarted(ServerEntry& entry) {
  if (entry.test_install && entry.client != nullptr) {
    return entry.client.get();
  }
  if (entry.client == nullptr) {
    util::StartupTrace::Scope trace_scope("LspManager::GetServer::InitializeServer");
    entry.last_error.clear();
    entry.client = std::make_unique<LspClient>();
    entry.client->SetWakeEventType(wake_event_type_);
    // The label is only used for tracing; use the canonical language id.
    const std::string& label =
        entry.language_ids.empty() ? std::string() : entry.language_ids.front();
    if (!entry.client->Start(entry.command, entry.root_uri, label, entry.cwd,
                             entry.initialization_options, entry.settings, entry.sandbox)) {
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

LspClient* LspManager::GetServer(const std::string& language_id) {
  ServerEntry* entry = ResolveEntry(language_id);
  return entry == nullptr ? nullptr : EnsureStarted(*entry);
}

LspClient* LspManager::FindStartedServer(const std::string& language_id) {
  ServerEntry* entry = ResolveEntry(language_id);
  if (entry == nullptr || entry->client == nullptr) {
    return nullptr;
  }
  if (entry->test_install) {
    return entry->client.get();
  }
  return entry->client->IsRunning() ? entry->client.get() : nullptr;
}

bool LspManager::HasServer(const std::string& language_id) const {
  return ResolveEntry(language_id) != nullptr;
}

bool LspManager::HasRegisteredServers() const {
  return !servers_.empty();
}

void LspManager::DrainCallbacks() {
  util::StartupTrace::Scope trace_scope("LspManager::DrainCallbacks");
  for (auto& [_, entry] : servers_) {
    // Drain regardless of IsRunning(): when a server dies unexpectedly its IO
    // thread posts synthetic failure callbacks (FailPendingRequests) — the only
    // thing that clears a hung "LSP: working..." indicator and the requesting UI's
    // loading state — and by then IsRunning() is already false. Gating on
    // IsRunning() stranded those callbacks unrun until teardown. Mirrors
    // DapManager::DrainCallbacks and the retiring_clients_ loop below.
    if (entry.client) {
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
  const ServerEntry* entry = ResolveEntry(language_id);
  return entry != nullptr && entry->client != nullptr && entry->client->IsRunning();
}

std::string LspManager::LastServerError(const std::string& language_id) const {
  const ServerEntry* entry = ResolveEntry(language_id);
  return entry == nullptr ? std::string{} : entry->last_error;
}

void LspManager::BeginShutdownAll() {
  for (auto& [_, entry] : servers_) {
    if (entry.client) {
      entry.client->BeginShutdown();
      retiring_clients_.push_back(std::move(entry.client));
    }
  }
  servers_.clear();
  alias_.clear();
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

void LspManager::InstallTestClientForTesting(const std::vector<std::string>& language_ids,
                                              std::unique_ptr<LspClient> client) {
  if (language_ids.empty()) {
    return;
  }
  const std::string& key = language_ids.front();
  ServerEntry& entry = servers_[key];
  entry = ServerEntry{};
  entry.language_ids = language_ids;
  entry.client = std::move(client);
  entry.test_install = true;
  for (const std::string& id : language_ids) {
    alias_[id] = key;
  }
}

void LspManager::InstallTestClientForTesting(const std::string& language_id,
                                              std::unique_ptr<LspClient> client) {
  InstallTestClientForTesting(std::vector<std::string>{language_id}, std::move(client));
}

bool LspManager::InstallTestClientIntoExistingForTesting(const std::string& language_id,
                                                         std::unique_ptr<LspClient> client) {
  ServerEntry* entry = ResolveEntry(language_id);
  if (entry == nullptr) {
    return false;
  }
  entry->client = std::move(client);
  entry->test_install = true;
  entry->last_error.clear();
  return true;
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
