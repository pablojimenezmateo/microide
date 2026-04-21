#pragma once

#include "workspace/WorkspaceLspClient.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace microide::workspace {

// Manages multiple LSP servers, one per language_id.
class LspManager {
 public:
  LspManager();
  ~LspManager();
  LspManager(const LspManager&) = delete;
  LspManager& operator=(const LspManager&) = delete;

  // Register a server command for a language.
  // command: e.g., ["rust-analyzer"]
  // root_uri: workspace root, e.g., "file:///path/to/project"
  void RegisterServer(const std::string& language_id, const std::vector<std::string>& command,
                      const std::string& root_uri);

  // Get or start server for language; returns nullptr if not registered or failed to start.
  LspClient* GetServer(const std::string& language_id);

  // True if server is running for language.
  bool IsServerRunning(const std::string& language_id) const;

  // Stop all servers.
  void ShutdownAll();

 private:
  struct ServerEntry {
    std::vector<std::string> command;
    std::string root_uri;
    std::unique_ptr<LspClient> client;
  };

  std::unordered_map<std::string, ServerEntry> servers_;
};

}  // namespace microide::workspace
