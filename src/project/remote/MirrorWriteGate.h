#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "project/FileWriteGate.h"

namespace microide::project::remote {

class MirrorStore;
class MirrorSyncEngine;

// The write gate of a remote project (dev-docs/design/remote-projects.md § 6.3):
// every writer in the editor — the save, the plugin file API, replace-in-project,
// the merge writer, the LSP's resource ops, the sidebar — lands here, and here
// alone decides that a write into the mirror also reaches the host.
//
// A write lands on local disk first, at local speed and under the path's lock (so
// a pull of the same path cannot interleave), and then is journaled and pushed by
// the sync engine under compare-and-swap. The caller never waits on the network.
// Writes outside the mirror's tree/ are local writes and nothing more.
class MirrorWriteGate final : public FileWriteGate {
 public:
  MirrorWriteGate(MirrorStore& store, MirrorSyncEngine& engine) : store_(store), engine_(engine) {}

  [[nodiscard]] Result WriteText(const std::filesystem::path& path, std::string_view text,
                                 Signature signature = Signature::Skip) override;
  [[nodiscard]] TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override;
  void DisposeStaged(std::span<const std::filesystem::path> staged) override;

  // `path` as a path relative to tree/, or nullopt when it is not under it.
  std::optional<std::string> MirrorRelative(const std::filesystem::path& path) const;

 private:
  MirrorStore& store_;
  MirrorSyncEngine& engine_;
};

}  // namespace microide::project::remote
