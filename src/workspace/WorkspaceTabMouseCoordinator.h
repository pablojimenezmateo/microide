#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class TabMouseCoordinator {
 public:
  struct Operations {
    std::function<std::vector<WorkspaceShell::VisibleStripTab>(const SDL_FRect&)>
        compute_visible_project_tabs;
    std::function<void(std::size_t)> request_close_project;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<std::vector<WorkspaceShell::VisibleStripTab>(const SDL_FRect&)>
        compute_visible_tabs;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::size_t)> activate_tab;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<bool()> bottom_panel_visible;
    std::function<SDL_FRect(const SDL_FRect&)> bottom_panel_terminal_new_tab_rect;
    std::function<void(std::string)> open_terminal;
    std::function<std::vector<WorkspaceShell::VisibleStripTab>(const SDL_FRect&)>
        compute_visible_bottom_panel_tabs;
    std::function<bool(std::size_t)> activate_bottom_panel_tab;
    std::function<bool(std::size_t)> close_bottom_panel_tab;
    std::function<bool(std::size_t)> bottom_panel_tab_is_terminal;
    std::function<void()> clear_tab_drag;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<bool(std::size_t)> move_active_project_to;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool(std::size_t)> move_active_terminal_tab_to;
    std::function<bool(std::size_t)> move_active_output_tab_to;
    std::function<void()> save_workspace_session;
    std::function<void()> save_session_state;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_project_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_bottom_panel_tab_overflow_controls;
    std::function<bool(int)> scroll_project_tab_strip;
    std::function<bool(int)> scroll_editor_tab_strip;
    std::function<bool(int)> scroll_bottom_panel_tab_strip;
    // Per-group editor tab strips: maps each group index to its on-screen tab
    // strip rect (one entry for a single group, two in a split). Lets the tab
    // mouse path resolve which group's strip the pointer hit and focus it.
    std::function<std::vector<std::pair<std::size_t, SDL_FRect>>()> compute_editor_group_tab_strips;
    std::function<void(std::size_t)> focus_editor_group;
  };

  TabMouseCoordinator(ProjectCatalogState& project_catalog,
                      ProjectWorkspaceState& current_project_state,
                      TabDragState& tab_drag_state,
                      TabSlideState& tab_slide_state,
                      Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  // Resolved geometry/state for the strip that owns the in-flight drag. Shared
  // by motion (compute target slot) and commit (single reorder on release).
  struct DragStrip {
    SDL_FRect strip{};
    std::vector<WorkspaceShell::VisibleStripTab> tabs;  // model-space, as rendered
    std::size_t count = 0;          // reorderable items in the dragged tab's kind
    std::size_t active = 0;         // active index within that kind list
    std::size_t model_offset = 0;   // model index where this kind's range begins
    std::function<bool(std::size_t)> move;  // commit a reorder within the kind list
    bool valid = false;
  };
  DragStrip ResolveDragStrip(const WorkspaceLayout& layout, TabDragKind kind);
  DragStrip ResolveBottomPanelDragStrip(const WorkspaceLayout& layout);
  void CommitDrag();
  void PersistReorderedTabs(TabDragKind kind);
  // Recomputes the Chrome-like slide targets for the live pointer: neighbor tabs
  // ease toward `d`'s displaced layout with a gap opened at `insertion_slot`.
  void SeedSlideTargets(const DragStrip& d, std::size_t insertion_slot);
  // Kicks off the post-release settle so the dropped tab glides from the ghost
  // position into its committed slot. Call after CommitDrag(), before the drag
  // state is cleared. Clears the slide animation when there was no live drag.
  void SeedSettle();

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  TabDragState& tab_drag_state_;
  TabSlideState& tab_slide_state_;
  Operations operations_;
};

}  // namespace microide::workspace
