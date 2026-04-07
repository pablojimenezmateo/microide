#include <string>

namespace microide::seed {

std::string ActivePaneLabel(bool terminal_focused) {
  return terminal_focused ? "terminal" : "editor";
}

int VisiblePanelCount(bool sidebar_open, bool terminal_open) {
  int count = 1;
  if (sidebar_open) {
    ++count;
  }
  if (terminal_open) {
    ++count;
  }
  return count;
}

}  // namespace microide::seed
