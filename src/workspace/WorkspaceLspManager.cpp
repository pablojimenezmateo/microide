#include "workspace/WorkspaceLspManager.h"

namespace microide::workspace {

LspManager::LspManager() = default;

LspManager::~LspManager() { ShutdownAll(); }

void LspManager::SetWakeEventType(Uint32 event_type) {
  wake_event_type_ = event_type;
}

void LspManager::RegisterServer(const std::string& language_id,
                                 const std::vector<std::string>& command,
                                 const std::string& root_uri) {
  servers_[language_id] = ServerEntry{command, root_uri, nullptr};
}

LspClient* LspManager::GetServer(const std::string& language_id) {
  auto it = servers_.find(language_id);
  if (it == servers_.end()) {
    return nullptr;
  }

  ServerEntry& entry = it->second;
  if (entry.client == nullptr) {
    entry.client = std::make_unique<LspClient>();
    entry.client->SetWakeEventType(wake_event_type_);
    if (!entry.client->Start(entry.command, entry.root_uri, language_id)) {
      entry.client = nullptr;
      return nullptr;
    }
  }

  if (entry.client->IsRunning()) {
    return entry.client.get();
  }
  entry.client = nullptr;
  return nullptr;
}

bool LspManager::HasServer(const std::string& language_id) const {
  return servers_.find(language_id) != servers_.end();
}

void LspManager::DrainCallbacks() {
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
