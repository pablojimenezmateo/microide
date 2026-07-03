#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace microide::project {

// Detects directory-symlink cycles during recursive filesystem walks.
//
// A real directory can never be its own ancestor (POSIX forbids hard-linked
// directories), so a recursive walk can only become unbounded when it follows
// a directory *symlink* that resolves to an ancestor on the current descent
// branch. This guard tracks the canonical targets of the symlinks followed on
// the active branch: before following a directory symlink, the caller asks
// TryEnter() to resolve its real target and reject it when that target is
// already on the branch (a cycle) or the link is broken/inaccessible.
//
// Non-symlink directories enter unconditionally and record nothing — they
// cannot start a cycle, so the common case pays no canonicalization cost.
//
// When constructed with a project root, the guard additionally refuses to follow
// any directory symlink whose canonical target escapes that root. The opened
// project is untrusted: a symlink such as `project/evil -> /` is not a cycle, so
// without this the walk would descend into and index the entire host filesystem
// — a resource-exhaustion DoS and an information-disclosure/traversal escape.
class SymlinkLoopGuard {
 public:
  SymlinkLoopGuard() = default;

  // `root` bounds which symlink targets may be followed: a symlink resolving
  // outside the (canonicalized) root is skipped. An empty/uncanonicalizable root
  // disables the containment check (cycle detection still applies).
  //
  // `enforce_containment` (default true) is the opt-out for the
  // `project.follow_out_of_root_symlinks` user setting: when false the root is
  // left unset so out-of-root symlink targets are followed (still cycle-guarded),
  // restoring the pre-hardening behavior for monorepos / symlinked dependencies.
  explicit SymlinkLoopGuard(const std::filesystem::path& root,
                            bool enforce_containment = true) {
    if (!enforce_containment) {
      return;  // root_ stays empty -> containment disabled, cycle guard intact
    }
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::canonical(root, error);
    if (!error) {
      root_ = canonical_root;
    }
  }

  // RAII record of a successful entry. While alive, the entered symlink target
  // (if any) stays on the branch; destruction removes it so sibling branches
  // can legitimately follow a symlink to the same real directory.
  class Scope {
   public:
    Scope() = default;
    Scope(Scope&& other) noexcept
        : owner_(other.owner_), key_(std::move(other.key_)), entered_(other.entered_) {
      other.owner_ = nullptr;
      other.entered_ = false;
    }
    Scope& operator=(Scope&&) = delete;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
      if (owner_ != nullptr) {
        owner_->followed_.erase(key_);
      }
    }

    // True when the directory may be entered; false means skip it (cycle or
    // broken/inaccessible symlink target).
    bool entered() const { return entered_; }

   private:
    friend class SymlinkLoopGuard;
    Scope(SymlinkLoopGuard* owner, std::string key)
        : owner_(owner), key_(std::move(key)), entered_(true) {}
    static Scope Untracked() {
      Scope scope;
      scope.entered_ = true;
      return scope;
    }

    SymlinkLoopGuard* owner_ = nullptr;
    std::string key_;
    bool entered_ = false;
  };

  // `is_symlink` is whether `path` itself is a symlink (the caller usually
  // already knows from the directory entry). Returns a Scope whose entered()
  // is false when the directory must be skipped.
  Scope TryEnter(const std::filesystem::path& path, bool is_symlink) {
    if (!is_symlink) {
      return Scope::Untracked();
    }
    std::error_code error;
    const std::filesystem::path real = std::filesystem::canonical(path, error);
    if (error) {
      // Broken or inaccessible symlink target: do not follow.
      return Scope{};
    }
    if (!WithinRoot(real)) {
      // Target escapes the project root: refuse (see class comment).
      return Scope{};
    }
    std::string key = real.generic_string();
    if (!followed_.insert(key).second) {
      // Target already on the current branch: following it would cycle.
      return Scope{};
    }
    return Scope{this, std::move(key)};
  }

 private:
  // True when `real` is the root itself or lies underneath it. With no root set,
  // containment is not enforced (returns true) so cycle detection alone governs.
  bool WithinRoot(const std::filesystem::path& real) const {
    if (root_.empty()) {
      return true;
    }
    const std::filesystem::path relative = real.lexically_relative(root_);
    // Empty means unrelated trees; a leading ".." *component* means the target
    // climbs out. Compare the first component, not a raw string prefix, so an
    // in-root directory whose name merely starts with ".." (e.g. "..build") is
    // not misread as an escape.
    if (relative.empty()) {
      return false;
    }
    return *relative.begin() != std::filesystem::path("..");
  }

  std::filesystem::path root_;
  std::unordered_set<std::string> followed_;
};

}  // namespace microide::project
