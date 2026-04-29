#include "workspace/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "workspace/TerminalPanelService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

constexpr float kTabDragStartDistance = 6.0f;

template <typename VisibleTabType>
std::size_t StripInsertionSlot(const SDL_FRect& strip,
                               const std::vector<VisibleTabType>& tabs,
                               float x,
                               std::size_t item_count) {
  if (item_count == 0) {
    return 0;
  }
  if (tabs.empty()) {
    return x <= strip.x + strip.w * 0.5f ? 0 : item_count;
  }
  if (x <= tabs.front().rect.x) {
    return 0;
  }
  for (const VisibleTabType& tab : tabs) {
    const float midpoint = tab.rect.x + tab.rect.w * 0.5f;
    if (x < midpoint) {
      return tab.index;
    }
  }
  const VisibleTabType& last = tabs.back();
  if (x >= last.rect.x + last.rect.w) {
    return item_count;
  }
  return std::min(item_count, last.index + 1);
}

std::size_t MoveTargetIndexForInsertion(std::size_t insertion_slot,
                                        std::size_t active_index,
                                        std::size_t item_count) {
  if (item_count == 0) {
    return 0;
  }
  const std::size_t clamped_slot = std::min(insertion_slot, item_count);
  const std::size_t target_index = clamped_slot > active_index ? clamped_slot - 1 : clamped_slot;
  return std::min(target_index, item_count - 1);
}

TabMouseCoordinator::TabMouseCoordinator(ProjectCatalogState& project_catalog,
                                         ProjectWorkspaceState& current_project_state,
                                         TabDragState& tab_drag_state,
                                         Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      tab_drag_state_(tab_drag_state),
      operations_(std::move(operations)) {}

bool TabMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                           const WorkspaceLayout& layout) {
  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    for (const WorkspaceShell::VisibleStripTab& tab :
         operations_.compute_visible_project_tabs(layout.project_tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        operations_.request_close_project(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        operations_.switch_project(tab.index, true);
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Project,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
        };
      }
      return true;
    }
  }

  if (Contains(layout.tab_strip, event.button.x, event.button.y)) {
    if (state_.root.empty()) {
      return false;
    }
    if (state_.open_tabs.empty()) {
      const SDL_FRect placeholder_tab =
          MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                   std::max(22.0f, layout.tab_strip.h - 2.0f));
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(placeholder_tab, event.button.x, event.button.y)) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
      return false;
    }

    for (const WorkspaceShell::VisibleStripTab& tab :
         operations_.compute_visible_tabs(layout.tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        operations_.request_close_tab(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        operations_.activate_tab(tab.index);
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Editor,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        operations_.activate_tab(tab.index);
        operations_.open_anchored_menu(
            MenuId::EditorTabContext,
            MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                     1.0f));
      }
      return true;
    }
  }

  if (!operations_.bottom_panel_visible()) {
    return false;
  }

  const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                          layout.bottom_panel.w, kWorkspaceBottomPanelHeaderHeight);
  if (!Contains(panel_header, event.button.x, event.button.y)) {
    return false;
  }

  if (event.button.button == SDL_BUTTON_LEFT &&
      Contains(operations_.bottom_panel_terminal_new_tab_rect(panel_header), event.button.x,
               event.button.y)) {
    operations_.open_terminal({});
    return true;
  }

  for (const WorkspaceShell::VisibleStripTab& tab :
       operations_.compute_visible_bottom_panel_tabs(panel_header)) {
    if (!Contains(tab.rect, event.button.x, event.button.y)) {
      continue;
    }

    if (event.button.button == SDL_BUTTON_MIDDLE ||
        (event.button.button == SDL_BUTTON_LEFT &&
         Contains(tab.close_rect, event.button.x, event.button.y))) {
      operations_.close_bottom_panel_tab(tab.index);
    } else if (event.button.button == SDL_BUTTON_LEFT) {
      if (operations_.activate_bottom_panel_tab(tab.index) &&
          operations_.bottom_panel_tab_is_terminal(tab.index)) {
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Terminal,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
        };
      }
    } else if (event.button.button == SDL_BUTTON_RIGHT) {
      if (operations_.activate_bottom_panel_tab(tab.index) &&
          operations_.bottom_panel_tab_is_terminal(tab.index)) {
        operations_.open_anchored_menu(
            MenuId::TerminalTabContext,
            MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                     1.0f));
      }
    }
    return true;
  }

  return false;
}

