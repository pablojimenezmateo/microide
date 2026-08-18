#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "workspace/render/TabStripAnimation.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/TerminalPanelService.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

constexpr float kTabDragStartDistance = 6.0f;

// The result of a press hit-test against a strip, in plain values. Every caller
// then runs an operation that can rebuild the memoized visible list underneath
// it, so nothing may survive the hit-test as a reference into that list.
struct TabHit {
  bool hit = false;
  bool on_close = false;
  std::size_t index = 0;
  SDL_FRect rect{};
};

static TabHit HitStripTab(const std::vector<WorkspaceShell::VisibleStripTab>& tabs,
                          float x,
                          float y) {
  for (const WorkspaceShell::VisibleStripTab& tab : tabs) {
    if (!Contains(tab.rect, x, y)) {
      continue;
    }
    return TabHit{
        .hit = true,
        .on_close = Contains(TabCloseHitRect(tab.close_rect, tab.rect), x, y),
        .index = tab.index,
        .rect = tab.rect,
    };
  }
  return TabHit{};
}

TabMouseCoordinator::EditorGroupTabStrips TabMouseCoordinator::ResolveEditorGroupTabStrips(
    const WorkspaceLayout& layout) const {
  EditorGroupTabStrips strips;
  if (operations_.compute_editor_group_rects) {
    const EditorGroupRectsLayout rects = operations_.compute_editor_group_rects(layout);
    for (std::size_t i = 0; i < rects.groups.size() && strips.count < kMaxEditorGroups; ++i) {
      strips.entries[strips.count++] = {i, rects.groups[i].tab_strip};
    }
  }
  if (strips.count == 0) {
    strips.entries[strips.count++] = {state_.focused_group_index, layout.tab_strip};
  }
  return strips;
}

