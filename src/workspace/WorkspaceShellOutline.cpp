#include "workspace/WorkspaceShell.h"

#include "editor/TextViewport.h"
#include "workspace/WorkspaceOutlineFlat.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

bool WorkspaceShell::EditorOutlineEnabled() const {
  const auto value = GetSettingValue("editor.outline.enabled");
  if (!value.has_value()) {
    return true;
  }
  const std::string& s = *value;
  return !(s == "false" || s == "0" || s == "off");
}

void WorkspaceShell::PollOutlineService(uint32_t time_ms) {
  outline_service_.Poll(time_ms, EditorOutlineEnabled(), context_.current_project_state,
                          CurrentLspManager(), language_contract_,
                          [this](std::string_view id) { return GetSettingValue(id); });
}

void WorkspaceShell::TouchOutlineDebouncedAfterEditorSync() {
  if (!EditorOutlineEnabled()) {
    return;
  }
  outline_service_.ScheduleDebouncedRefresh();
}

void WorkspaceShell::HandleOutlineSidebarPointerDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return;
  }
  const auto rows = BuildOutlineFlatRows(context_.current_project_state.sidebar.outline);
  if (rows.empty()) {
    return;
  }
  const auto list_layout = ComputePluginSidebarListLayout(layout.sidebar, rows.size());
  const auto item_index =
      ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (!item_index.has_value() || *item_index < 0 ||
      *item_index >= static_cast<int>(rows.size())) {
    return;
  }
  const std::size_t idx = static_cast<std::size_t>(*item_index);
  context_.current_project_state.sidebar.outline.selected_flat_index = idx;

  const int scroll_row = list_layout.scroll_row;
  const SDL_FRect row_rect = ScrollableListRowRect(list_layout, *item_index - scroll_row);
  if (!Contains(row_rect, event.button.x, event.button.y)) {
    return;
  }

  constexpr float kTreeIndentWidth = 14.0f;
  constexpr float kTreeChevronSlotWidth = 12.0f;
  const float tree_x = row_rect.x + 6.0f + static_cast<float>(rows[idx].depth) * kTreeIndentWidth;
  const float chevron_right = tree_x + kTreeChevronSlotWidth;
  if (rows[idx].has_children && event.button.x >= tree_x && event.button.x < chevron_right) {
    auto& coll = context_.current_project_state.sidebar.outline.collapsed_paths;
    const std::string& pk = rows[idx].path_key;
    if (coll.contains(pk)) {
      coll.erase(pk);
    } else {
      coll.insert(pk);
    }
    RequestEditorSurfaceRedraw();
    return;
  }

  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return;
  }
  const OutlineFlatRow& r = rows[idx];
  viewport->ClearSecondaryCarets();
  viewport->MoveCursorTo(r.jump_line, r.jump_column);
  const std::size_t vrow = viewport->VisualRowForLine(r.jump_line);
  const std::size_t half = viewport->visible_lines() / 2;
  const std::size_t new_scroll = vrow > half ? vrow - half : 0;
  const std::size_t max_scroll =
      viewport->visual_line_count() > 0 ? viewport->visual_line_count() - 1 : 0;
  viewport->SetScrollLine(std::min(new_scroll, max_scroll));
  RevealSelectedOutlineSidebarLine();
  RequestFocusedEditorRedraw();
  if (context_.current_project_state.sidebar.temporary) {
    RestorePreviousSidebar();
  }
  context_.current_project_state.surface.focus = FocusTarget::Editor;
}

void WorkspaceShell::RevealSelectedOutlineSidebarLine() {
  const auto rows = BuildOutlineFlatRows(context_.current_project_state.sidebar.outline);
  if (rows.empty() ||
      context_.current_project_state.sidebar.outline.selected_flat_index >= rows.size()) {
    return;
  }
  if (!prepared_frame_layout_.has_value()) {
    return;
  }
  const WorkspaceLayout& layout = *prepared_frame_layout_;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      ComputePluginSidebarListLayout(layout.sidebar, rows.size());
  context_.current_project_state.sidebar.scroll_row = RevealScrollableListIndex(
      list_layout,
      static_cast<int>(context_.current_project_state.sidebar.outline.selected_flat_index));
}

}  // namespace microide::workspace
