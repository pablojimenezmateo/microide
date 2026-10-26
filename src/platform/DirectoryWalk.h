#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "platform/Filesystem.h"

namespace microide::platform {

// One entry of a WalkDirectoryTree walk. Everything it points at is the walk's
// own storage and valid only for the duration of the visit.
struct WalkEntry {
  // The entry's full path, spelled as the root was plus "/name" per level, so a
  // normal root gives normal entries. NUL-terminated: `path.data()` can be handed
  // to a system call as is.
  std::string_view path;
  std::string_view name;
  // With symlinks followed, as `std::filesystem::is_directory` would answer: a
  // link to a directory is a Directory, a dangling link is Missing.
  PathType type = PathType::Other;
  bool is_symlink = false;
  // 1 for the root's own entries.
  int depth = 1;
};

enum class WalkAction {
  Continue,  // a directory is entered (unless it is a symlink the options do not follow)
  Prune,     // a directory is not entered; meaningless for anything else
  Stop,      // the walk ends here
};

class TreeWalkVisitor {
 public:
  virtual ~TreeWalkVisitor() = default;
  virtual WalkAction Visit(const WalkEntry& entry) = 0;
  // Called exactly once for every directory Visit let the walk enter (Continue,
  // and not a symlink the options leave alone), once the walk is done with it --
  // including when it then could not open it or was past max_depth. That pairing
  // is what lets a visitor scope per-directory state to the subtree (a symlink
  // loop guard, a matcher chain) with a stack.
  virtual void Leave(const WalkEntry& directory) { (void) directory; }
};

struct WalkOptions {
  // Descend into a symlink whose target is a directory. Off by default, as for
  // `recursive_directory_iterator`; a walk that turns it on guards against cycles
  // itself (SymlinkLoopGuard, through Visit/Leave).
  bool follow_directory_symlinks = false;
  // Directories deeper than this are not entered (the walk holds one descriptor
  // per level, and a recursive walker one stack frame per level).
  int max_depth = kMaxTreeWalkDepth;
};

struct WalkStatus {
  // The root could not be opened as a directory; nothing was visited.
  bool root_unreadable = false;
  // A directory Visit let the walk enter could not be opened for want of
  // permission. Its subtree is missing from the walk.
  bool permission_limited = false;
  // A directory could not be opened or read for any other reason (it vanished,
  // the descriptor table is full). Its subtree is missing, or partly so.
  bool incomplete = false;
  bool truncated_by_depth = false;
  // A Visit returned Stop.
  bool stopped = false;
  // The first error behind root_unreadable or incomplete, for a message.
  std::error_code error;
};

// Depth-first, pre-order (a directory is visited before what is in it), in the
// order the filesystem lists each directory. One `readdir` per entry and no stat
// unless the filesystem did not report the type or the entry is a symlink; no
// allocation per entry -- the path is one reused buffer.
//
// It replaces `recursive_directory_iterator` in every whole-tree walk: that one
// builds a `std::filesystem::path` per entry (two allocations each in libstdc++),
// ~1.7 us an entry against ~0.66 us here (TD-2026-08-17-257), and six walkers each
// repeated the same prune, type and error handling around it.
WalkStatus WalkDirectoryTree(const std::filesystem::path& root, const WalkOptions& options,
                             TreeWalkVisitor& visitor);

// The adapter for a visitor that is one lambda, `WalkAction(const WalkEntry&)`.
template <typename Fn>
  requires(!std::is_base_of_v<TreeWalkVisitor, std::remove_cvref_t<Fn>>)
WalkStatus WalkDirectoryTree(const std::filesystem::path& root, const WalkOptions& options,
                             Fn&& fn) {
  struct Adapter final : TreeWalkVisitor {
    explicit Adapter(Fn& visit) : visit_(visit) {}
    WalkAction Visit(const WalkEntry& entry) override { return visit_(entry); }
    Fn& visit_;
  } adapter(fn);
  return WalkDirectoryTree(root, options, static_cast<TreeWalkVisitor&>(adapter));
}

}  // namespace microide::platform
