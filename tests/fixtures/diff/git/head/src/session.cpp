#include <string>

namespace microide::seed {

std::string ActivePaneLabel(bool terminal_focused, bool compare_focused) {
  if (compare_focused) {
    return "compare";
  }
  return terminal_focused ? "terminal" : "editor";
}

int VisiblePanelCount(bool sidebar_open, bool terminal_open, bool log_open) {
  int count = 1;
  if (sidebar_open) {
    ++count;
  }
  if (terminal_open) {
    ++count;
  }
  if (log_open) {
    ++count;
  }
  return count;
}

}  // namespace microide::seed
