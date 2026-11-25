#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace microide::workspace {

class EditorNavigationHistory {
 public:
  struct Location {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  static constexpr std::size_t kSignificantLines = 10;
  static constexpr std::size_t kMaxEntries = 50;

  void Observe(const std::filesystem::path& path, std::size_t line, std::size_t column);

  template <typename Exists>
  std::optional<Location> Step(int direction, Exists&& exists);

  [[nodiscard]] bool CanGoBack() const { return cursor_ > 0; }
  [[nodiscard]] bool CanGoForward() const { return cursor_ + 1 < entries_.size(); }
  struct Entry {
    std::shared_ptr<const std::filesystem::path> path;
    std::size_t line = 0;
    std::size_t column = 0;
  };
  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
  [[nodiscard]] std::size_t cursor() const { return cursor_; }

 private:
  std::shared_ptr<const std::filesystem::path> SharedPath(const std::filesystem::path& path) const;

  std::vector<Entry> entries_;
  std::size_t cursor_ = 0;
};

template <typename Exists>
std::optional<EditorNavigationHistory::Location> EditorNavigationHistory::Step(int direction,
                                                                               Exists&& exists) {
  while (direction < 0 ? CanGoBack() : CanGoForward()) {
    const std::size_t target = direction < 0 ? cursor_ - 1 : cursor_ + 1;
    if (exists(*entries_[target].path)) {
      cursor_ = target;
      const Entry& entry = entries_[target];
      return Location{.path = *entry.path, .line = entry.line, .column = entry.column};
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(target));
    if (direction < 0) {
      --cursor_;
    }
  }
  return std::nullopt;
}

}
