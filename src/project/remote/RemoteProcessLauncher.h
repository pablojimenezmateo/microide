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
#include "project/remote/RemoteServerClient.h"

namespace microide::project::remote {

// The project launcher of a host session (dev-docs/design/remote-projects.md § 6.5):
// every spawn the project makes — git, the formatter, plugin tools through Run; the
// language server and the debug adapter through StartAsync — becomes a `proc/spawn`
// on the host, with the working directory mapped from the editor's tree to the
// host's and argv passed through untouched. It is also the project's git metadata
// source, answered by the host's `git/metadata` and cached per root, so a status
// probe never costs a host process.
class RemoteProcessLauncher final : public platform::ProcessLauncher,
                                    public project::GitMetadataSource {
 public:
  struct Options {
    // True only when the host's paths are ALSO readable here (the parity suite's
    // server-on-this-machine locality). A real host's paths may exist locally with
    // other content, so ReadableGitDirectory says nothing rather than guess.
    bool host_paths_readable_locally = false;
    std::string description = "remote";
  };

  RemoteProcessLauncher(std::shared_ptr<RemoteServerClient> client, RemotePathMap map,
                        Options options);

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override;
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override;
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool StartAsync(platform::AsyncSubprocess& process, const std::vector<std::string>& argv,
                  const std::filesystem::path& cwd,
                  const platform::SubprocessSandbox& sandbox) const override;
  bool is_local() const override { return false; }
  std::string_view description() const override { return options_.description; }

  project::GitAvailability Availability(const std::filesystem::path& root) const override;
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override;

  // Host processes started through this launcher (Run and StartAsync), for the
  // parity suite's spawn-count check.
  std::size_t spawn_count() const;

 private:
  std::optional<RemoteServerClient::GitMetadata> Metadata(const std::filesystem::path& root) const;

  std::shared_ptr<RemoteServerClient> client_;
  RemotePathMap map_;
  Options options_;
  mutable std::mutex mutex_;
  mutable std::map<std::filesystem::path, RemoteServerClient::GitMetadata> metadata_;
  mutable std::size_t spawns_ = 0;
};

}  // namespace microide::project::remote
