#include "workspace/state/EditorNavigationHistory.h"

namespace microide::workspace {

void EditorNavigationHistory::Observe(const std::filesystem::path& path, std::size_t line,
                                      std::size_t column) {
  if (!entries_.empty()) {
    Entry& current = entries_[cursor_];
    const std::size_t distance = line > current.line ? line - current.line : current.line - line;
    // native(), not path ==: the component-wise compare is the slow one, and this
    // runs every frame.
    if (current.path->native() == path.native() && distance <= kSignificantLines) {
      current.line = line;
      current.column = column;
      return;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_) + 1, entries_.end());
    if (entries_.size() >= kMaxEntries) {
      entries_.erase(entries_.begin());
    }
  }
  if (entries_.capacity() < kMaxEntries) {
    entries_.reserve(kMaxEntries);
  }
  entries_.push_back(Entry{.path = SharedPath(path), .line = line, .column = column});
  cursor_ = entries_.size() - 1;
}

std::shared_ptr<const std::filesystem::path> EditorNavigationHistory::SharedPath(
    const std::filesystem::path& path) const {
  for (const Entry& entry : entries_) {
    if (entry.path->native() == path.native()) {
      return entry.path;
    }
  }
  return std::make_shared<const std::filesystem::path>(path);
}

}  // namespace microide::workspace
