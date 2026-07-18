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

  // Register a server command for one or more languages. Every language id in
  // `language_ids` resolves to the same subprocess (e.g. clangd serving
  // c / c++ / objective-c), so a mixed-language project spawns one process.
  // If eager_start is true, the server will be started in the background immediately.
  // initialization_options / settings are forwarded to LspClient::Start.
  void RegisterServer(const std::vector<std::string>& language_ids,
                      const std::vector<std::string>& command,
                      const std::string& root_uri,
                      const std::string& cwd = {},
                      bool eager_start = true,
                      const util::JsonValue& initialization_options = {},
                      const util::JsonValue& settings = {},
                      const platform::SubprocessSandbox& sandbox = {});
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

  // Begin background shutdown for all active servers AND hand every retiring client
  // (both the just-begun ones and any already retiring) to the caller. Used so a
  // per-project manager can be destroyed without its ~LspManager blocking on
  // WaitForShutdown: a host-owned pool that outlives the project drains them async
  // instead (TD-2026-07-17-091).
  std::vector<std::unique_ptr<LspClient>> BeginShutdownAllAndTakeClients();

  // Stop all servers and wait for active and retiring processes to exit.
  void ShutdownAll();

  // Unit tests only: install a client without a language-server subprocess.
  // Every language id resolves to the supplied client (aliasing coverage).
  void InstallTestClientForTesting(const std::vector<std::string>& language_ids,
                                   std::unique_ptr<LspClient> client);
  void InstallTestClientForTesting(const std::string& language_id, std::unique_ptr<LspClient> client);

  // Unit tests only: attach a test client to an ALREADY-registered server entry
  // without touching its registration params (command/root/options). This keeps
  // RegisterServer idempotent across a plugin reload so the injected client is
  // retained (warm) rather than torn down. Returns false if no entry resolves
  // for `language_id`.
  bool InstallTestClientIntoExistingForTesting(const std::string& language_id,
                                               std::unique_ptr<LspClient> client);

 private:
  struct ServerEntry {
    // Every language id this single subprocess answers for.
    std::vector<std::string> language_ids;
    std::vector<std::string> command;
    std::string root_uri;
    std::string cwd;
    std::string last_error;
    util::JsonValue initialization_options;
    util::JsonValue settings;
    // Kernel-confinement descriptor for plugin-contributed servers; default-disabled (no-op) for
    // test-installed clients and any non-plugin registration.
    platform::SubprocessSandbox sandbox;
    std::unique_ptr<LspClient> client;
    bool test_install = false;
  };

  Uint32 wake_event_type_ = 0;
  // Keyed by canonical server key (the registration's first language id).
  std::unordered_map<std::string, ServerEntry> servers_;
  // Maps every declared language id to its canonical server key.
  std::unordered_map<std::string, std::string> alias_;
  std::vector<std::unique_ptr<LspClient>> retiring_clients_;

  // Resolve a language id to its server entry, or nullptr if none.
  ServerEntry* ResolveEntry(const std::string& language_id);
  const ServerEntry* ResolveEntry(const std::string& language_id) const;
  // Start (if needed) and return the running client for an entry, else nullptr.
  LspClient* EnsureStarted(ServerEntry& entry);
  void CollectRetiredClients();
};

}  // namespace microide::workspace