bool TabMouseCoordinator::HandleButtonUp(const SDL_Event& event) {
  if (event.button.button != SDL_BUTTON_LEFT || tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }

  const TabDragKind kind = tab_drag_state_.kind;
  const bool reordered = tab_drag_state_.reordered;
  operations_.clear_tab_drag();
  if (reordered) {
    PersistReorderedTabs(kind);
  }
  return true;
}

bool TabMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
  if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
    const TabDragKind kind = tab_drag_state_.kind;
    const bool reordered = tab_drag_state_.reordered;
    operations_.clear_tab_drag();
    if (reordered) {
      PersistReorderedTabs(kind);
    }
    return false;
  }

  const float delta_x = static_cast<float>(event.motion.x) - tab_drag_state_.press_x;
  const float delta_y = static_cast<float>(event.motion.y) - tab_drag_state_.press_y;
  if (!tab_drag_state_.dragging && std::hypot(delta_x, delta_y) < kTabDragStartDistance) {
    return true;
  }
  tab_drag_state_.dragging = true;

  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;
  switch (tab_drag_state_.kind) {
    case TabDragKind::Project:
      if (Contains(layout.project_tab_strip, event.motion.x, event.motion.y) &&
          !project_catalog_.entries.empty()) {
        const auto visible_tabs = operations_.compute_visible_project_tabs(layout.project_tab_strip);
        const std::size_t insertion_slot = StripInsertionSlot(
            layout.project_tab_strip, visible_tabs, static_cast<float>(event.motion.x),
            project_catalog_.entries.size());
        const std::size_t target_index = MoveTargetIndexForInsertion(
            insertion_slot, project_catalog_.active_index, project_catalog_.entries.size());
        if (target_index != project_catalog_.active_index &&
            operations_.move_active_project_to(target_index)) {
          tab_drag_state_.reordered = true;
        }
      }
      return true;
    case TabDragKind::Editor:
      if (Contains(layout.tab_strip, event.motion.x, event.motion.y) &&
          !state_.open_tabs.empty()) {
        const auto visible_tabs = operations_.compute_visible_tabs(layout.tab_strip);
        const std::size_t insertion_slot =
            StripInsertionSlot(layout.tab_strip, visible_tabs, static_cast<float>(event.motion.x),
                               state_.open_tabs.size());
        const std::size_t target_index =
            MoveTargetIndexForInsertion(insertion_slot, state_.active_tab_index, state_.open_tabs.size());
        if (target_index != state_.active_tab_index && operations_.move_active_tab_to(target_index)) {
          tab_drag_state_.reordered = true;
        }
      }
      return true;
    case TabDragKind::Terminal:
      if (operations_.bottom_panel_visible()) {
        const SDL_FRect panel_header =
            MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                     kWorkspaceBottomPanelHeaderHeight);
        if (Contains(panel_header, event.motion.x, event.motion.y) && !state_.terminal_tabs.empty()) {
          const auto visible_tabs = operations_.compute_visible_terminal_tabs(panel_header);
          const std::size_t insertion_slot = StripInsertionSlot(
              panel_header, visible_tabs, static_cast<float>(event.motion.x), state_.terminal_tabs.size());
          const std::size_t target_index = MoveTargetIndexForInsertion(
              insertion_slot, state_.active_terminal_tab_index, state_.terminal_tabs.size());
          if (target_index != state_.active_terminal_tab_index &&
              operations_.move_active_terminal_tab_to(target_index)) {
            tab_drag_state_.reordered = true;
          }
        }
      }
      return true;
    case TabDragKind::None:
    default:
      return false;
  }
}

