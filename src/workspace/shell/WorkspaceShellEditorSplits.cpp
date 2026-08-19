#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "workspace/services/EditorTabService.h"

namespace microide::workspace {

// Editor splits are modelled as editor *groups* above the tab level, arranged by
// `ProjectWorkspaceState::editor_split` (see EditorSplitTree). Every geometry
// query here goes through the one tree walk in ComputeEditorGroupRects, which
// produces each group's tab strip, breadcrumb and surface together — the panes
// below are just that walk's surface rects, with the focused group's external-
// change banner trimmed off.

SDL_FRect WorkspaceShell::EditorSurfaceBelowBanner(const SDL_FRect& editor_surface) const {
  if (ActiveEditorBannerForTab(context_.current_project_state) == nullptr) {
    return editor_surface;
  }
  const SDL_FRect strip = ComputeEditorBannerStripRect(editor_surface);
  SDL_FRect content = editor_surface;
  content.y += strip.h;
  content.h = std::max(0.0f, content.h - strip.h);
  return content;
}

std::size_t WorkspaceShell::FocusedEditorGroupIndex() const {
  // Clamped: the stored index outlives a group collapse by one call, and every
  // caller here indexes groups or compares against a pane ordinal with it.
  return context_.current_project_state.clamped_focused_group_index();
}

editor::TextViewport* WorkspaceShell::ViewportForPane(const EditorPaneLayout& pane) {
  std::vector<EditorGroup>& groups = context_.current_project_state.editor_groups;
  if (pane.group_index >= groups.size()) {
    return nullptr;
  }
  return GroupActiveViewport(groups[pane.group_index]);
}

const editor::TextViewport* WorkspaceShell::ViewportForPane(const EditorPaneLayout& pane) const {
  const std::vector<EditorGroup>& groups = context_.current_project_state.editor_groups;
  if (pane.group_index >= groups.size()) {
    return nullptr;
  }
  return GroupActiveViewport(groups[pane.group_index]);
}

EditorGroupRectsLayout WorkspaceShell::ComputeEditorGroupRectsForState(
    const WorkspaceLayout& layout) const {
  return ComputeEditorGroupRects(layout, context_.current_project_state.editor_split);
}

WorkspaceShell::EditorPaneLayouts WorkspaceShell::EditorPaneLayoutsFromGroupRects(
    const EditorGroupRectsLayout& group_rects) const {
  EditorPaneLayouts panes;
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (state.editor_groups.empty()) {
    return panes;
  }
  const std::size_t focused = state.focused_group_index < group_rects.groups.size()
                                  ? state.focused_group_index
                                  : 0;
  for (std::size_t i = 0; i < group_rects.groups.size(); ++i) {
    const bool active = i == focused;
    // The external-change banner belongs to the focused group; trim it from that
    // group's surface so the editor content lays out below the strip.
    const SDL_FRect surface = active ? EditorSurfaceBelowBanner(group_rects.groups[i].editor_surface)
                                     : group_rects.groups[i].editor_surface;
    panes.push_back(EditorPaneLayout{.group_index = i, .rect = surface, .active = active});
  }
  return panes;
}

float WorkspaceShell::EditorGroupTabStripWidth(std::size_t group_index) const {
  const auto window_rect = CurrentWindowRect();
  const float window_width = window_rect.has_value() ? window_rect->w : 1440.0f;
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return window_width;
  }
  const EditorGroupRectsLayout rects = ComputeEditorGroupRectsForState(*layout);
  if (group_index >= rects.groups.size()) {
    return layout->tab_strip.w > 0.0f ? layout->tab_strip.w : window_width;
  }
  const float width = rects.groups[group_index].tab_strip.w;
  return width > 0.0f ? width : window_width;
}

WorkspaceShell::EditorPaneLayouts WorkspaceShell::ComputeEditorPaneLayouts(
    const WorkspaceLayout& layout) const {
  if (context_.current_project_state.editor_groups.empty()) {
    return {};
  }
  return EditorPaneLayoutsFromGroupRects(ComputeEditorGroupRectsForState(layout));
}

}  // namespace microide::workspace
