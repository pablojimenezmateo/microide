#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "platform/Filesystem.h"

namespace microide::platform {

struct WalkEntry {
  std::string_view path;
  std::string_view name;
  PathType type = PathType::Other;
  bool is_symlink = false;
  int depth = 1;
};

enum class WalkAction {
  Continue,
  Prune,
  Stop,
};

class TreeWalkVisitor {
 public:
  virtual ~TreeWalkVisitor() = default;
  virtual WalkAction Visit(const WalkEntry& entry) = 0;
  virtual void Leave(const WalkEntry& directory) { (void) directory; }
};

struct WalkOptions {
  bool follow_directory_symlinks = false;
  int max_depth = kMaxTreeWalkDepth;
};

struct WalkStatus {
  bool root_unreadable = false;
  bool permission_limited = false;
  bool incomplete = false;
  bool truncated_by_depth = false;
  bool stopped = false;
  std::error_code error;
};

WalkStatus WalkDirectoryTree(const std::filesystem::path& root, const WalkOptions& options,
                             TreeWalkVisitor& visitor);

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

}
