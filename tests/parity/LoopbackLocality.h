#pragma once

#include <atomic>
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

namespace microide::tests::parity {

// The non-local locality the parity suite runs every scenario under before a
// remote server exists (dev-docs/design/remote-projects.md § 10.1, Groundwork G11).
//
// Two trees on this machine stand in for the two machines: the MIRROR, which the
// editor opens and does its I/O in, and the HOST, where every process the project
// spawns runs and whose bytes are what "the remote has". They are different
// directories on purpose: with one shared root every path-translation bug passes,
// because the untranslated path is also a correct one.

// Maps a mirror path onto the host tree, component-wise. A path outside the mirror
// root is not the project's and maps to nothing.
class LoopbackPathMap {
 public:
  LoopbackPathMap(std::filesystem::path mirror_root, std::filesystem::path host_root);
  std::optional<std::filesystem::path> ToHost(const std::filesystem::path& mirror_path) const;
  const std::filesystem::path& mirror_root() const { return mirror_root_; }
  const std::filesystem::path& host_root() const { return host_root_; }

 private:
  std::filesystem::path mirror_root_;
  std::filesystem::path host_root_;
};

// `is_local() == false`; runs the real command on this machine with the working
// directory mapped from the mirror to the host, and records every spawn. argv is
// passed through untouched -- exactly what a remote launcher can do, since it
// cannot see which words of an argv are paths (ProcessLauncher::Run), so a caller
// that names its tree inside argv shows up here as a process working on the mirror.
//
// It is also the project's git metadata source (project/GitMetadataSource.h), as a
// remote launcher is: availability and the git directory are answered from the
// HOST tree -- what the server's `git/metadata` reports -- because the mirror has
// no `.git` to stat.
class LoopbackProcessLauncher final : public platform::ProcessLauncher,
                                      public project::GitMetadataSource {
 public:
  explicit LoopbackProcessLauncher(LoopbackPathMap map) : map_(std::move(map)) {}

  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override;
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override;
  platform::SubprocessResult Run(std::vector<std::string> argv,
                                 platform::SubprocessOptions options) const override;
  bool is_local() const override { return false; }
  std::string_view description() const override { return "loopback"; }

  project::GitAvailability Availability(const std::filesystem::path& root) const override;
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override;

  // Every spawn this launcher saw, synchronous (Run) or long-lived (ResolveArgv,
  // which a language server, a debug adapter and a terminal resolve through
  // before starting the process themselves), as "program arg…" lines.
  std::vector<std::string> spawns() const;
  std::size_t spawn_count() const;

 private:
  void Record(const std::vector<std::string>& argv) const;

  LoopbackPathMap map_;
  mutable std::mutex mutex_;
  mutable std::vector<std::string> spawns_;
};

// Writes go to the mirror through the local gate and are then replicated to the
// host synchronously: a PERFECT sync engine, so that a parity failure is never the
// stand-in's latency and always a write that did not go through the gate, or a
// path that was not translated. (The real engine's lag, conflicts and journal are
// § 10's mirror tests, not parity's.)
class LoopbackWriteGate final : public project::FileWriteGate {
 public:
  explicit LoopbackWriteGate(LoopbackPathMap map) : map_(std::move(map)) {}

  [[nodiscard]] Result WriteText(const std::filesystem::path& path, std::string_view text,
                                 Signature signature = Signature::Skip) override;
  [[nodiscard]] TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override;
  void DisposeStaged(std::span<const std::filesystem::path> staged) override;

  std::size_t write_count() const { return writes_.load(); }

 private:
  LoopbackPathMap map_;
  std::atomic<std::size_t> writes_{0};
};

}  // namespace microide::tests::parity
