#include "project/remote/MirrorWriteGate.h"

#include <mutex>
#include <vector>

#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteManifest.h"

namespace microide::project::remote {

std::optional<std::string> MirrorWriteGate::MirrorRelative(const std::filesystem::path& path) const {
  const std::filesystem::path relative = path.lexically_normal().lexically_relative(store_.tree());
  std::string text = relative.generic_string();
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

namespace {

// A Copy's new tree, replayed to the host: its folders created, its files pushed
// as local writes.
void ReplayCopiedTree(const MirrorWriteGate& gate, MirrorSyncEngine& engine, const std::filesystem::path& root,
                      std::vector<MirrorSyncEngine::LocalTreeOp>& replay) {
  using Kind = MirrorSyncEngine::LocalTreeOp::Kind;
  std::error_code error;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error))) {
    if (const std::optional<std::string> relative = gate.MirrorRelative(root)) {
      engine.NotifyLocalWrite(*relative);
    }
    return;
  }
  if (const std::optional<std::string> relative = gate.MirrorRelative(root)) {
    replay.push_back({Kind::CreateDirectory, *relative, {}});
  }
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::optional<std::string> relative = gate.MirrorRelative(it->path());
    if (!relative.has_value()) {
      continue;
    }
    if (it->is_directory(error) && !it->is_symlink(error)) {
      replay.push_back({Kind::CreateDirectory, *relative, {}});
    } else {
      engine.NotifyLocalWrite(*relative);
    }
  }
}

}  // namespace

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
      case TreeOp::Kind::Copy:
        // The copy is new local content: its folders are created on the host and
        // its files pushed like any written file.
        ReplayCopiedTree(*this, engine_, op.new_path, replay);
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
