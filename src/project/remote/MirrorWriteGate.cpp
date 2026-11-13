#include "project/remote/MirrorWriteGate.h"

#include "util/TextFileIO.h"

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

std::optional<std::string> MirrorWriteGate::UnsyncableTarget(const std::filesystem::path& path) const {
  const std::string relative = path.lexically_normal().lexically_relative(store_.tree()).generic_string();
  if (relative.empty() || relative == "." || relative == ".." || relative.rfind("../", 0) == 0 ||
      IsSafeRelativePath(relative)) {
    return std::nullopt;
  }
  return "\"" + path.filename().string() + "\" cannot be synced to the host (a remote project's names " +
         "cannot hold a backslash, and each part of a name is at most 255 bytes)";
}

FileWriteGate::Result MirrorWriteGate::WriteText(const std::filesystem::path& path,
                                                 std::string_view text, Signature signature,
                                                 util::WriteTarget target) {
  if (UnsyncableTarget(path).has_value()) {
    // Written here, it would never reach the host: refused, as a rename to such a
    // name is (ApplyTreeOps).
    Result refused;
    refused.error = std::make_error_code(std::errc::invalid_argument);
    return refused;
  }
  const std::optional<std::string> relative = MirrorRelative(path);
  if (!relative.has_value()) {
    return LocalFileWriteGate().WriteText(path, text, signature, target);
  }
  // Where the bytes land: a save through a link writes the file the link names
  // (the atomic write resolves the chain the same way, so the link survives), and
  // that path is the one that changed. Journaling the link's own path pushed the
  // link path, which the host refuses -- a link is there -- and never sent the
  // file that changed (the QA monkey's remote job: soak 517966306bfc). A link
  // out of the tree names a file no host path is: it is written and not synced.
  const std::optional<std::string> written = MirrorRelative(util::ResolveSymlinkTarget(path));
  Result result;
  {
    std::lock_guard path_lock(store_.PathLock(written.value_or(*relative)));
    // The temporary in meta/staging, not beside the file: a crash between writing
    // it and renaming it into place must not leave it in the tree, where nothing
    // would ever remove it (the fault matrix's disk axis found one). Open sweeps
    // staging.
    result = WriteTextLocally(path, text, signature, target, store_.staging());
  }
  if (result.ok && written.has_value()) {
    engine_.NotifyLocalWrite(*written);
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
  // A target inside the tree under a name the host cannot be sent is refused
  // before anything is applied. A rename to one read as a move out of the
  // project: the host deleted the folder while the mirror kept it under its new
  // name, which never synced (the QA monkey's remote job: soak f3ad7063ff9a, a
  // folder renamed to text with a backslash in it).
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const TreeOp& op = ops[index];
    const bool to_new_path = op.kind == TreeOp::Kind::Rename || op.kind == TreeOp::Kind::Copy;
    const bool creates = op.kind == TreeOp::Kind::CreateFile || op.kind == TreeOp::Kind::CreateDirectory;
    if (!to_new_path && !creates) {
      continue;
    }
    if (std::optional<std::string> why = UnsyncableTarget(to_new_path ? op.new_path : op.path)) {
      TreeResult refused;
      refused.failed_index = index;
      refused.error_message = std::move(*why);
      return refused;
    }
  }
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
