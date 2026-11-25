#pragma once

#include <atomic>
#include <memory>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"
#include "project/GitMetadataSource.h"
#include "project/remote/RemotePathMap.h"

namespace microide::project::remote {
class RemoteProcessLauncher;
}

namespace microide::tests::parity {

class LoopbackPathMap : public project::remote::RemotePathMap {
 public:
  using RemotePathMap::RemotePathMap;
  std::optional<std::filesystem::path> ToMirror(const std::filesystem::path& host_path) const {
    return ToLocal(host_path);
  }
  const std::filesystem::path& mirror_root() const { return local_root(); }
};

class LoopbackProcessLauncher final : public platform::ProcessLauncher,
                                      public project::GitMetadataSource {
 public:
  explicit LoopbackProcessLauncher(LoopbackPathMap map) : map_(std::move(map)) {}

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override;
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override;
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override;
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool is_local() const override { return false; }
  std::string_view description() const override { return "loopback"; }

  project::GitAvailability Availability(const std::filesystem::path& root) const override;
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override;

  std::vector<std::string> spawns() const;
  std::size_t spawn_count() const;

 private:
  void Record(const std::vector<std::string>& argv) const;

  LoopbackPathMap map_;
  mutable std::mutex mutex_;
  mutable std::vector<std::string> spawns_;
};

class RecordingLocalLauncher final : public platform::ProcessLauncher {
 public:
  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override;
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return cwd;
  }
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override {
    return host_path;
  }
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool is_local() const override { return true; }
  std::string_view description() const override { return "local (recorded)"; }

  std::vector<std::string> spawns() const;

 private:
  mutable std::mutex mutex_;
  mutable std::vector<std::string> spawns_;
};

class LoopbackWriteGate final : public project::FileWriteGate {
 public:
  explicit LoopbackWriteGate(LoopbackPathMap map) : map_(std::move(map)) {}

  [[nodiscard]] Result WriteText(const std::filesystem::path& path, std::string_view text,
                                 Signature signature = Signature::Skip,
                                 util::WriteTarget target = util::WriteTarget::Replace) override;
  [[nodiscard]] TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override;
  void DisposeStaged(std::span<const std::filesystem::path> staged) override;

  std::size_t write_count() const { return writes_.load(); }

 private:
  LoopbackPathMap map_;
  std::atomic<std::size_t> writes_{0};
};

std::unique_ptr<project::remote::RemoteProcessLauncher> ConnectServerLocality(
    const LoopbackPathMap& map, std::string* error);

}
