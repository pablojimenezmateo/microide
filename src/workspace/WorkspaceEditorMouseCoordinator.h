#pragma once

#include <functional>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class EditorMouseCoordinator {
 public:
  struct Operations {
    std::function<std::vector<WorkspaceShell::EditorSplitDividerLayout>(const SDL_FRect&)>
        compute_editor_split_divider_layouts;
    std::function<std::vector<WorkspaceShell::EditorPaneLayout>(const SDL_FRect&)>
        compute_editor_pane_layouts;
    std::function<void(std::size_t)> set_active_editor_split;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<ScrollSurfaceLayout(const SDL_FRect&,
                                      const editor::TextViewport&,
                                      const editor::EditorViewMetrics&)>
        compute_editor_scroll_layout;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<TabEntry::EditorTabState*()> active_editor_tab;
    std::function<void(TabEntry::EditorTabState&)> normalize_editor_split_tree;
    std::function<TabEntry::EditorTabState::EditorSplitNode*(
        TabEntry::EditorTabState::EditorSplitNode*, const std::vector<std::size_t>&)>
        find_editor_split_node;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&, const std::vector<std::size_t>&)>
        compute_editor_split_node_rect;
    std::function<void(TabEntry::EditorTabState::EditorSplitNode&)> normalize_editor_split_node;
    std::function<void()> clear_drag_state;
  };

  EditorMouseCoordinator(ProjectWorkspaceState& state,
                         InteractionState& interaction_state,
                         render::TextRenderer& text_renderer,
                         Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  render::TextRenderer& text_renderer_;
  Operations operations_;
};

}  // namespace microide::workspace
