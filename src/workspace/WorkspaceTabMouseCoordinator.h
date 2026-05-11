#pragma once

#include <functional>

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
    std::function<std::vector<WorkspaceShell::VisibleStripTab>(const SDL_FRect&)>
        compute_visible_terminal_tabs;
    std::function<bool(std::size_t)> activate_bottom_panel_tab;
    std::function<bool(std::size_t)> close_bottom_panel_tab;
    std::function<bool(std::size_t)> bottom_panel_tab_is_terminal;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<void(std::size_t)> close_terminal_tab;
    std::function<void()> clear_tab_drag;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<bool(std::size_t)> move_active_project_to;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool(std::size_t)> move_active_terminal_tab_to;
    std::function<void()> save_workspace_session;
    std::function<void()> save_session_state;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_project_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls;
    std::function<bool(int)> scroll_project_tab_strip;
    std::function<bool(int)> scroll_editor_tab_strip;
  };

  TabMouseCoordinator(ProjectCatalogState& project_catalog,
                      ProjectWorkspaceState& current_project_state,
                      TabDragState& tab_drag_state,
                      Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  void PersistReorderedTabs(TabDragKind kind);

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  TabDragState& tab_drag_state_;
  Operations operations_;
};

}  // namespace microide::workspace
