#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class TerminalPanelService;
class PersistenceCoordinator;
class MenuCoordinator;

class TabMouseCoordinator {
 public:
  struct Services {
    TerminalPanelService& terminal_panel;
    PersistenceCoordinator& persistence;
    MenuCoordinator& menus;
  };

  struct Operations {
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_project_tabs;
    std::function<void(std::size_t)> request_close_project;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_tabs;
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(std::size_t,
                                                                      const SDL_FRect&)>
        compute_visible_tabs_for_group;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::size_t)> activate_tab;
    std::function<bool()> bottom_panel_visible;
    std::function<SDL_FRect(const SDL_FRect&)> bottom_panel_terminal_new_tab_rect;
    std::function<void(std::string)> open_terminal;
    std::function<SDL_FRect(const SDL_FRect&)> bottom_panel_maximize_button_rect;
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_bottom_panel_tabs;
    std::function<bool(std::size_t)> activate_bottom_panel_tab;
    std::function<bool(std::size_t)> close_bottom_panel_tab;
    std::function<bool(std::size_t)> bottom_panel_tab_is_terminal;
    std::function<void()> clear_tab_drag;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<bool(std::size_t)> move_active_project_to;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool(std::size_t, std::size_t, std::size_t, std::size_t)> move_tab_to_group;
    std::function<bool(std::size_t, std::size_t, std::size_t, EditorSplitOrientation, bool)>
        move_tab_to_new_group;
    std::function<bool(std::size_t)> move_active_output_tab_to;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_project_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        std::size_t, const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls_for_group;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_bottom_panel_tab_overflow_controls;
    std::function<bool(int)> scroll_project_tab_strip;
    std::function<bool(int)> scroll_editor_tab_strip;
    std::function<bool(std::size_t, int)> scroll_editor_tab_strip_for_group;
    std::function<bool(int)> scroll_bottom_panel_tab_strip;
    std::function<EditorGroupRectsLayout(const WorkspaceLayout&)> compute_editor_group_rects;
    std::function<void(std::size_t)> focus_editor_group;
    std::function<void()> open_untitled;
  };

  TabMouseCoordinator(ProjectCatalogState& project_catalog,
                      ProjectWorkspaceState& current_project_state,
                      TabDragState& tab_drag_state,
                      TabSlideState& tab_slide_state,
                      Services services,
                      Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);
  bool TickDragAutoScroll();
  bool CancelDrag();

 private:
  struct EditorGroupTabStrips {
    struct Entry {
      std::size_t group_index = 0;
      SDL_FRect strip{};
    };
    std::array<Entry, kMaxEditorGroups> entries{};
    std::size_t count = 0;
  };
  EditorGroupTabStrips ResolveEditorGroupTabStrips(const WorkspaceLayout& layout) const;

  struct DragStrip {
    SDL_FRect strip{};
    const std::vector<WorkspaceShell::VisibleStripTab>* tabs = nullptr;
    std::size_t count = 0;
    std::size_t active = 0;
    std::size_t model_offset = 0;
    std::size_t group_index = 0;
    std::function<bool(std::size_t)> move;
    std::function<bool(int)> scroll;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        overflow;
    bool valid = false;
  };
  DragStrip ResolveDragStrip(const WorkspaceLayout& layout, TabDragKind kind);
  DragStrip ResolveEditorDragStrip(const WorkspaceLayout& layout, std::size_t group_index);
  DragStrip ResolveBottomPanelDragStrip(const WorkspaceLayout& layout);
  std::size_t ResolvePointerEditorGroup(const WorkspaceLayout& layout,
                                        const EditorGroupRectsLayout& rects) const;
  void ResolveEditorBodyDrop(const EditorGroupRectsLayout& rects);
  void FinishEditorBodyDrop(const WorkspaceLayout& layout);
  bool UpdateDragForPointer();
  bool AutoScrollDragStrip(const DragStrip& d);
  void FinishDrag();
  void FinishCrossGroupDrag(const WorkspaceLayout& layout);
  void PersistReorderedTabs(TabDragKind kind);
  static constexpr std::size_t kNoLiftedTab = static_cast<std::size_t>(-1);
  static constexpr std::size_t kNoInsertionSlot = static_cast<std::size_t>(-1);
  void SeedSlideTargets(const DragStrip& d,
                        std::size_t insertion_slot,
                        std::size_t slide_slot,
                        std::size_t lifted_index);
  void ClearDestinationSlide();
  void AdvanceSlideOnInputThread();

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  TabDragState& tab_drag_state_;
  TabSlideState& tab_slide_state_;
  Services services_;
  Operations operations_;
};

}
