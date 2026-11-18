#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace microide::workspace {

// Save As: binding a buffer to a new file. Split out of WorkspaceTabCoordinator.cpp
// so that TU stays under the architecture lint's 900-line coordinator budget.

bool TabCoordinator::SaveGroupTabAs(std::size_t group_index, std::size_t index,
                                    const std::filesystem::path& path, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (group_index >= state_.editor_groups.size() ||
      index >= state_.editor_groups[group_index].open_tabs.size()) {
    return fail("No tab to save");
  }
  TabEntry& tab = state_.editor_groups[group_index].open_tabs[index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return fail("Only an editor tab can be saved as a file");
  }
  if (path.empty() || path.filename().empty()) {
    return fail("save needs a file name");
  }
  if (path.native().find('\0') != std::string::npos) {
    return fail("a file name cannot contain a NUL character");  // the write would cut it short
  }
  editor::TextViewport& viewport = tab.editor_state->viewport;
  const std::filesystem::path normalized = path.lexically_normal();
  if (normalized == viewport.path().lexically_normal()) {
    return SaveGroupTab(group_index, index, SaveMode::Blocking) || fail("Save failed");
  }
  std::error_code exists_error;
  if (std::filesystem::exists(normalized, exists_error)) {
    return fail(normalized.string() + " already exists");
  }
  if (AnotherBufferHoldsPath(state_.editor_groups, normalized, viewport)) {
    return fail(normalized.string() + " is open in another tab");
  }
  // The write goes to the buffer's path, so bind first -- and undo the binding if
  // the write fails (a directory that cannot be created, no permission): as in VS
  // Code, a failed Save As leaves the buffer what it was, not named after a file
  // that does not exist.
  const std::filesystem::path previous = viewport.path();
  const util::FileSignature previous_signature = viewport.disk_signature();
  const std::filesystem::path previous_tab_path = tab.path;
  std::string previous_title = tab.title;
  viewport.RebindToPath(normalized);
  tab.path = normalized;
  tab.title = normalized.filename().string();
  if (!SaveGroupTab(group_index, index, SaveMode::Blocking)) {
    viewport.RebindToPath(previous, previous_signature);
    tab.path = previous_tab_path;
    tab.title = std::move(previous_title);
    return fail("Save failed");
  }
  // A split's other panes share the document the save just re-pointed.
  RenameTabsOnDocument(state_.editor_groups, viewport, normalized);
  if (!previous.empty()) {
    operations_.invalidate_editor_blame_path(previous.lexically_normal());
  }
  operations_.notify_plugin_buffer_open(normalized);
  // The tree follows the active file; a buffer saved under a new name is a new
  // file the tree can show, so select and reveal it as activating a tab does
  // (VS Code reveals the saved file). It used to leave the previous selection,
  // so F2 after a first save renamed whatever row was selected before.
  // The index has not seen the new file yet; Save As is rare and the rescan
  // is what makes the row exist to select.
  if (!state_.directory_tree.SelectPath(normalized)) {
    state_.directory_tree.Refresh();
  }
  if (state_.directory_tree.SelectPath(normalized) && operations_.reveal_selected_tree_sidebar_line) {
    operations_.reveal_selected_tree_sidebar_line();
  }
  // The buffer has a new name: its tab title, its breadcrumb, and -- through the
  // extension -- possibly its language, so its highlighting. A plain save repaints
  // only the dirty marker.
  operations_.request_active_tab_redraw(/*include_tree_sidebar=*/true);
  return true;
}

}  // namespace microide::workspace
