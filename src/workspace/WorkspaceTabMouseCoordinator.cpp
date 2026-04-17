#include "workspace/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

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
  const std::size_t target_index =
      clamped_slot > active_index ? clamped_slot - 1 : clamped_slot;
  return std::min(target_index, item_count - 1);
}

}  // namespace

WorkspaceShell::TabMouseCoordinator::TabMouseCoordinator(WorkspaceShell& shell) : shell_(shell) {}

bool WorkspaceShell::TabMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                                           const WorkspaceLayout& layout) {
  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    for (const VisibleStripTab& tab : shell_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        shell_.RequestCloseProject(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        shell_.SwitchProject(tab.index, true);
        shell_.tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Project,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
        };
      }
      return true;
    }
  }

  if (Contains(layout.tab_strip, event.button.x, event.button.y)) {
    if (shell_.project_root_.empty()) {
      return false;
    }
    if (shell_.open_tabs_.empty()) {
      const SDL_FRect placeholder_tab =
          MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                   std::max(22.0f, layout.tab_strip.h - 2.0f));
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(placeholder_tab, event.button.x, event.button.y)) {
        shell_.surface_.focus = FocusTarget::Editor;
        return true;
      }
      return false;
    }

    for (const VisibleStripTab& tab : shell_.ComputeVisibleTabs(layout.tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        shell_.RequestCloseTab(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        shell_.ActivateTab(tab.index);
        shell_.tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Editor,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        shell_.ActivateTab(tab.index);
        shell_.OpenAnchoredMenu(MenuId::EditorTabContext,
                                MakeRect(static_cast<float>(event.button.x),
                                         static_cast<float>(event.button.y), 1.0f, 1.0f));
      }
      return true;
    }
  }

  if (!shell_.BottomPanelVisible()) {
    return false;
  }

  const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                          layout.bottom_panel.w,
                                          kWorkspaceBottomPanelHeaderHeight);
  if (shell_.ActiveTerminalTab() == nullptr ||
      !Contains(panel_header, event.button.x, event.button.y)) {
    return false;
  }

  if (event.button.button == SDL_BUTTON_LEFT &&
      Contains(shell_.BottomPanelTerminalNewTabRect(panel_header), event.button.x,
               event.button.y)) {
    shell_.OpenTerminal({});
    return true;
  }

  for (const VisibleStripTab& tab : shell_.ComputeVisibleTerminalTabs(panel_header)) {
    if (!Contains(tab.rect, event.button.x, event.button.y)) {
      continue;
    }

    if (event.button.button == SDL_BUTTON_MIDDLE ||
        (event.button.button == SDL_BUTTON_LEFT &&
         Contains(tab.close_rect, event.button.x, event.button.y))) {
      shell_.CloseTerminalTab(tab.index);
    } else if (event.button.button == SDL_BUTTON_LEFT) {
      shell_.active_terminal_tab_index_ = tab.index;
      shell_.surface_.focus = FocusTarget::Panel;
      shell_.tab_drag_state_ = TabDragState{
          .kind = TabDragKind::Terminal,
          .press_x = static_cast<float>(event.button.x),
          .press_y = static_cast<float>(event.button.y),
      };
    } else if (event.button.button == SDL_BUTTON_RIGHT) {
      shell_.active_terminal_tab_index_ = tab.index;
      shell_.surface_.focus = FocusTarget::Panel;
      shell_.OpenAnchoredMenu(MenuId::TerminalTabContext,
                              MakeRect(static_cast<float>(event.button.x),
                                       static_cast<float>(event.button.y), 1.0f, 1.0f));
    }
    return true;
  }

  return false;
}

bool WorkspaceShell::TabMouseCoordinator::HandleButtonUp(const SDL_Event& event) {
  if (event.button.button != SDL_BUTTON_LEFT ||
      shell_.tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }

  const TabDragKind kind = shell_.tab_drag_state_.kind;
  const bool reordered = shell_.tab_drag_state_.reordered;
  shell_.ClearTabDrag();
  if (reordered) {
    PersistReorderedTabs(kind);
  }
  return true;
}

