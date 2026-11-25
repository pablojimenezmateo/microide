#pragma once

#include <filesystem>
#include <shared_mutex>
#include <span>
#include <vector>

#include "project/FileWriteGate.h"

namespace microide::project::remote {

class MirrorRoutingWriteGate final : public FileWriteGate {
 public:
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
  FileWriteGate& GateForLocked(const std::filesystem::path& path) const;

  mutable std::shared_mutex mutex_;
  std::vector<Mirror> mirrors_;
};

}
