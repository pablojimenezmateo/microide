#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace microide::project {

class SymlinkLoopGuard {
 public:
  SymlinkLoopGuard() = default;

  explicit SymlinkLoopGuard(const std::filesystem::path& root,
                            bool enforce_containment = true) {
    if (!enforce_containment) {
      return;
    }
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::canonical(root, error);
    if (!error) {
      root_ = canonical_root;
    }
  }

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

  Scope TryEnter(const std::filesystem::path& path, bool is_symlink) {
    if (!is_symlink) {
      return Scope::Untracked();
    }
    std::error_code error;
    const std::filesystem::path real = std::filesystem::canonical(path, error);
    if (error) {
      return Scope{};
    }
    if (!WithinRoot(real)) {
      return Scope{};
    }
    std::string key = real.generic_string();
    if (!followed_.insert(key).second) {
      return Scope{};
    }
    return Scope{this, std::move(key)};
  }

 private:
  bool WithinRoot(const std::filesystem::path& real) const {
    if (root_.empty()) {
      return true;
    }
    const std::filesystem::path relative = real.lexically_relative(root_);
    if (relative.empty()) {
      return false;
    }
    return *relative.begin() != std::filesystem::path("..");
  }

  std::filesystem::path root_;
  std::unordered_set<std::string> followed_;
};

}
