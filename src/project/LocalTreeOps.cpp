#include "project/LocalTreeOps.h"

#include <cerrno>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/FsOps.h"
#include "platform/Trash.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace microide::project {

namespace {

namespace fs = std::filesystem;
using TreeOp = FileWriteGate::TreeOp;
using TreeResult = FileWriteGate::TreeResult;

// Atomically create `path` failing if it already exists. Returns 1 on success, 0
// if the file already existed (EEXIST), -1 on any other error. O_CREAT|O_EXCL is
// a single kernel operation, so unlike an exists() probe followed by a truncating
// open it cannot clobber a file another process creates in the meantime.
int ExclusiveCreateEmptyFile(const fs::path& path) {
#ifdef _WIN32
  int fd = -1;
  const errno_t open_error = _wsopen_s(&fd, path.c_str(),
                                       _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
                                       _SH_DENYNO, _S_IREAD | _S_IWRITE);
  if (open_error != 0 || fd < 0) {
    return (open_error == EEXIST) ? 0 : -1;
  }
  _close(fd);
  return 1;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    return (errno == EEXIST) ? 0 : -1;
  }
  ::close(fd);
  return 1;
#endif
}

// Presence of the directory entry itself, without following a symlink. exists()
// dereferences the link, so a dangling symlink (a real, renameable entry) reports
// as absent (TD-2026-07-17A-125). A stat error also counts as "present" so
// rename/no-overwrite decisions fail closed rather than clobbering.
bool NodeExists(const fs::path& path) {
  std::error_code error;
  return fs::symlink_status(path, error).type() != fs::file_type::not_found;
}

bool IsReservedPathComponent(const fs::path& path) {
  for (const auto& component : path) {
    const std::string name = component.string();
    if (name == "." || name == "..") {
      return true;
    }
  }
  return false;
}

// One applied mutation's inverse. Rolled back in reverse order.
struct Undo {
  enum class Kind : std::uint8_t {
    RemoveCreatedFile,  // a: the file this batch created
    RemoveCreatedDirs,  // a: the TOPMOST directory this batch created
    RenameBack,         // a: current (new) path, b: original path
    RestoreStaged,      // a: original path, b: where it was staged
    RecreateDirectory,  // a: an empty directory this batch removed
    Untrash,            // a: original path, b: its place in the trash
  };
  Kind kind;
  fs::path a;
  fs::path b;
};

