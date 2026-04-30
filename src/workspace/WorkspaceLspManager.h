#pragma once

#include "workspace/WorkspaceLspClient.h"

#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

// Manages multiple LSP servers, one per language_id.
class LspManager {
 public:
  LspManager();
  ~LspManager();
  LspManager(const LspManager&) = delete;
  LspManager& operator=(const LspManager&) = delete;

  // Set SDL event type used to wake the main loop when responses arrive.
  void SetWakeEventType(Uint32 event_type);

  // Register a server command for a language.
  // If eager_start is true, the server will be started in the background immediately.
  void RegisterServer(const std::string& language_id, const std::vector<std::string>& command,
                      const std::string& root_uri,
                      const std::string& cwd = {},
                      bool eager_start = true);
  void BeginShutdownServersNotIn(const std::unordered_set<std::string>& language_ids);

  // Get or start server for language; returns nullptr if not registered or failed to start.
  LspClient* GetServer(const std::string& language_id);
  LspClient* FindStartedServer(const std::string& language_id);

  // True if a server is registered for language, regardless of running state.
  bool HasServer(const std::string& language_id) const;
  bool HasRegisteredServers() const;

  // True if server is running for language.
  bool IsServerRunning(const std::string& language_id) const;

  // Last startup/runtime error for a language server, if any.
  std::string LastServerError(const std::string& language_id) const;

  // Call from main thread each frame to dispatch pending LSP callbacks.
  void DrainCallbacks();

  // Begin background shutdown for all active servers without blocking the caller.
  void BeginShutdownAll();

  // Stop all servers and wait for active and retiring processes to exit.
  void ShutdownAll();

 private:
  struct ServerEntry {
    std::vector<std::string> command;
    std::string root_uri;
    std::string cwd;
    std::string last_error;
    std::unique_ptr<LspClient> client;
  };

  Uint32 wake_event_type_ = 0;
  std::unordered_map<std::string, ServerEntry> servers_;
  std::vector<std::unique_ptr<LspClient>> retiring_clients_;

  void CollectRetiredClients();
};

}  // namespace microide::workspace
