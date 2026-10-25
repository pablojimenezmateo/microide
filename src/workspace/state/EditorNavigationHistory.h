#pragma once

// Go Back / Go Forward (VS Code's navigation history, Alt+Left / Alt+Right).
//
// A plain value: the shell observes where the caret of the active file editor is
// once per prepared frame (and again right before a step), and this decides
// whether that is a new place or the current one moved a little. VS Code's rule:
// another file, or more than kSignificantLines away in the same one, is a new
// entry; a smaller move updates the current entry, so going back returns to where
// you last were in that file, not where you first arrived.
//
// Observing the unchanged location allocates nothing, which matters because it runs
// every frame. Recording a far jump in a file already on the list allocates nothing
// either: entries of one file share its path, and the list is reserved once.

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
    std::size_t line = 0;    // 0-based
    std::size_t column = 0;  // 0-based byte column
  };

  static constexpr std::size_t kSignificantLines = 10;
  static constexpr std::size_t kMaxEntries = 50;

  // Records `location` as a new entry (dropping everything ahead of the cursor) or
  // updates the current entry in place.
  void Observe(const std::filesystem::path& path, std::size_t line, std::size_t column);

  // Steps the cursor and returns the entry to show, skipping entries `exists`
  // rejects (a deleted file) and erasing them. nullopt when there is nowhere to go.
  template <typename Exists>
  std::optional<Location> Step(int direction, Exists&& exists);

  [[nodiscard]] bool CanGoBack() const { return cursor_ > 0; }
  [[nodiscard]] bool CanGoForward() const { return cursor_ + 1 < entries_.size(); }
  struct Entry {
    std::shared_ptr<const std::filesystem::path> path;  // shared by a file's entries
    std::size_t line = 0;
    std::size_t column = 0;
  };
  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
  [[nodiscard]] std::size_t cursor() const { return cursor_; }

 private:
  // The path of an entry already listed for this file, or a new one.
  std::shared_ptr<const std::filesystem::path> SharedPath(const std::filesystem::path& path) const;

  std::vector<Entry> entries_;
  std::size_t cursor_ = 0;  // index into entries_; meaningless while it is empty
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
      --cursor_;  // the cursor's entry moved down one
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace
