#pragma once

#include "workspace/lsp/WorkspaceLspClient.h"

#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

class LspManager {
 public:
  LspManager();
  ~LspManager();
  LspManager(const LspManager&) = delete;
  LspManager& operator=(const LspManager&) = delete;

  void SetWakeChannel(Uint32 event_type);

  void RegisterServer(const std::vector<std::string>& language_ids,
                      const platform::ProcessLauncher& launcher,
                      const std::vector<std::string>& command,
                      const std::string& root_uri,
                      const std::string& cwd = {},
                      bool eager_start = true,
                      const util::JsonValue& initialization_options = {},
                      const util::JsonValue& settings = {},
                      const platform::SubprocessSandbox& sandbox = {});
  void BeginShutdownServersNotIn(const std::unordered_set<std::string>& language_ids);

  LspClient* GetServer(const std::string& language_id);
  LspClient* FindStartedServer(const std::string& language_id);
  std::size_t CloseDocumentEverywhere(const std::string& uri);

  bool HasServer(const std::string& language_id) const;
  bool HasRegisteredServers() const;

  bool IsServerRunning(const std::string& language_id) const;

  std::string LastServerError(const std::string& language_id) const;

  void DrainCallbacks();

  struct WatchedFileChange {
    std::string relative_path;
    std::string absolute_path;
    std::string uri;
    LspFileChangeType type = LspFileChangeType::Changed;
  };

  std::size_t NotifyWatchedFileChanges(const std::vector<WatchedFileChange>& changes);

  void BeginShutdownAll();

  std::vector<std::unique_ptr<LspClient>> BeginShutdownAllAndTakeClients();

  void ShutdownAll();

  void InstallTestClientForTesting(const std::vector<std::string>& language_ids,
                                   std::unique_ptr<LspClient> client);
  void InstallTestClientForTesting(const std::string& language_id, std::unique_ptr<LspClient> client);

  bool InstallTestClientIntoExistingForTesting(const std::string& language_id,
                                               std::unique_ptr<LspClient> client);

 private:
  struct ServerEntry {
    std::vector<std::string> language_ids;
    const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
    std::vector<std::string> command;
    std::string root_uri;
    std::string cwd;
    std::string last_error;
    util::JsonValue initialization_options;
    util::JsonValue settings;
    platform::SubprocessSandbox sandbox;
    std::unique_ptr<LspClient> client;
    bool test_install = false;
  };

  Uint32 wake_event_type_ = 0;
  std::unordered_map<std::string, ServerEntry> servers_;
  std::unordered_map<std::string, std::string> alias_;
  std::vector<std::unique_ptr<LspClient>> retiring_clients_;

  ServerEntry* ResolveEntry(const std::string& language_id);
  const ServerEntry* ResolveEntry(const std::string& language_id) const;
  LspClient* EnsureStarted(ServerEntry& entry);
  void CollectRetiredClients();
};

}
