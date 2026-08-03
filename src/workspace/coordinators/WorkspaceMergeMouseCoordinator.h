#pragma once

#include <functional>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class MergeMouseCoordinator {
 public:
  struct Operations {
    std::function<WorkspaceShell::MergeSurfaceLayout(const SDL_FRect&, const MergeTabState&)>
        compute_merge_surface_layout;
    std::function<ScrollSurfaceLayout(const SDL_FRect&,
                                      const WorkspaceShell::MergeSurfaceLayout&,
                                      const MergeTabState&)>
        compute_merge_scroll_layout;
    std::function<WorkspaceShell::MergeInteractionLayout(const SDL_FRect&,
                                                         const WorkspaceShell::MergeSurfaceLayout&,
                                                         MergeTabState&)>
        build_merge_interaction_layout;
    std::function<WorkspaceShell::MergeToolbarLayout(
        const SDL_FRect&, const WorkspaceShell::MergeSurfaceLayout&)>
        compute_merge_toolbar_layout;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<void()> open_merge_result_file;
    std::function<SDL_FRect(const WorkspaceShell::MergeSurfaceLayout&,
                            const WorkspaceShell::MergeInteractionLayout&,
                            const MergeTrackedConflict&,
                            bool)>
        build_merge_source_action_button_rect;
    std::function<std::array<SDL_FRect, 4>(const WorkspaceShell::MergeSurfaceLayout&,
                                           const WorkspaceShell::MergeInteractionLayout&,
                                           const MergeTrackedConflict&)>
        build_merge_result_action_button_rects;
    std::function<void(compare::MergeChoice)> apply_merge_choice;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void(MergeTabState&,
                       std::optional<editor::SelectionRange>,
                       editor::TextPosition)>
        update_merge_tracking_after_viewport_edit;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(std::size_t)> request_merge_conflict_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> clear_drag_state;
    std::function<std::optional<std::size_t>(const MergeTabState&, std::size_t, bool)>
        find_merge_tracked_conflict_at_source_line;
    std::function<std::optional<std::size_t>(const MergeTabState&, std::size_t)>
        find_merge_tracked_conflict_at_result_line;
    std::function<void()> reveal_active_merge_selection;
    std::function<std::optional<MergeHoverState>(const WorkspaceShell::MergeSurfaceLayout&,
                                                 const WorkspaceShell::MergeInteractionLayout&,
                                                 const MergeTabState&,
                                                 float,
                                                 float)>
        classify_merge_hover_state;
    std::function<void()> reset_caret_blink;
    std::function<void(int)> move_merge_selection;
    std::function<void(int)> scroll_merge_columns;
    std::function<void()> mark_merge_resolved;
    std::function<void()> toggle_merge_base_pane;
    std::function<void()> jump_next_unresolved_merge_conflict;
    std::function<std::optional<SDL_FRect>(
        const SDL_FRect&,
        const WorkspaceShell::MergeSurfaceLayout&,
        std::string_view)>
        merge_secondary_toolbar_button_rect;
  };

  MergeMouseCoordinator(ProjectWorkspaceState& state,
                        InteractionState& interaction_state,
                        Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleHoverMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  MergeTabState* ActiveMergeTab() const;
  bool ActiveTabIsMerge() const;

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
