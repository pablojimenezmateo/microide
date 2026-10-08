#include "project/remote/MirrorWriteGate.h"

#include <mutex>
#include <vector>

#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteManifest.h"

namespace microide::project::remote {

std::optional<std::string> MirrorWriteGate::MirrorRelative(const std::filesystem::path& path) const {
  const std::filesystem::path relative = path.lexically_normal().lexically_relative(store_.tree());
  const std::string text = relative.generic_string();
  if (text.empty() || text == "." || !IsSafeRelativePath(text)) {
    return std::nullopt;
  }
  return text;
}

FileWriteGate::Result MirrorWriteGate::WriteText(const std::filesystem::path& path,
                                                 std::string_view text, Signature signature) {
  const std::optional<std::string> relative = MirrorRelative(path);
  if (!relative.has_value()) {
    return LocalFileWriteGate().WriteText(path, text, signature);
  }
  Result result;
  {
    std::lock_guard path_lock(store_.PathLock(*relative));
    result = LocalFileWriteGate().WriteText(path, text, signature);
  }
  if (result.ok) {
    engine_.NotifyLocalWrite(*relative);
  }
  return result;
}

FileWriteGate::TreeResult MirrorWriteGate::ApplyTreeOps(std::span<const TreeOp> ops) {
  TreeResult result = LocalFileWriteGate().ApplyTreeOps(ops);
  if (!result.ok) {
    return result;  // all or nothing: undone locally, so nothing to replay
  }
  std::vector<MirrorSyncEngine::LocalTreeOp> replay;
  for (const TreeOp& op : ops) {
    const std::optional<std::string> from = MirrorRelative(op.path);
    if (!from.has_value()) {
      continue;
    }
    using Kind = MirrorSyncEngine::LocalTreeOp::Kind;
    switch (op.kind) {
      case TreeOp::Kind::CreateFile:
        replay.push_back({Kind::CreateFile, *from, {}});
        break;
      case TreeOp::Kind::CreateDirectory:
        replay.push_back({Kind::CreateDirectory, *from, {}});
        break;
      case TreeOp::Kind::Rename:
        if (const std::optional<std::string> to = MirrorRelative(op.new_path)) {
          replay.push_back({Kind::Rename, *from, *to});
        } else {
          replay.push_back({Kind::Delete, *from, {}});  // moved out of the project
        }
        break;
      case TreeOp::Kind::Delete:
      case TreeOp::Kind::Trash:
        // The host has no desktop trash to restore from; the local trash keeps the
        // user's copy, and the host's goes.
        replay.push_back({Kind::Delete, *from, {}});
        break;
    }
  }
  if (!replay.empty()) {
    engine_.NotifyLocalTreeOps(std::move(replay));
  }
  return result;
}

void MirrorWriteGate::DisposeStaged(std::span<const std::filesystem::path> staged) {
  LocalFileWriteGate().DisposeStaged(staged);
}

}  // namespace microide::project::remote
