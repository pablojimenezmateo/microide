#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/GitMetadataSource.h"
#include "project/remote/RemotePathMap.h"
#include "terminal/TerminalHostChannel.h"
#include "project/remote/RemoteServerClient.h"

namespace microide::project::remote {

class RemoteConnection;

// The project launcher of a host session (dev-docs/design/remote-projects.md § 6.5):
// every spawn the project makes — git, the formatter, plugin tools through Run; the
// language server and the debug adapter through StartAsync — becomes a `proc/spawn`
// on the host, with the working directory mapped from the editor's tree to the
// host's and argv passed through untouched. It is also the project's git metadata
// source, answered by the host's `git/metadata` and cached per root, so a status
// probe never costs a host process.
class RemoteProcessLauncher final : public platform::ProcessLauncher,
                                    public project::GitMetadataSource,
                                    public terminal::HostTerminalSource {
 public:
  struct Options {
    // True only when the host's paths are ALSO readable here (the parity suite's
    // server-on-this-machine locality). A real host's paths may exist locally with
    // other content, so ReadableGitDirectory says nothing rather than guess.
    bool host_paths_readable_locally = false;
    std::string description = "remote";
    // Per host terminal: remote.term_credit_bytes and remote.scrollback_prefetch_lines
    // (0 = the host's defaults).
    std::size_t terminal_credit_bytes = 0;
    std::size_t terminal_prefetch_lines = 0;
    // Where host files OUTSIDE the project live on this side ("" = nowhere): a host
    // path a language server or debugger names that the mirror does not hold maps to
    // <cache>/<host path>, fetched read-only when the editor opens it (file/read).
    std::filesystem::path host_file_cache;
    // The host's name as the user typed it, for DisplayPathOf ("" = no display).
    std::string host_label;
  };

  // Over the host's current connection, which a reconnect replaces underneath.
  RemoteProcessLauncher(std::shared_ptr<RemoteConnection> connection, RemotePathMap map,
                        Options options);
  // Over one client for good (tests, the parity suite).
  RemoteProcessLauncher(std::shared_ptr<RemoteServerClient> client, RemotePathMap map,
                        Options options);

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override;
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override;
  // The host path a file in the out-of-project cache stands for, if it is one.
  std::optional<std::filesystem::path> HostPathOfCached(const std::filesystem::path& local_path) const;
  // A cached host copy shows as `host:/abs/path`.
  std::string DisplayPathOf(const std::filesystem::path& local_path) const override;
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool StartAsync(platform::AsyncSubprocess& process, const std::vector<std::string>& argv,
                  const std::filesystem::path& cwd,
                  const platform::SubprocessSandbox& sandbox) const override;
  bool is_local() const override { return false; }
  std::string_view description() const override { return options_.description; }

  project::GitAvailability Availability(const std::filesystem::path& root) const override;

  // A terminal in a remote project is a host terminal: the pty and the shell run
  // on the host (term/open), never a local shell in a mapped directory.
  std::shared_ptr<terminal::TerminalHostChannel> OpenTerminal(
      const OpenRequest& request, terminal::TerminalSession& session,
      std::string* error) const override;
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override;

  // Host processes started through this launcher (Run and StartAsync), for the
  // parity suite's spawn-count check, and the most recent of them as
  // "program arg…" lines (bounded) for diagnostics.
  std::size_t spawn_count() const;
  std::vector<std::string> recent_spawns() const;

 private:
  std::optional<RemoteServerClient::GitMetadata> Metadata(const std::filesystem::path& root) const;

  std::shared_ptr<RemoteConnection> connection_;
  RemotePathMap map_;
  Options options_;
  mutable std::mutex mutex_;
  mutable std::map<std::filesystem::path, RemoteServerClient::GitMetadata> metadata_;
  void Record(const std::vector<std::string>& argv) const;

  mutable std::size_t spawns_ = 0;
  mutable std::vector<std::string> recent_;
};

}  // namespace microide::project::remote
