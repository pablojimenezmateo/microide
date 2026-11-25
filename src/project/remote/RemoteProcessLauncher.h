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
#include "project/ProjectSearchService.h"
#include "project/remote/RemotePathMap.h"
#include "terminal/TerminalHostChannel.h"
#include "project/remote/RemoteServerClient.h"

namespace microide::project::remote {

class RemoteConnection;

class RemoteProcessLauncher final : public platform::ProcessLauncher,
                                    public project::GitMetadataSource,
                                    public project::ProjectSearchHost,
                                    public terminal::HostTerminalSource {
 public:
  struct Options {
    bool host_paths_readable_locally = false;
    std::string description = "remote";
    std::size_t terminal_credit_bytes = 0;
    std::size_t terminal_prefetch_lines = 0;
    std::filesystem::path host_file_cache;
    std::string host_label;
  };

  RemoteProcessLauncher(std::shared_ptr<RemoteConnection> connection, RemotePathMap map,
                        Options options);
  RemoteProcessLauncher(std::shared_ptr<RemoteServerClient> client, RemotePathMap map,
                        Options options);

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return argv;
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override;
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override;
  std::optional<std::filesystem::path> HostPathOfCached(const std::filesystem::path& local_path) const;
  std::string DisplayPathOf(const std::filesystem::path& local_path) const override;
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool StartAsync(platform::AsyncSubprocess& process, const std::vector<std::string>& argv,
                  const std::filesystem::path& cwd,
                  const platform::SubprocessSandbox& sandbox) const override;
  bool is_local() const override { return false; }
  std::string_view description() const override { return options_.description; }
  const Options& options() const { return options_; }

  project::GitAvailability Availability(const std::filesystem::path& root) const override;
  std::optional<project::ProjectSearchCompletion> SearchOnHost(
      const std::filesystem::path& root, const std::string& query,
      const project::ProjectSearchOptions& options, const project::ProjectSearchSink& sink,
      const std::atomic_bool& cancel, const util::CancellationToken& token) const override;
  std::optional<project::GitStatusOutput> CurrentStatus(
      const std::filesystem::path& root) const override;

  bool Ready() const override;
  std::shared_ptr<terminal::TerminalHostChannel> OpenTerminal(
      const OpenRequest& request, terminal::TerminalSession& session,
      std::string* error) const override;
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override;

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

}
