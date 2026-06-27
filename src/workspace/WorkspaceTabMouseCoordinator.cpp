#include "workspace/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "workspace/TerminalPanelService.h"
#include "util/PerformanceTrace.h"
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
  util::PerformanceTrace::Scope perf_scope("TabMouseCoordinator::HandleButtonDown");
  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    const auto visible_project_tabs =
        operations_.compute_visible_project_tabs(layout.project_tab_strip);
    if (event.button.button == SDL_BUTTON_LEFT &&
        operations_.compute_project_tab_overflow_controls) {
      const auto overflow = operations_.compute_project_tab_overflow_controls(
          layout.project_tab_strip, visible_project_tabs);
      if (overflow.hidden_left > 0 &&
          Contains(overflow.left_button, event.button.x, event.button.y)) {
        if (operations_.scroll_project_tab_strip) {
          operations_.scroll_project_tab_strip(-1);
        }
        return true;
      }
      if (overflow.hidden_right > 0 &&
          Contains(overflow.right_button, event.button.x, event.button.y)) {
        if (operations_.scroll_project_tab_strip) {
          operations_.scroll_project_tab_strip(+1);
        }
        return true;
      }
    }
    for (const WorkspaceShell::VisibleStripTab& tab : visible_project_tabs) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(TabCloseHitRect(tab.close_rect, tab.rect), event.button.x, event.button.y))) {
        operations_.request_close_project(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        operations_.switch_project(tab.index, true);
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Project,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = tab.index,
            .target_slot = tab.index,
            .ghost_width = tab.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - tab.rect.x,
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        operations_.switch_project(tab.index, true);
        operations_.open_anchored_menu(
            MenuId::ProjectTabContext,
            MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                     1.0f));
      }
      return true;
    }
  }

  // Editor tab strips, one per group. Resolve which group's strip the pointer hit
  // (falls back to the global tab strip when the per-group op is unavailable) and
  // focus that group before activating/closing/scrolling within it.
  std::vector<std::pair<std::size_t, SDL_FRect>> editor_group_strips;
  if (operations_.compute_editor_group_tab_strips) {
    editor_group_strips = operations_.compute_editor_group_tab_strips();
  }
  if (editor_group_strips.empty()) {
    editor_group_strips.emplace_back(state_.focused_group_index, layout.tab_strip);
  }
  for (const auto& [hit_group_index, group_strip] : editor_group_strips) {
    if (!Contains(group_strip, event.button.x, event.button.y)) {
      continue;
    }
    if (state_.root.empty()) {
      return false;
    }
    if (hit_group_index != state_.focused_group_index && operations_.focus_editor_group) {
      operations_.focus_editor_group(hit_group_index);
    }
    if (state_.focused_group().open_tabs.empty()) {
      const SDL_FRect placeholder_tab = EmptyTabStripPlaceholderRect(group_strip);
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(placeholder_tab, event.button.x, event.button.y)) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
      return false;
    }

    const auto visible_tabs = operations_.compute_visible_tabs(group_strip);
    if (event.button.button == SDL_BUTTON_LEFT &&
        operations_.compute_tab_overflow_controls) {
      const auto overflow =
          operations_.compute_tab_overflow_controls(group_strip, visible_tabs);
      if (overflow.hidden_left > 0 &&
          Contains(overflow.left_button, event.button.x, event.button.y)) {
        if (operations_.scroll_editor_tab_strip) {
          operations_.scroll_editor_tab_strip(-1);
        }
        return true;
      }
      if (overflow.hidden_right > 0 &&
          Contains(overflow.right_button, event.button.x, event.button.y)) {
        if (operations_.scroll_editor_tab_strip) {
          operations_.scroll_editor_tab_strip(+1);
        }
        return true;
      }
    }
    for (const WorkspaceShell::VisibleStripTab& tab : visible_tabs) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(TabCloseHitRect(tab.close_rect, tab.rect), event.button.x, event.button.y))) {
        operations_.request_close_tab(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        {
          util::PerformanceTrace::Scope activate_scope(
              "TabMouseCoordinator::HandleButtonDown::ActivateEditorTab");
          operations_.activate_tab(tab.index);
        }
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Editor,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = tab.index,
            .target_slot = tab.index,
            .ghost_width = tab.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - tab.rect.x,
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        {
          util::PerformanceTrace::Scope activate_scope(
              "TabMouseCoordinator::HandleButtonDown::ActivateEditorTab");
          operations_.activate_tab(tab.index);
        }
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

  const auto bottom_panel_tabs = operations_.compute_visible_bottom_panel_tabs(panel_header);
  if (event.button.button == SDL_BUTTON_LEFT &&
      operations_.compute_bottom_panel_tab_overflow_controls) {
    const auto overflow =
        operations_.compute_bottom_panel_tab_overflow_controls(panel_header, bottom_panel_tabs);
    if (overflow.hidden_left > 0 &&
        Contains(overflow.left_button, event.button.x, event.button.y)) {
      if (operations_.scroll_bottom_panel_tab_strip) {
        operations_.scroll_bottom_panel_tab_strip(-1);
      }
      return true;
    }
    if (overflow.hidden_right > 0 &&
        Contains(overflow.right_button, event.button.x, event.button.y)) {
      if (operations_.scroll_bottom_panel_tab_strip) {
        operations_.scroll_bottom_panel_tab_strip(+1);
      }
      return true;
    }
  }

  for (const WorkspaceShell::VisibleStripTab& tab : bottom_panel_tabs) {
    if (!Contains(tab.rect, event.button.x, event.button.y)) {
      continue;
    }

    if (event.button.button == SDL_BUTTON_MIDDLE ||
        (event.button.button == SDL_BUTTON_LEFT &&
         Contains(TabCloseHitRect(tab.close_rect, tab.rect), event.button.x, event.button.y))) {
      operations_.close_bottom_panel_tab(tab.index);
    } else if (event.button.button == SDL_BUTTON_LEFT) {
      if (operations_.activate_bottom_panel_tab(tab.index)) {
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Terminal,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = tab.index,
            .target_slot = tab.index,
            .ghost_width = tab.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - tab.rect.x,
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
  CommitDrag();
  operations_.clear_tab_drag();
  return true;
}

TabMouseCoordinator::DragStrip TabMouseCoordinator::ResolveDragStrip(const WorkspaceLayout& layout,
                                                                     TabDragKind kind) {
  DragStrip d;
  switch (kind) {
    case TabDragKind::Project:
      if (project_catalog_.entries.empty()) {
        return d;
      }
      d.strip = layout.project_tab_strip;
      d.tabs = operations_.compute_visible_project_tabs(d.strip);
      d.count = project_catalog_.entries.size();
      d.active = project_catalog_.active_index;
      d.move = operations_.move_active_project_to;
      d.valid = true;
      return d;
    case TabDragKind::Editor:
      if (state_.focused_group().open_tabs.empty()) {
        return d;
      }
      d.strip = layout.tab_strip;
      if (operations_.compute_editor_group_tab_strips) {
        for (const auto& [group_index, group_strip] :
             operations_.compute_editor_group_tab_strips()) {
          if (group_index == state_.focused_group_index) {
            d.strip = group_strip;
            break;
          }
        }
      }
      d.tabs = operations_.compute_visible_tabs(d.strip);
      d.count = state_.focused_group().open_tabs.size();
      d.active = state_.focused_group().active_tab_index;
      d.move = operations_.move_active_tab_to;
      d.valid = true;
      return d;
    case TabDragKind::Terminal:
      return ResolveBottomPanelDragStrip(layout);
    case TabDragKind::None:
    default:
      return d;
  }
}

TabMouseCoordinator::DragStrip TabMouseCoordinator::ResolveBottomPanelDragStrip(
    const WorkspaceLayout& layout) {
  DragStrip d;
  if (!operations_.bottom_panel_visible()) {
    return d;
  }
  const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                          layout.bottom_panel.w, kWorkspaceBottomPanelHeaderHeight);
  d.strip = panel_header;
  d.tabs = operations_.compute_visible_bottom_panel_tabs(panel_header);

  // Bottom-panel tabs are laid out terminals-first then output channels. The
  // dragged tab's kind follows the active panel content (button-down activated
  // it), and reordering stays within that kind's contiguous model range.
  const std::size_t terminal_count = state_.terminal_tabs.size();
  if (state_.panel.content == PanelContentKind::Output) {
    std::vector<std::string> output_ids = state_.panel.output.open_channel_ids;
    if (!state_.panel.output.channel_id.empty() &&
        std::find(output_ids.begin(), output_ids.end(), state_.panel.output.channel_id) ==
            output_ids.end()) {
      output_ids.push_back(state_.panel.output.channel_id);
    }
    if (output_ids.size() < 2) {
      return d;  // nothing to reorder
    }
    const auto active_it =
        std::find(output_ids.begin(), output_ids.end(), state_.panel.output.channel_id);
    d.count = output_ids.size();
    d.active = active_it == output_ids.end()
                   ? 0
                   : static_cast<std::size_t>(active_it - output_ids.begin());
    d.model_offset = terminal_count;
    d.move = operations_.move_active_output_tab_to;
    d.valid = static_cast<bool>(d.move);
    return d;
  }

  if (state_.terminal_tabs.empty()) {
    return d;
  }
  d.count = terminal_count;
  d.active = state_.active_terminal_tab_index;
  d.model_offset = 0;
  d.move = operations_.move_active_terminal_tab_to;
  d.valid = true;
  return d;
}

bool TabMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
  if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
    // Button released without a button-up event reaching us: commit what we have.
    CommitDrag();
    operations_.clear_tab_drag();
    return false;
  }

  const float delta_x = static_cast<float>(event.motion.x) - tab_drag_state_.press_x;
  const float delta_y = static_cast<float>(event.motion.y) - tab_drag_state_.press_y;
  if (!tab_drag_state_.dragging && std::hypot(delta_x, delta_y) < kTabDragStartDistance) {
    return true;
  }
  tab_drag_state_.dragging = true;
  tab_drag_state_.pointer_x = static_cast<float>(event.motion.x);
  tab_drag_state_.pointer_y = static_cast<float>(event.motion.y);

  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return true;
  }
  const DragStrip d = ResolveDragStrip(*layout_state, tab_drag_state_.kind);
  if (!d.valid) {
    return true;
  }

  // Deferred commit: the model is NOT mutated during the drag. We compute the
  // target insertion slot for the live pointer (the strip "captures" the pointer
  // horizontally so a drop past either edge still pins to start/end) and record
  // it; the floating ghost + insertion caret render from this. A single reorder
  // is committed on release. `total_for_slot` is the end of this kind's range so
  // a mixed terminal/output strip never lets a slot escape its own kind.
  const std::size_t total_for_slot = d.model_offset + d.count;
  const std::size_t model_slot = StripInsertionSlot(d.strip, d.tabs,
                                                    static_cast<float>(event.motion.x), total_for_slot);
  const std::size_t clamped_model_slot = std::clamp(model_slot, d.model_offset, total_for_slot);
  tab_drag_state_.target_slot = clamped_model_slot;
  const std::size_t list_slot = clamped_model_slot - d.model_offset;
  const std::size_t target = MoveTargetIndexForInsertion(list_slot, d.active, d.count);
  tab_drag_state_.reordered = target != d.active;
  return true;
}

