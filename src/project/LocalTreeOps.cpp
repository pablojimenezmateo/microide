#include "project/LocalTreeOps.h"

#include <cerrno>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/FsOps.h"
#include "platform/Trash.h"
#include "util/PathMatch.h"

#include <fcntl.h>
#include <unistd.h>

namespace microide::project {

namespace {

namespace fs = std::filesystem;
using TreeOp = FileWriteGate::TreeOp;
using TreeResult = FileWriteGate::TreeResult;

int ExclusiveCreateEmptyFile(const fs::path& path) {
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    return (errno == EEXIST) ? 0 : -1;
  }
  ::close(fd);
  return 1;
}

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

struct Undo {
  enum class Kind : std::uint8_t {
    RemoveCreatedFile,
    RemoveCreatedDirs,
    RenameBack,
    RestoreStaged,
    RecreateDirectory,
    Untrash,
  };
  Kind kind;
  fs::path a;
  fs::path b;
};

class Batch {
 public:
  TreeResult Apply(std::span<const TreeOp> ops) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
      if (ops[i].path.native().find('\0') != std::string::npos ||
          ops[i].new_path.native().find('\0') != std::string::npos) {
        return TreeResult{.ok = false,
                          .failed_index = i,
                          .error_message = "a file name cannot contain a NUL character",
                          .resulting_path = {},
                          .staged_directories = {}};
      }
    }
    for (std::size_t i = 0; i < ops.size(); ++i) {
      std::string failure = ApplyOne(ops[i]);
      if (!failure.empty()) {
        Rollback();
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
      case TreeOp::Kind::Copy:
        return Copy(path, platform::NormalizeAbsolutePath(op.new_path));
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
        return "The destination path already exists";
      }
      if (!StageAside(destination)) {
        return "Could not set aside the existing " + destination.filename().string();
      }
    }
    if (!EnsureParentDirectories(destination)) {
      return "Failed to prepare the destination directory";
    }
    if (!platform::MovePathNoOverwrite(source, destination)) {
      return NodeExists(destination) ? "The destination path already exists"
                                     : "Failed to rename the path";
    }
    undo_.push_back({Undo::Kind::RenameBack, destination, source});
    resulting_path_ = destination;
    return {};
  }

  std::string Copy(const fs::path& source, const fs::path& destination) {
    if (destination.empty()) {
      return "A source and destination path are required";
    }
    if (IsReservedPathComponent(destination.filename())) {
      return "Invalid destination name";
    }
    std::error_code error;
    const fs::file_status status = fs::symlink_status(source, error);
    if (status.type() == fs::file_type::not_found) {
      return "The source path does not exist";
    }
    const fs::path relative = destination.lexically_relative(source);
    if (fs::is_directory(status) && !relative.empty() && !util::RelativePathClimbsOut(relative)) {
      return "A folder cannot be copied into itself";
    }
    if (NodeExists(destination)) {
      return "The destination path already exists";
    }
    if (!EnsureParentDirectories(destination)) {
      return "Failed to prepare the destination directory";
    }
    if (fs::is_directory(status)) {
      fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
    } else if (fs::is_symlink(status)) {
      fs::copy_symlink(source, destination, error);
    } else {
      fs::copy_file(source, destination, fs::copy_options::none, error);
    }
    if (error) {
      std::error_code cleanup;
      if (NodeExists(destination)) {
        fs::remove_all(destination, cleanup);
      }
      return "Failed to copy " + source.filename().string();
    }
    undo_.push_back({Undo::Kind::RemoveCreatedDirs, destination, {}});
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

  bool StageAside(const fs::path& victim) {
    for (;;) {
      const fs::path staged = victim.parent_path() /
                              (".microide-staged-" + std::to_string(stage_seq_++) + "-" +
                               victim.filename().string());
      if (NodeExists(staged)) {
        continue;
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

}

TreeResult ApplyLocalTreeOps(std::span<const TreeOp> ops) {
  return Batch{}.Apply(ops);
}

}
