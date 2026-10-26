#include "platform/DirectoryWalk.h"

#include <cerrno>
#include <string>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::platform {

namespace {

PathType PathTypeFromMode(mode_t mode) {
  if (S_ISDIR(mode)) {
    return PathType::Directory;
  }
  if (S_ISREG(mode)) {
    return PathType::RegularFile;
  }
  return PathType::Other;
}

// The entry's type and whether it is a link, from readdir's `d_type` when the
// filesystem reported one; a stat (relative to the open directory) otherwise, and
// for a link, whose type is its target's.
void ClassifyEntry(int directory_fd, const dirent& entry, PathType& type, bool& is_symlink) {
  is_symlink = false;
  switch (entry.d_type) {
    case DT_DIR:
      type = PathType::Directory;
      return;
    case DT_REG:
      type = PathType::RegularFile;
      return;
    case DT_LNK:
      is_symlink = true;
      break;
    case DT_UNKNOWN: {
      struct ::stat link_status {};
      if (::fstatat(directory_fd, entry.d_name, &link_status, AT_SYMLINK_NOFOLLOW) != 0) {
        type = PathType::Missing;
        return;
      }
      if (!S_ISLNK(link_status.st_mode)) {
        type = PathTypeFromMode(link_status.st_mode);
        return;
      }
      is_symlink = true;
      break;
    }
    default:
      type = PathType::Other;
      return;
  }
  struct ::stat target_status {};
  type = ::fstatat(directory_fd, entry.d_name, &target_status, 0) == 0
             ? PathTypeFromMode(target_status.st_mode)
             : PathType::Missing;
}

class Walker {
 public:
  Walker(const WalkOptions& options, TreeWalkVisitor& visitor, std::string root)
      : options_(options), visitor_(visitor), path_(std::move(root)) {}

  WalkStatus Run() {
    const int root_fd = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
      status_.root_unreadable = true;
      NoteError(errno);
      return status_;
    }
    WalkDirectory(root_fd, 1);
    return status_;
  }

 private:
  // Takes ownership of `directory_fd`. Returns false when the walk must stop.
  bool WalkDirectory(int directory_fd, int depth) {
    DIR* const directory = ::fdopendir(directory_fd);
    if (directory == nullptr) {
      NoteError(errno);
      ::close(directory_fd);
      status_.incomplete = true;
      return true;
    }
    const std::size_t parent_length = path_.size();
    bool keep_walking = true;
    while (keep_walking) {
      errno = 0;
      const dirent* const entry = ::readdir(directory);
      if (entry == nullptr) {
        if (errno != 0) {
          NoteError(errno);
          status_.incomplete = true;
        }
        break;
      }
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      WalkEntry walk_entry;
      ClassifyEntry(::dirfd(directory), *entry, walk_entry.type, walk_entry.is_symlink);
      if (path_.empty() || path_.back() != '/') {
        path_.push_back('/');
      }
      path_.append(name);
      walk_entry.path = path_;
      walk_entry.name = std::string_view(path_).substr(path_.size() - name.size());
      walk_entry.depth = depth;

      const WalkAction action = visitor_.Visit(walk_entry);
      if (action == WalkAction::Stop) {
        status_.stopped = true;
        keep_walking = false;
      } else if (action == WalkAction::Continue && walk_entry.type == PathType::Directory &&
                 (!walk_entry.is_symlink || options_.follow_directory_symlinks)) {
        keep_walking = Descend(::dirfd(directory), depth, walk_entry.name.size());
        // The buffer holds this entry's path again once the subtree is done with
        // it, but the subtree's longer paths may have moved it: re-point the views.
        walk_entry.path = path_;
        walk_entry.name = std::string_view(path_).substr(path_.size() - name.size());
        visitor_.Leave(walk_entry);
      }
      path_.resize(parent_length);
    }
    ::closedir(directory);
    return keep_walking;
  }

  // Walks the directory whose path is in the buffer, `name_length` its last
  // component. Returns false when the walk must stop.
  bool Descend(int parent_fd, int depth, std::size_t name_length) {
    if (depth + 1 > options_.max_depth) {
      status_.truncated_by_depth = true;
      return true;
    }
    // The name is the buffer's tail, so it is NUL-terminated there.
    const int child_fd = ::openat(parent_fd, path_.c_str() + (path_.size() - name_length),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (child_fd < 0) {
      if (errno == EACCES || errno == EPERM) {
        status_.permission_limited = true;
      } else {
        NoteError(errno);
        status_.incomplete = true;
      }
      return true;
    }
    return WalkDirectory(child_fd, depth + 1);
  }

  void NoteError(int error_number) {
    if (!status_.error) {
      status_.error = std::error_code(error_number, std::generic_category());
    }
  }

  const WalkOptions& options_;
  TreeWalkVisitor& visitor_;
  std::string path_;
  WalkStatus status_;
};

}  // namespace

WalkStatus WalkDirectoryTree(const std::filesystem::path& root, const WalkOptions& options,
                             TreeWalkVisitor& visitor) {
  return Walker(options, visitor, root.native()).Run();
}

}  // namespace microide::platform
