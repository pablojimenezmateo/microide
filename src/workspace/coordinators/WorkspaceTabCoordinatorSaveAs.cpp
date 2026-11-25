#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include "util/ErrorOut.h"

namespace microide::workspace {

bool TabCoordinator::SaveGroupTabAs(std::size_t group_index, std::size_t index,
                                    const std::filesystem::path& path, std::string* error) {
  const auto fail = [&](std::string message) {
    return util::Fail(error, std::move(message));
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
    return fail("a file name cannot contain a NUL character");
  }
  if (tab.editor_state->content_pending()) {
    return fail(tab.title + (tab.editor_state->content == EditorTabState::Content::Failed
                                 ? " could not be read"
                                 : " is still loading"));
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
  RenameTabsOnDocument(state_.editor_groups, viewport, normalized);
  if (!previous.empty()) {
    operations_.invalidate_editor_blame_path(previous.lexically_normal());
  }
  operations_.notify_plugin_buffer_open(normalized);
  if (!state_.directory_tree.SelectPath(normalized)) {
    state_.directory_tree.Refresh();
  }
  if (state_.directory_tree.SelectPath(normalized) && operations_.reveal_selected_tree_sidebar_line) {
    operations_.reveal_selected_tree_sidebar_line();
  }
  operations_.request_frame();
  return true;
}

}