bool TabMouseCoordinator::HandleWheel(const SDL_Event& event,
                                      const WorkspaceLayout& layout,
                                      int vertical_ticks) {
  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !project_catalog_.entries.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(project_catalog_.entries.size()) - 1);
    project_catalog_.tab_scroll_index =
        std::clamp(project_catalog_.tab_scroll_index - vertical_ticks, 0, max_scroll);
    return true;
  }

  if (Contains(layout.tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !state_.open_tabs.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(state_.open_tabs.size()) - 1);
    state_.tab_scroll_index = std::clamp(state_.tab_scroll_index - vertical_ticks, 0, max_scroll);
    return true;
  }

  return false;
}

void TabMouseCoordinator::PersistReorderedTabs(TabDragKind kind) {
  if (kind == TabDragKind::Project) {
    operations_.save_workspace_session();
  } else {
    operations_.save_session_state();
  }
}

TabMouseCoordinator WorkspaceShell::MakeTabMouseCoordinator() {
  auto terminal_panel = MakeTerminalPanelService();
  return TabMouseCoordinator(
      context_.project_catalog,
      context_.current_project_state,
      context_.interaction_state.tab_drag,
      TabMouseCoordinator::Operations{
          .compute_visible_project_tabs =
              [this](const SDL_FRect& rect) { return ComputeVisibleProjectTabs(rect); },
          .request_close_project = [this](std::size_t index) { RequestCloseProject(index); },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .compute_visible_tabs = [this](const SDL_FRect& rect) { return ComputeVisibleTabs(rect); },
          .request_close_tab = [this](std::size_t index) { RequestCloseTab(index); },
          .activate_tab = [this](std::size_t index) { ActivateTab(index); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, rect);
              },
          .bottom_panel_visible = [this]() { return BottomPanelVisible(); },
          .bottom_panel_terminal_new_tab_rect =
              [this](const SDL_FRect& rect) { return BottomPanelTerminalNewTabRect(rect); },
          .open_terminal =
              [terminal_panel](std::string command) mutable {
                terminal_panel.OpenTerminal(std::move(command));
              },
          .compute_visible_bottom_panel_tabs =
              [this](const SDL_FRect& rect) { return ComputeVisibleBottomPanelTabs(rect); },
          .compute_visible_terminal_tabs =
              [this](const SDL_FRect& rect) { return ComputeVisibleTerminalTabs(rect); },
          .activate_bottom_panel_tab =
              [this](std::size_t index) { return ActivateBottomPanelTab(index); },
          .close_bottom_panel_tab =
              [this](std::size_t index) { return CloseBottomPanelTab(index); },
          .bottom_panel_tab_is_terminal =
              [this](std::size_t index) { return BottomPanelTabIsTerminal(index); },
          .active_terminal_tab = [this]() { return ActiveTerminalTab(); },
          .close_terminal_tab =
              [terminal_panel](std::size_t index) mutable {
                terminal_panel.CloseTerminalTab(index);
              },
          .clear_tab_drag = [this]() { ClearTabDrag(); },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .move_active_project_to = [this](std::size_t index) { return MoveActiveProjectTo(index); },
          .move_active_tab_to = [this](std::size_t index) { return MoveActiveTabTo(index); },
          .move_active_terminal_tab_to =
              [terminal_panel](std::size_t index) mutable {
                return terminal_panel.MoveActiveTerminalTabTo(index);
              },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .save_session_state = [this]() { MakePersistenceCoordinator().SaveSessionState(); },
      });
}

bool WorkspaceShell::HandleTabMouseMotion(const SDL_Event& event, const WorkspaceLayout& layout) {
  (void)layout;
  return MakeTabMouseCoordinator().HandleMotion(event);
}

bool WorkspaceShell::HandleTabMouseButtonDown(const SDL_Event& event, const WorkspaceLayout& layout) {
  return MakeTabMouseCoordinator().HandleButtonDown(event, layout);
}

bool WorkspaceShell::HandleTabMouseButtonUp(const SDL_Event& event) {
  return MakeTabMouseCoordinator().HandleButtonUp(event);
}

bool WorkspaceShell::HandleTabMouseWheel(const SDL_Event& event, const WorkspaceLayout& layout, int vertical_ticks) {
  return MakeTabMouseCoordinator().HandleWheel(event, layout, vertical_ticks);
}

}  // namespace microide::workspace
