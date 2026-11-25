#pragma once

#include <functional>

#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class CompareMouseCoordinator {
 public:
  struct Operations {
    std::function<workspace::CompareSurfaceLayout(const SDL_FRect&, const CompareTabState&)>
        compute_compare_surface_layout;
    std::function<ScrollSurfaceLayout(const SDL_FRect&, const workspace::CompareSurfaceLayout&, const CompareTabState&)>
        compute_compare_scroll_layout;
    std::function<void(CompareTabState&)> sync_compare_viewport_scroll;
    std::function<TextGridInteractionLayout(const workspace::CompareSurfaceLayout&, CompareTabState&)>
        build_compare_right_interaction_layout;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void()> reset_caret_blink;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void()> note_active_editable_edit;
    std::function<void()> request_frame;
    std::function<CollapsedContextActionRects(const SDL_FRect&, const compare::ComparePresentationRow&)>
        build_compare_collapsed_context_action_rects;
    std::function<bool(CompareTabState&, std::size_t, CompareCollapsedContextAction)>
        expand_compare_collapsed_context;
    std::function<void()> clear_drag_state;
    std::function<std::optional<CompareControlHit>(const SDL_FRect&, const workspace::CompareSurfaceLayout&,
                                                   const CompareTabState&, float, float)>
        compare_control_at;
    std::function<bool(CompareTabState&, const CompareControlHit&)> run_compare_control;
  };

  CompareMouseCoordinator(ProjectWorkspaceState& state,
                          InteractionState& interaction_state,
                          Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const SDL_FRect& surface);
  bool HandleDrag(const SDL_Event& event, const SDL_FRect& surface);
  bool HandleHoverMotion(const SDL_Event& event, const SDL_FRect& surface);
  bool HandleSelectionMotion(const SDL_Event& event, const SDL_FRect& surface);
  bool HandleWheel(const SDL_Event& event,
                   const SDL_FRect& surface,
                   CompareTabState& compare_tab,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  CompareTabState* ActiveCompareTab() const;

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}
