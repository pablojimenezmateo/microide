#pragma once

#include <filesystem>
#include <shared_mutex>
#include <span>
#include <vector>

#include "project/FileWriteGate.h"

namespace microide::project::remote {

// The write gate of a LOCAL project while remote projects are open: a write whose
// path is in an open remote project's mirror goes through that project's gate, so
// it is journaled and pushed like the remote project's own saves; anything else is
// a local write.
//
// A gate belongs to the path, not to the project in front (the plugin API's
// rule, PluginFsContext::WriteGateFor). A mirror file opened while a local
// project was active -- dropped on the window, opened by its path -- was saved
// through the local gate: the mirror changed, the sync was never told and read
// settled, and the host kept the old text (soak 500d49cd7220).
//
// Thread-safe: the editor's save runs on the shell thread, replace-in-project and
// the closed-file edits on workers. A routed call holds a shared lock for its
// whole length and Unregister takes it exclusively, so a mirror's gate is never
// destroyed under a write that resolved to it.
class MirrorRoutingWriteGate final : public FileWriteGate {
 public:
  // `tree` is the mirror's tree/; `gate` its project's gate, alive until Unregister.
  void Register(const std::filesystem::path& tree, FileWriteGate& gate);
  void Unregister(const std::filesystem::path& tree);

  [[nodiscard]] Result WriteText(const std::filesystem::path& path, std::string_view text,
                                 Signature signature = Signature::Skip,
                                 util::WriteTarget target = util::WriteTarget::Replace) override;
  [[nodiscard]] TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override;
  void DisposeStaged(std::span<const std::filesystem::path> staged) override;
  [[nodiscard]] bool IsReadOnly(const std::filesystem::path& path) const override;
  [[nodiscard]] bool MakeWritable(const std::filesystem::path& path) override;

 private:
  struct Mirror {
    std::filesystem::path tree;
    FileWriteGate* gate = nullptr;
  };
  // The gate for `path`: the deepest registered tree holding it, else the local
  // gate. Under the shared lock.
  FileWriteGate& GateForLocked(const std::filesystem::path& path) const;

  mutable std::shared_mutex mutex_;
  std::vector<Mirror> mirrors_;
};

}  // namespace microide::project::remote