bool WorkspaceShell::TabMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (shell_.tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
  if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
    const TabDragKind kind = shell_.tab_drag_state_.kind;
    const bool reordered = shell_.tab_drag_state_.reordered;
    shell_.ClearTabDrag();
    if (reordered) {
      PersistReorderedTabs(kind);
    }
    return false;
  }

  const float delta_x = static_cast<float>(event.motion.x) - shell_.tab_drag_state_.press_x;
  const float delta_y = static_cast<float>(event.motion.y) - shell_.tab_drag_state_.press_y;
  if (!shell_.tab_drag_state_.dragging &&
      std::hypot(delta_x, delta_y) < kTabDragStartDistance) {
    return true;
  }
  shell_.tab_drag_state_.dragging = true;

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;
  switch (shell_.tab_drag_state_.kind) {
    case TabDragKind::Project:
      if (Contains(layout.project_tab_strip, event.motion.x, event.motion.y) &&
          !shell_.project_catalog_.entries.empty()) {
        const auto visible_tabs = shell_.ComputeVisibleProjectTabs(layout.project_tab_strip);
        const std::size_t insertion_slot = StripInsertionSlot(
            layout.project_tab_strip, visible_tabs, static_cast<float>(event.motion.x),
            shell_.project_catalog_.entries.size());
        const std::size_t target_index = MoveTargetIndexForInsertion(
            insertion_slot, shell_.project_catalog_.active_index,
            shell_.project_catalog_.entries.size());
        if (target_index != shell_.project_catalog_.active_index &&
            shell_.MoveActiveProjectTo(target_index)) {
          shell_.tab_drag_state_.reordered = true;
        }
      }
      return true;
    case TabDragKind::Editor:
      if (Contains(layout.tab_strip, event.motion.x, event.motion.y) &&
          !shell_.open_tabs_.empty()) {
        const auto visible_tabs = shell_.ComputeVisibleTabs(layout.tab_strip);
        const std::size_t insertion_slot =
            StripInsertionSlot(layout.tab_strip, visible_tabs,
                               static_cast<float>(event.motion.x), shell_.open_tabs_.size());
        const std::size_t target_index = MoveTargetIndexForInsertion(
            insertion_slot, shell_.active_tab_index_, shell_.open_tabs_.size());
        if (target_index != shell_.active_tab_index_ && shell_.MoveActiveTabTo(target_index)) {
          shell_.tab_drag_state_.reordered = true;
        }
      }
      return true;
    case TabDragKind::Terminal:
      if (shell_.BottomPanelVisible()) {
        const SDL_FRect panel_header =
            MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                     kWorkspaceBottomPanelHeaderHeight);
        if (Contains(panel_header, event.motion.x, event.motion.y) &&
            !shell_.terminal_tabs_.empty()) {
          const auto visible_tabs = shell_.ComputeVisibleTerminalTabs(panel_header);
          const std::size_t insertion_slot = StripInsertionSlot(
              panel_header, visible_tabs, static_cast<float>(event.motion.x),
              shell_.terminal_tabs_.size());
          const std::size_t target_index = MoveTargetIndexForInsertion(
              insertion_slot, shell_.active_terminal_tab_index_, shell_.terminal_tabs_.size());
          if (target_index != shell_.active_terminal_tab_index_ &&
              shell_.MoveActiveTerminalTabTo(target_index)) {
            shell_.tab_drag_state_.reordered = true;
          }
        }
      }
      return true;
    case TabDragKind::None:
    default:
      return false;
  }
}

bool WorkspaceShell::TabMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                      const WorkspaceLayout& layout,
                                                      int vertical_ticks) {
  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !shell_.project_catalog_.entries.empty()) {
    const int max_scroll =
        std::max(0, static_cast<int>(shell_.project_catalog_.entries.size()) - 1);
    shell_.project_catalog_.tab_scroll_index =
        std::clamp(shell_.project_catalog_.tab_scroll_index - vertical_ticks, 0, max_scroll);
    return true;
  }

  if (Contains(layout.tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !shell_.open_tabs_.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(shell_.open_tabs_.size()) - 1);
    shell_.tab_scroll_index_ =
        std::clamp(shell_.tab_scroll_index_ - vertical_ticks, 0, max_scroll);
    return true;
  }

  return false;
}

void WorkspaceShell::TabMouseCoordinator::PersistReorderedTabs(TabDragKind kind) {
  if (kind == TabDragKind::Project) {
    PersistenceCoordinator(shell_).SaveWorkspaceSession();
  } else {
    PersistenceCoordinator(shell_).SaveSessionState();
  }
}

}  // namespace microide::workspace
