#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "workspace/EditorTabService.h"

namespace microide::workspace {

// Editor splits are modelled as editor *groups* above the tab level (Phase B+).
// The editor area hosts 1 or 2 groups; each group's active tab renders into its
// own surface rect. Here we carve the editor surface into per-group panes; the
// matching per-group tab strips are produced from the full layout in the chrome
// pass (see ComputeEditorGroupRects).

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

namespace {

// Build the minimal WorkspaceLayout the geometry helper needs to split a surface
// rect into per-group editor-surface rects. Only `editor_surface` / `editor_area`
// participate in the surface split (tab strip / breadcrumb are chrome-only), and
// editor_surface shares the editor_area x/width, so this reproduces the chrome
// pass's split exactly.
WorkspaceLayout SurfaceOnlyLayout(const SDL_FRect& editor_surface) {
  WorkspaceLayout tmp{};
  tmp.editor_surface = editor_surface;
  tmp.editor_area =
      MakeRect(editor_surface.x, editor_surface.y, editor_surface.w, editor_surface.h);
  return tmp;
}

}  // namespace

std::size_t WorkspaceShell::FocusedEditorGroupIndex() const {
  return context_.current_project_state.focused_group_index;
}

EditorGroupRectsLayout WorkspaceShell::ComputeEditorGroupRectsForState(
    const WorkspaceLayout& layout) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  const std::size_t group_count = std::min<std::size_t>(state.editor_groups.size(), 2);
  const bool split = group_count >= 2 &&
                     state.group_split_orientation != EditorSplitOrientation::None;
  const bool vertical = state.group_split_orientation == EditorSplitOrientation::Vertical;
  return ComputeEditorGroupRects(layout, split ? group_count : 1, vertical,
                                 state.group_split_fraction);
}

std::vector<WorkspaceShell::EditorPaneLayout> WorkspaceShell::ComputeEditorPaneLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorPaneLayout> panes;
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (state.editor_groups.empty()) {
    return panes;
  }
  const EditorGroupRectsLayout group_rects =
      ComputeEditorGroupRectsForState(SurfaceOnlyLayout(editor_surface));
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

std::vector<WorkspaceShell::EditorSplitDividerLayout>
WorkspaceShell::ComputeEditorSplitDividerLayouts(const SDL_FRect& editor_surface) const {
  std::vector<EditorSplitDividerLayout> dividers;
  const EditorGroupRectsLayout group_rects =
      ComputeEditorGroupRectsForState(SurfaceOnlyLayout(editor_surface));
  if (group_rects.divider.has_value()) {
    dividers.push_back(EditorSplitDividerLayout{.node_path = {}, .divider_index = 0,
                                                .rect = *group_rects.divider});
  }
  return dividers;
}

}  // namespace microide::workspace
