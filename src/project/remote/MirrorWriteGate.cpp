#include "project/remote/MirrorWriteGate.h"

#include "util/TextFileIO.h"

#include <mutex>
#include <vector>

#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteManifest.h"

namespace microide::project::remote {

namespace {

std::optional<std::string> RealTreeRelative(const std::filesystem::path& tree, const std::filesystem::path& path) {
  std::error_code path_error;
  std::error_code tree_error;
  const std::filesystem::path real = std::filesystem::weakly_canonical(util::ResolveSymlinkTarget(path), path_error);
  const std::filesystem::path real_tree = std::filesystem::weakly_canonical(tree, tree_error);
  if (path_error || tree_error) {
    return std::nullopt;
  }
  std::string text = real.lexically_relative(real_tree).generic_string();
  if (text.empty() || text == "." || !IsSafeRelativePath(text)) {
    return std::nullopt;
  }
  return text;
}

}

std::optional<std::string> MirrorWriteGate::MirrorRelative(const std::filesystem::path& path) const {
  const std::filesystem::path relative = path.lexically_normal().lexically_relative(store_.tree());
  std::string text = relative.generic_string();
  if (text.empty() || text == "." || !IsSafeRelativePath(text)) {
    return std::nullopt;
  }
  return text;
}

std::optional<std::string> MirrorWriteGate::HostRelative(const std::filesystem::path& path) const {
  const std::filesystem::path normal = path.lexically_normal();
  if (!normal.has_filename() || normal.filename() == "..") {
    return std::nullopt;
  }
  std::error_code parent_error;
  std::error_code tree_error;
  const std::filesystem::path real_parent = std::filesystem::weakly_canonical(normal.parent_path(), parent_error);
  const std::filesystem::path real_tree = std::filesystem::weakly_canonical(store_.tree(), tree_error);
  if (parent_error || tree_error) {
    return std::nullopt;
  }
  std::string text = (real_parent / normal.filename()).lexically_relative(real_tree).generic_string();
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
    Result refused;
    refused.error = std::make_error_code(std::errc::invalid_argument);
    return refused;
  }
  const std::optional<std::string> relative = MirrorRelative(path);
  if (!relative.has_value()) {
    return LocalFileWriteGate().WriteText(path, text, signature, target);
  }
  const std::optional<std::string> written = RealTreeRelative(store_.tree(), path);
  Result result;
  {
    std::lock_guard path_lock(store_.PathLock(written.value_or(*relative)));
    result = WriteTextLocally(path, text, signature, target, store_.staging());
  }
  if (result.ok && written.has_value()) {
    engine_.NotifyLocalWrite(*written);
  }
  return result;
}

namespace {

void ReplayCopiedTree(const MirrorWriteGate& gate, MirrorSyncEngine& engine, const std::filesystem::path& root,
                      std::vector<MirrorSyncEngine::LocalTreeOp>& replay) {
  using Kind = MirrorSyncEngine::LocalTreeOp::Kind;
  std::error_code error;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error))) {
    if (const std::optional<std::string> relative = gate.HostRelative(root)) {
      engine.NotifyLocalWrite(*relative);
    }
    return;
  }
  if (const std::optional<std::string> relative = gate.HostRelative(root)) {
    replay.push_back({Kind::CreateDirectory, *relative, {}});
  }
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::optional<std::string> relative = gate.HostRelative(it->path());
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

}

FileWriteGate::TreeResult MirrorWriteGate::ApplyTreeOps(std::span<const TreeOp> ops) {
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
  std::vector<std::optional<std::string>> host_from(ops.size());
  std::vector<std::optional<std::string>> host_to(ops.size());
  for (std::size_t index = 0; index < ops.size(); ++index) {
    host_from[index] = HostRelative(ops[index].path);
    if (ops[index].kind == TreeOp::Kind::Rename) {
      host_to[index] = HostRelative(ops[index].new_path);
    }
  }
  TreeResult result = LocalFileWriteGate().ApplyTreeOps(ops);
  if (!result.ok) {
    return result;
  }
  std::vector<MirrorSyncEngine::LocalTreeOp> replay;
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const TreeOp& op = ops[index];
    const std::optional<std::string>& from = host_from[index];
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
        if (const std::optional<std::string>& to = host_to[index]) {
          replay.push_back({Kind::Rename, *from, *to});
        } else {
          replay.push_back({Kind::Delete, *from, {}});
        }
        break;
      case TreeOp::Kind::Delete:
      case TreeOp::Kind::Trash:
        replay.push_back({Kind::Delete, *from, {}});
        break;
      case TreeOp::Kind::Copy:
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

}
