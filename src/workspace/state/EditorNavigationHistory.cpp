#include "workspace/state/EditorNavigationHistory.h"

namespace microide::workspace {

void EditorNavigationHistory::Observe(const std::filesystem::path& path, std::size_t line,
                                      std::size_t column) {
  if (!entries_.empty()) {
    Location& current = entries_[cursor_];
    const std::size_t distance = line > current.line ? line - current.line : current.line - line;
    // native(), not path ==: the component-wise compare is the slow one, and this
    // runs every frame.
    if (current.path.native() == path.native() && distance <= kSignificantLines) {
      current.line = line;
      current.column = column;
      return;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_) + 1, entries_.end());
    if (entries_.size() >= kMaxEntries) {
      entries_.erase(entries_.begin());
    }
  }
  entries_.push_back(Location{.path = path, .line = line, .column = column});
  cursor_ = entries_.size() - 1;
}

}  // namespace microide::workspace
