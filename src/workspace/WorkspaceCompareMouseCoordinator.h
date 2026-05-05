#pragma once

#include <functional>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class CompareMouseCoordinator {
 public:
  struct Operations {
    std::function<WorkspaceShell::CompareSurfaceLayout(const SDL_FRect&, const CompareTabState&)>
        compute_compare_surface_layout;
    std::function<ScrollSurfaceLayout(const SDL_FRect&,
                                      const WorkspaceShell::CompareSurfaceLayout&,
                                      const CompareTabState&)>
        compute_compare_scroll_layout;
    std::function<void(CompareTabState&)> sync_compare_viewport_scroll;
    std::function<SDL_FRect(const SDL_FRect&, const WorkspaceShell::CompareSurfaceLayout&)>
        compare_divider_hit_rect;
    std::function<TextGridInteractionLayout(const WorkspaceShell::CompareSurfaceLayout&,
                                            CompareTabState&)>
        build_compare_right_interaction_layout;
    std::function<std::size_t(const CompareTabState&, std::size_t)> compare_right_line_for_row;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void()> reset_caret_blink;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(std::size_t, std::size_t)> request_compare_row_range_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<void(int)> scroll_compare_rows;
    std::function<void(int)> scroll_compare_columns;
    std::function<void()> clear_drag_state;
  };

  CompareMouseCoordinator(ProjectWorkspaceState& state,
                          InteractionState& interaction_state,
                          Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  CompareTabState* ActiveCompareTab() const;
  bool ActiveTabIsCompare() const;

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
