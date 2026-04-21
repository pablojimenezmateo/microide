#include "workspace/WorkspaceDapManager.h"

namespace microide::workspace {

DapManager::DapManager() = default;

DapManager::~DapManager() { ShutdownAll(); }

void DapManager::RegisterDebugger(const std::string& type,
                                   const std::vector<std::string>& command) {
  debuggers_[type] = DebuggerEntry{command, nullptr};
}

LspClient* DapManager::GetDebugger(const std::string& type) {
  auto it = debuggers_.find(type);
  if (it == debuggers_.end()) {
    return nullptr;
  }

  DebuggerEntry& entry = it->second;
  if (entry.client == nullptr) {
    entry.client = std::make_unique<LspClient>();
    if (!entry.client->Start(entry.command, "", type)) {
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

bool DapManager::IsDebuggerRunning(const std::string& type) const {
  auto it = debuggers_.find(type);
  if (it == debuggers_.end()) {
    return false;
  }
  return it->second.client != nullptr && it->second.client->IsRunning();
}

void DapManager::ShutdownAll() {
  for (auto& [_, entry] : debuggers_) {
    if (entry.client) {
      entry.client->Shutdown();
      entry.client = nullptr;
    }
  }
  debuggers_.clear();
}

}  // namespace microide::workspace