void TabMouseCoordinator::CommitDrag() {
  if (!tab_drag_state_.dragging || !tab_drag_state_.reordered) {
    return;
  }
  const TabDragKind kind = tab_drag_state_.kind;
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const DragStrip d = ResolveDragStrip(*layout_state, kind);
  if (!d.valid || !d.move) {
    return;
  }
  const std::size_t list_slot = tab_drag_state_.target_slot >= d.model_offset
                                    ? tab_drag_state_.target_slot - d.model_offset
                                    : 0;
  const std::size_t target = MoveTargetIndexForInsertion(list_slot, d.active, d.count);
  if (target == d.active) {
    return;
  }
  if (d.move(target)) {
    PersistReorderedTabs(kind);
  }
}

bool TabMouseCoordinator::HandleWheel(const SDL_Event& event,
                                      const WorkspaceLayout& layout,
                                      int vertical_ticks) {
  const auto scroll_strip = [vertical_ticks](std::size_t entry_count, int& scroll_index) {
    const int max_scroll = std::max(0, static_cast<int>(entry_count) - 1);
    scroll_index = std::clamp(scroll_index - vertical_ticks, 0, max_scroll);
  };

  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !project_catalog_.entries.empty()) {
    scroll_strip(project_catalog_.entries.size(), project_catalog_.tab_scroll_index);
    return true;
  }

  {
    std::vector<std::pair<std::size_t, SDL_FRect>> editor_group_strips;
    if (operations_.compute_editor_group_tab_strips) {
      editor_group_strips = operations_.compute_editor_group_tab_strips();
    }
    if (editor_group_strips.empty()) {
      editor_group_strips.emplace_back(state_.focused_group_index, layout.tab_strip);
    }
    for (const auto& [group_index, group_strip] : editor_group_strips) {
      if (!Contains(group_strip, event.wheel.mouse_x, event.wheel.mouse_y) ||
          group_index >= state_.editor_groups.size() ||
          state_.editor_groups[group_index].open_tabs.empty()) {
        continue;
      }
      EditorGroup& group = state_.editor_groups[group_index];
      scroll_strip(group.open_tabs.size(), group.tab_scroll_index);
      return true;
    }
  }

  // Bottom-panel tab strip. Gated strictly to the header band so wheeling over
  // the terminal/output content below still scrolls the transcript.
  if (operations_.bottom_panel_visible() && operations_.scroll_bottom_panel_tab_strip &&
      vertical_ticks != 0) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, event.wheel.mouse_x, event.wheel.mouse_y)) {
      const int steps = std::abs(vertical_ticks);
      for (int i = 0; i < steps; ++i) {
        operations_.scroll_bottom_panel_tab_strip(vertical_ticks > 0 ? -1 : 1);
      }
      return true;
    }
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
              [this](const SDL_FRect& rect) { return tab_strip_chrome_.ComputeVisibleProjectTabs(rect); },
          .request_close_project = [this](std::size_t index) { RequestCloseProject(index); },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .compute_visible_tabs = [this](const SDL_FRect& rect) { return tab_strip_chrome_.ComputeVisibleTabs(rect); },
          .request_close_tab = [this](std::size_t index) { RequestCloseTab(index); },
          .activate_tab = [this](std::size_t index) { ActivateTab(index); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, rect);
              },
          .bottom_panel_visible = [this]() { return BottomPanelVisible(); },
          .bottom_panel_terminal_new_tab_rect =
              [this](const SDL_FRect& rect) {
                return tab_strip_service_.BottomPanelTerminalNewTabRect(
                    layout_mode_service_.CurrentMode(), rect);
              },
          .open_terminal =
              [terminal_panel](std::string command) mutable {
                terminal_panel.OpenTerminal(std::move(command));
              },
          .compute_visible_bottom_panel_tabs =
              [this](const SDL_FRect& rect) {
                return tab_strip_service_.ComputeVisibleBottomPanelTabs(
                    context_.current_project_state, rect, layout_mode_service_.CurrentMode(),
                    [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
                    output_channels_.Channels());
              },
          .activate_bottom_panel_tab =
              [this](std::size_t index) { return tab_strip_chrome_.ActivateBottomPanelTab(index); },
          .close_bottom_panel_tab =
              [this](std::size_t index) { return tab_strip_chrome_.CloseBottomPanelTab(index); },
          .bottom_panel_tab_is_terminal =
              [this](std::size_t index) {
                return tab_strip_service_.BottomPanelTabIsTerminal(
                    context_.current_project_state, index, output_channels_.Channels());
              },
          .clear_tab_drag = [this]() { context_.interaction_state.tab_drag = TabDragState{}; },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .move_active_project_to = [this](std::size_t index) { return MoveActiveProjectTo(index); },
          .move_active_tab_to = [this](std::size_t index) { return MoveActiveTabTo(index); },
          .move_active_terminal_tab_to =
              [terminal_panel](std::size_t index) mutable {
                return terminal_panel.MoveActiveTerminalTabTo(index);
              },
          .move_active_output_tab_to =
              [this](std::size_t index) { return MoveActiveOutputTabTo(index); },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .save_session_state = [this]() { MakePersistenceCoordinator().SaveSessionState(); },
          .compute_project_tab_overflow_controls =
              [this](const SDL_FRect& strip, const std::vector<VisibleStripTab>& tabs) {
                return tab_strip_chrome_.ComputeProjectTabOverflowControls(strip, tabs);
              },
          .compute_tab_overflow_controls =
              [this](const SDL_FRect& strip, const std::vector<VisibleStripTab>& tabs) {
                return tab_strip_chrome_.ComputeTabOverflowControls(strip, tabs);
              },
          .compute_bottom_panel_tab_overflow_controls =
              [this](const SDL_FRect& strip, const std::vector<VisibleStripTab>& tabs) {
                return tab_strip_chrome_.ComputeBottomPanelTabOverflowControls(strip, tabs);
              },
          .scroll_project_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollProjectTabStrip(direction); },
          .scroll_editor_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollEditorTabStrip(direction); },
          .scroll_bottom_panel_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollBottomPanelTabStrip(direction); },
          .compute_editor_group_tab_strips =
              [this]() {
                std::vector<std::pair<std::size_t, SDL_FRect>> strips;
                const auto layout_state = CurrentWorkspaceLayout();
                if (!layout_state.has_value()) {
                  return strips;
                }
                const auto group_rects = ComputeEditorGroupRectsForState(*layout_state);
                for (std::size_t i = 0; i < group_rects.groups.size(); ++i) {
                  strips.emplace_back(i, group_rects.groups[i].tab_strip);
                }
                return strips;
              },
          .focus_editor_group =
              [this](std::size_t group_index) { FocusEditorGroup(group_index); },
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