TabMouseCoordinator::TabMouseCoordinator(ProjectCatalogState& project_catalog,
                                         ProjectWorkspaceState& current_project_state,
                                         TabDragState& tab_drag_state,
                                         TabSlideState& tab_slide_state,
                                         Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      tab_drag_state_(tab_drag_state),
      tab_slide_state_(tab_slide_state),
      operations_(std::move(operations)) {}

bool TabMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                           const WorkspaceLayout& layout) {
  util::PerformanceTrace::Scope perf_scope("TabMouseCoordinator::HandleButtonDown");
  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    const auto& visible_project_tabs =
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
    // The hit tab's POD fields are copied out before any operation runs: the
    // visible list is borrowed from a memoized cache that switch_project /
    // close rebuild, so reading `tab` after one of those is a use-after-free.
    if (const TabHit hit = HitStripTab(visible_project_tabs, event.button.x, event.button.y);
        hit.hit) {
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT && hit.on_close)) {
        operations_.request_close_project(hit.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        operations_.switch_project(hit.index, true);
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Project,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = hit.index,
            .target_slot = hit.index,
            .ghost_width = hit.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - hit.rect.x,
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        operations_.switch_project(hit.index, true);
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
  const EditorGroupTabStrips editor_group_strips =
      ResolveEditorGroupTabStrips(layout);
  for (std::size_t s = 0; s < editor_group_strips.count; ++s) {
    const std::size_t hit_group_index = editor_group_strips.entries[s].group_index;
    const SDL_FRect group_strip = editor_group_strips.entries[s].strip;
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

    const auto& visible_tabs = operations_.compute_visible_tabs(group_strip);
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
    if (const TabHit hit = HitStripTab(visible_tabs, event.button.x, event.button.y); hit.hit) {
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT && hit.on_close)) {
        operations_.request_close_tab(hit.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        {
          util::PerformanceTrace::Scope activate_scope(
              "TabMouseCoordinator::HandleButtonDown::ActivateEditorTab");
          operations_.activate_tab(hit.index);
        }
        // `state_.focused_group_index`, not `hit_group_index`: the focus request
        // above can be refused (no handler bound), and the drag has to agree with
        // whichever group actually owns `activate_tab` / `move_active_tab_to`.
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Editor,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = hit.index,
            .target_slot = hit.index,
            .ghost_width = hit.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - hit.rect.x,
            .source_group_index = state_.focused_group_index,
            .target_group_index = state_.focused_group_index,
        };
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        {
          util::PerformanceTrace::Scope activate_scope(
              "TabMouseCoordinator::HandleButtonDown::ActivateEditorTab");
          operations_.activate_tab(hit.index);
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

  const auto& bottom_panel_tabs = operations_.compute_visible_bottom_panel_tabs(panel_header);
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

  if (const TabHit hit = HitStripTab(bottom_panel_tabs, event.button.x, event.button.y); hit.hit) {
    if (event.button.button == SDL_BUTTON_MIDDLE ||
        (event.button.button == SDL_BUTTON_LEFT && hit.on_close)) {
      operations_.close_bottom_panel_tab(hit.index);
    } else if (event.button.button == SDL_BUTTON_LEFT) {
      if (operations_.activate_bottom_panel_tab(hit.index)) {
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Terminal,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = hit.index,
            .target_slot = hit.index,
            .ghost_width = hit.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - hit.rect.x,
        };
      }
    } else if (event.button.button == SDL_BUTTON_RIGHT) {
      if (operations_.activate_bottom_panel_tab(hit.index) &&
          operations_.bottom_panel_tab_is_terminal(hit.index)) {
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
  FinishDrag();
  operations_.clear_tab_drag();
  return true;
}

bool TabMouseCoordinator::HandleMotion(const SDL_Event& event) {
  if (tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
  if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
    // Button released without a button-up event reaching us: commit what we have.
    FinishDrag();
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
  UpdateDragForPointer();
  return true;
}

bool TabMouseCoordinator::HandleWheel(const SDL_Event& event,
                                      const WorkspaceLayout& layout,
                                      int vertical_ticks) {
  if (vertical_ticks == 0) {
    return false;
  }
  // Wheel and the ⟨ ⟩ overflow buttons scroll the same strip, so they answer to the
  // same stop: keep stepping only while something is still hidden on that side.
  // The wheel used to bypass that and clamp on the raw index instead, so it ran on
  // to the last tab and left most of the strip empty — a state the buttons cannot
  // produce, because they disappear the moment nothing is hidden.
  const auto step_while_hidden = [&](auto&& overflow_for_now, auto&& scroll_one) {
    const int direction = vertical_ticks > 0 ? -1 : 1;
    for (int step = 0; step < std::abs(vertical_ticks); ++step) {
      const auto overflow = overflow_for_now();
      const std::size_t hidden = direction < 0 ? overflow.hidden_left : overflow.hidden_right;
      if (hidden == 0 || !scroll_one(direction)) {
        break;
      }
    }
    return true;
  };

  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !project_catalog_.entries.empty() && operations_.scroll_project_tab_strip &&
      operations_.compute_project_tab_overflow_controls) {
    const SDL_FRect strip = layout.project_tab_strip;
    return step_while_hidden(
        [&]() {
          return operations_.compute_project_tab_overflow_controls(
              strip, operations_.compute_visible_project_tabs(strip));
        },
        [&](int direction) { return operations_.scroll_project_tab_strip(direction); });
  }

  {
    const EditorGroupTabStrips editor_group_strips = ResolveEditorGroupTabStrips(layout);
    for (std::size_t s = 0; s < editor_group_strips.count; ++s) {
      const std::size_t group_index = editor_group_strips.entries[s].group_index;
      const SDL_FRect group_strip = editor_group_strips.entries[s].strip;
      if (!Contains(group_strip, event.wheel.mouse_x, event.wheel.mouse_y) ||
          group_index >= state_.editor_groups.size() ||
          state_.editor_groups[group_index].open_tabs.empty()) {
        continue;
      }
      if (!operations_.scroll_editor_tab_strip || !operations_.compute_tab_overflow_controls) {
        return false;
      }
      // The scroll operation acts on the focused group, so wheeling an unfocused
      // group's strip has to focus it first (a left-click on the strip does the
      // same thing).
      if (group_index != state_.focused_group_index && operations_.focus_editor_group) {
        operations_.focus_editor_group(group_index);
      }
      const SDL_FRect strip = group_strip;
      return step_while_hidden(
          [&]() {
            return operations_.compute_tab_overflow_controls(
                strip, operations_.compute_visible_tabs(strip));
          },
          [&](int direction) { return operations_.scroll_editor_tab_strip(direction); });
    }
  }

  // Bottom-panel tab strip. Gated strictly to the header band so wheeling over
  // the terminal/output content below still scrolls the transcript.
  if (operations_.bottom_panel_visible() && operations_.scroll_bottom_panel_tab_strip &&
      operations_.compute_bottom_panel_tab_overflow_controls) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, event.wheel.mouse_x, event.wheel.mouse_y)) {
      return step_while_hidden(
          [&]() {
            return operations_.compute_bottom_panel_tab_overflow_controls(
                panel_header, operations_.compute_visible_bottom_panel_tabs(panel_header));
          },
          [&](int direction) { return operations_.scroll_bottom_panel_tab_strip(direction); });
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
  // Constructed inside the hooks, not captured: see MakePanelMouseCoordinator for
  // why a captured TerminalPanelService is a heap allocation per hook, and this
  // coordinator is made per mouse-wheel event.
  return TabMouseCoordinator(
      context_.project_catalog,
      context_.current_project_state,
      context_.interaction_state.tab_drag,
      context_.interaction_state.tab_slide,
      TabMouseCoordinator::Operations{
          .compute_visible_project_tabs =
              [this](const SDL_FRect& rect) -> const std::vector<VisibleStripTab>& {
                return tab_strip_chrome_.ComputeVisibleProjectTabs(rect);
              },
          .request_close_project = [this](std::size_t index) { RequestCloseProject(index); },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .compute_visible_tabs =
              [this](const SDL_FRect& rect) -> const std::vector<VisibleStripTab>& {
                return tab_strip_chrome_.ComputeVisibleTabs(rect);
              },
          .compute_visible_tabs_for_group =
              [this](std::size_t group_index,
                     const SDL_FRect& rect) -> const std::vector<VisibleStripTab>& {
                return tab_strip_chrome_.ComputeVisibleTabsForGroup(group_index, rect);
              },
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
              [this](std::string command) {
                MakeTerminalPanelService().OpenTerminal(std::move(command));
              },
          .compute_visible_bottom_panel_tabs =
              [this](const SDL_FRect& rect) -> const std::vector<VisibleStripTab>& {
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
          .move_tab_to_group =
              [this](std::size_t from_group, std::size_t from_index, std::size_t to_group,
                     std::size_t to_slot) {
                // Bound inline rather than through a shell member: nothing else
                // needs this entry point, and the shell's declaration surface is
                // a capped architectural budget. The geometry drop is the same
                // one MoveActiveTabTo does, and for the same reason — both
                // strips change length, and the per-group title/width cache keys
                // only on (tab_count, window_width).
                tab_strip_service_.InvalidateTabStripGeometry();
                return MakeEditorTabService().MoveTabToGroup(from_group, from_index, to_group,
                                                             to_slot);
              },
          .move_tab_to_new_group =
              [this](std::size_t from_group, std::size_t from_index, std::size_t target_group,
                     EditorSplitOrientation orientation, bool insert_before) {
                // Same geometry drop as the cross-group move, and for the same
                // reason: both strips change length and the per-group title/width
                // cache keys only on (tab_count, window_width).
                tab_strip_service_.InvalidateTabStripGeometry();
                return MakeEditorTabService().MoveTabToNewGroup(
                    from_group, from_index, target_group, orientation, insert_before);
              },
          .move_active_terminal_tab_to =
              [this](std::size_t index) {
                return MakeTerminalPanelService().MoveActiveTerminalTabTo(index);
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
          .compute_tab_overflow_controls_for_group =
              [this](std::size_t group_index, const SDL_FRect& strip,
                     const std::vector<VisibleStripTab>& tabs) {
                return tab_strip_chrome_.ComputeTabOverflowControlsForGroup(group_index, strip,
                                                                            tabs);
              },
          .compute_bottom_panel_tab_overflow_controls =
              [this](const SDL_FRect& strip, const std::vector<VisibleStripTab>& tabs) {
                return tab_strip_chrome_.ComputeBottomPanelTabOverflowControls(strip, tabs);
              },
          .scroll_project_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollProjectTabStrip(direction); },
          .scroll_editor_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollEditorTabStrip(direction); },
          .scroll_editor_tab_strip_for_group =
              [this](std::size_t group_index, int direction) {
                return tab_strip_chrome_.ScrollEditorTabStripForGroup(group_index, direction);
              },
          .scroll_bottom_panel_tab_strip =
              [this](int direction) { return tab_strip_chrome_.ScrollBottomPanelTabStrip(direction); },
          .compute_editor_group_rects =
              [this](const WorkspaceLayout& layout) {
                return ComputeEditorGroupRectsForState(layout);
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