class Batch {
 public:
  TreeResult Apply(std::span<const TreeOp> ops) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
      std::string failure = ApplyOne(ops[i]);
      if (!failure.empty()) {
        Rollback();
        // Anything staged by the undone ops was restored by the rollback.
        return TreeResult{.ok = false,
                          .failed_index = i,
                          .error_message = std::move(failure),
                          .resulting_path = {},
                          .staged_directories = {}};
      }
    }
    TreeResult result{.ok = true,
                      .failed_index = 0,
                      .error_message = {},
                      .resulting_path = std::move(resulting_path_),
                      .staged_directories = {}};
    // The batch landed: dispose of what it staged. A staged FILE is one unlink,
    // done here so the hidden staging entry can never reach the file index; a
    // staged DIRECTORY is handed back for the caller to remove off-thread.
    for (fs::path& staged : staged_) {
      std::error_code error;
      if (fs::is_directory(fs::symlink_status(staged, error))) {
        result.staged_directories.push_back(std::move(staged));
        continue;
      }
      fs::remove(staged, error);
    }
    return result;
  }

 private:
  std::string ApplyOne(const TreeOp& op) {
    const fs::path path = platform::NormalizeAbsolutePath(op.path);
    if (path.empty()) {
      return "No path was provided";
    }
    switch (op.kind) {
      case TreeOp::Kind::CreateFile:
        return CreateFile(path, op.overwrite);
      case TreeOp::Kind::CreateDirectory:
        return CreateDirectory(path);
      case TreeOp::Kind::Rename:
        return Rename(path, platform::NormalizeAbsolutePath(op.new_path), op.overwrite);
      case TreeOp::Kind::Delete:
        return Delete(path, op.recursive);
      case TreeOp::Kind::Trash:
        return Trash(path);
    }
    return "Unknown file operation";
  }

  std::string CreateFile(const fs::path& path, const bool overwrite) {
    if (IsReservedPathComponent(path.filename())) {
      return "Invalid file name";
    }
    if (!EnsureParentDirectories(path)) {
      return "Failed to create the parent directory";
    }
    if (overwrite && NodeExists(path)) {
      if (!StageAside(path)) {
        return "Could not set aside the existing " + path.filename().string();
      }
    }
    // Exclusive create: never truncate an existing file. A plain exists()-then-open
    // sequence would silently overwrite a file that a racing process created
    // between the two steps.
    const int created = ExclusiveCreateEmptyFile(path);
    if (created == 0) {
      return "The file already exists";
    }
    if (created < 0) {
      return "Failed to create the file";
    }
    undo_.push_back({Undo::Kind::RemoveCreatedFile, path, {}});
    resulting_path_ = path;
    return {};
  }

  std::string CreateDirectory(const fs::path& path) {
    if (IsReservedPathComponent(path.filename())) {
      return "Invalid directory name";
    }
    const fs::path topmost = TopmostMissingAncestorOrSelf(path);
    // Create-first, then classify. A prior exists() probe followed by
    // create_directories is a TOCTOU: a racing process can drop a non-directory at
    // the target in the window between the two calls. create_directories is the
    // authoritative step — it returns false without error when the path already
    // exists — so "already exists" is told from a real failure by status.
    std::error_code error;
    if (fs::create_directories(path, error)) {
      undo_.push_back({Undo::Kind::RemoveCreatedDirs, topmost, {}});
      resulting_path_ = path;
      return {};
    }
    std::error_code status_error;
    const fs::file_status status = fs::status(path, status_error);
    if (!status_error && fs::is_directory(status)) {
      return "The directory already exists";
    }
    if (!status_error && fs::exists(status)) {
      return "A non-directory already exists at that path";
    }
    return "Failed to create the directory";
  }

  std::string Rename(const fs::path& source, const fs::path& destination, const bool overwrite) {
    if (destination.empty()) {
      return "A source and destination path are required";
    }
    if (source == destination) {
      return "The new path matches the current path";
    }
    if (IsReservedPathComponent(destination.filename())) {
      return "Invalid destination name";
    }
    if (!NodeExists(source)) {
      return "The source path does not exist";
    }
    if (NodeExists(destination)) {
      if (!overwrite) {
        // Kept for a clear early message; the move below is no-overwrite anyway.
        return "The destination path already exists";
      }
      if (!StageAside(destination)) {
        return "Could not set aside the existing " + destination.filename().string();
      }
    }
    if (!EnsureParentDirectories(destination)) {
      return "Failed to prepare the destination directory";
    }
    // No-overwrite and atomic (renameat2 RENAME_NOREPLACE) on one filesystem, so a
    // destination racing into existence after the check above cannot be clobbered.
    if (!platform::MovePathNoOverwrite(source, destination)) {
      return NodeExists(destination) ? "The destination path already exists"
                                     : "Failed to rename the path";
    }
    undo_.push_back({Undo::Kind::RenameBack, destination, source});
    resulting_path_ = destination;
    return {};
  }

  std::string Delete(const fs::path& path, const bool recursive) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
      return "The path does not exist";
    }
    if (fs::is_directory(status) && !recursive) {
      // rmdir: the kernel refuses a non-empty directory, so content that appears
      // after any emptiness check the caller made is never taken with it.
      if (!fs::remove(path, error) || error) {
        return "The directory is not empty";
      }
      undo_.push_back({Undo::Kind::RecreateDirectory, path, {}});
      resulting_path_ = path;
      return {};
    }
    if (!StageAside(path)) {
      return "Could not delete " + path.filename().string();
    }
    resulting_path_ = path;
    return {};
  }

  std::string Trash(const fs::path& path) {
    const platform::TrashOperationResult trashed = platform::MovePathToTrash(path);
    if (!trashed.ok) {
      return trashed.error_message.empty() ? "Failed to move the path to the trash"
                                           : trashed.error_message;
    }
    undo_.push_back({Undo::Kind::Untrash, path, trashed.resulting_path});
    resulting_path_ = trashed.resulting_path;
    return {};
  }

  // Rename `victim` to a hidden sibling — same directory, so a pure rename that
  // stays restorable until the batch lands.
  bool StageAside(const fs::path& victim) {
    for (;;) {
      const fs::path staged = victim.parent_path() /
                              (".microide-staged-" + std::to_string(stage_seq_++) + "-" +
                               victim.filename().string());
      if (NodeExists(staged)) {
        continue;  // leftover debris from an interrupted run; try the next name
      }
      if (!platform::MovePathNoOverwrite(victim, staged)) {
        return false;
      }
      undo_.push_back({Undo::Kind::RestoreStaged, victim, staged});
      staged_.push_back(staged);
      return true;
    }
  }

  static fs::path TopmostMissingAncestorOrSelf(const fs::path& path) {
    fs::path topmost = path;
    while (!topmost.parent_path().empty() && topmost.parent_path() != topmost &&
           !NodeExists(topmost.parent_path())) {
      topmost = topmost.parent_path();
    }
    return topmost;
  }

  // Create `target`'s missing parents, journalling the TOPMOST one created so a
  // rollback removes the whole new chain rather than just the leaf.
  bool EnsureParentDirectories(const fs::path& target) {
    const fs::path parent = target.parent_path();
    if (parent.empty() || NodeExists(parent)) {
      return !parent.empty();
    }
    const fs::path topmost = TopmostMissingAncestorOrSelf(parent);
    std::error_code error;
    fs::create_directories(parent, error);
    if (error) {
      return false;
    }
    undo_.push_back({Undo::Kind::RemoveCreatedDirs, topmost, {}});
    return true;
  }

  void Rollback() {
    for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
      std::error_code error;
      switch (it->kind) {
        case Undo::Kind::RemoveCreatedFile:
          fs::remove(it->a, error);
          break;
        case Undo::Kind::RemoveCreatedDirs:
          fs::remove_all(it->a, error);
          break;
        case Undo::Kind::RenameBack:
          (void)platform::MovePathNoOverwrite(it->a, it->b);
          break;
        case Undo::Kind::RestoreStaged:
          (void)platform::MovePathNoOverwrite(it->b, it->a);
          break;
        case Undo::Kind::RecreateDirectory:
          fs::create_directory(it->a, error);
          break;
        case Undo::Kind::Untrash:
          // The trash's .trashinfo stays behind; a stale info file is harmless and
          // the user's file is back where it was, which is what matters.
          (void)platform::MovePathNoOverwrite(it->b, it->a);
          break;
      }
    }
    undo_.clear();
    staged_.clear();
  }

  std::vector<Undo> undo_;
  std::vector<fs::path> staged_;
  std::size_t stage_seq_ = 0;
  fs::path resulting_path_;
};

}  // namespace

TreeResult ApplyLocalTreeOps(std::span<const TreeOp> ops) {
  return Batch{}.Apply(ops);
}

}  // namespace microide::project
