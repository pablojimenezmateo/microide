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

class MirrorWriteGate final : public FileWriteGate {
 public:
  MirrorWriteGate(MirrorStore& store, MirrorSyncEngine& engine) : store_(store), engine_(engine) {}

  [[nodiscard]] Result WriteText(const std::filesystem::path& path, std::string_view text,
                                 Signature signature = Signature::Skip,
                                 util::WriteTarget target = util::WriteTarget::Replace) override;
  [[nodiscard]] TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override;
  void DisposeStaged(std::span<const std::filesystem::path> staged) override;

  std::optional<std::string> MirrorRelative(const std::filesystem::path& path) const;
  std::optional<std::string> HostRelative(const std::filesystem::path& path) const;
  std::optional<std::string> UnsyncableTarget(const std::filesystem::path& path) const;

 private:
  MirrorStore& store_;
  MirrorSyncEngine& engine_;
};

}
