#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "workspace/render/TabStripAnimation.h"
#include "workspace/services/TerminalPanelService.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

constexpr float kTabDragStartDistance = 6.0f;
// How close to a strip edge the pointer has to get before the strip starts
// walking under it, and how fast it walks. One tab per ~90ms reads as deliberate
// rather than as a slip.
constexpr float kDragAutoScrollEdgePx = 28.0f;
constexpr Uint64 kDragAutoScrollIntervalMs = 90;

// Which slot the dragged tab would land in, given `x` — the center of its
// rendered box, NOT the pointer. Chrome and VS Code both resolve the drop from
// the tab the user can see; keying it off the raw cursor made the landing spot
// depend on where inside the tab it was grabbed, so grabbing a wide tab near an
// edge felt like the strip was lagging behind the drag by half a tab.
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
  // The first tab whose resting midpoint is right of `x`. A scrolled strip is a
  // window onto the list, so both ends pin to what is VISIBLE: answering 0 /
  // item_count for a drop past either visible edge teleported the tab to a slot
  // that is not on screen. Drag auto-scroll is what walks the window further.
  for (const VisibleTabType& tab : tabs) {
    const float midpoint = tab.rect.x + tab.rect.w * 0.5f;
    if (x < midpoint) {
      return tab.index;
    }
  }
  return std::min(item_count, tabs.back().index + 1);
}

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

static std::size_t MoveTargetIndexForInsertion(std::size_t insertion_slot,
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
        tab_drag_state_ = TabDragState{
            .kind = TabDragKind::Editor,
            .press_x = static_cast<float>(event.button.x),
            .press_y = static_cast<float>(event.button.y),
            .source_index = hit.index,
            .target_slot = hit.index,
            .ghost_width = hit.rect.w,
            .grab_offset_x = static_cast<float>(event.button.x) - hit.rect.x,
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

TabMouseCoordinator::DragStrip TabMouseCoordinator::ResolveDragStrip(const WorkspaceLayout& layout,
                                                                     TabDragKind kind) {
  DragStrip d;
  switch (kind) {
    case TabDragKind::Project:
      if (project_catalog_.entries.empty()) {
        return d;
      }
      d.strip = layout.project_tab_strip;
      d.tabs = &operations_.compute_visible_project_tabs(d.strip);
      d.count = project_catalog_.entries.size();
      d.active = project_catalog_.active_index;
      d.move = operations_.move_active_project_to;
      d.scroll = operations_.scroll_project_tab_strip;
      d.overflow = operations_.compute_project_tab_overflow_controls;
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
      d.tabs = &operations_.compute_visible_tabs(d.strip);
      d.count = state_.focused_group().open_tabs.size();
      d.active = state_.focused_group().active_tab_index;
      d.move = operations_.move_active_tab_to;
      d.scroll = operations_.scroll_editor_tab_strip;
      d.overflow = operations_.compute_tab_overflow_controls;
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
  d.tabs = &operations_.compute_visible_bottom_panel_tabs(panel_header);
  d.scroll = operations_.scroll_bottom_panel_tab_strip;
  d.overflow = operations_.compute_bottom_panel_tab_overflow_controls;

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

bool TabMouseCoordinator::CancelDrag() {
  if (tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
  // FinishDrag already knows how to drop without moving anything and glide the
  // lifted tab home from wherever the ghost is; clearing `reordered` is the whole
  // difference between abandoning a drag and completing one.
  tab_drag_state_.reordered = false;
  FinishDrag();
  operations_.clear_tab_drag();
  return true;
}

bool TabMouseCoordinator::TickDragAutoScroll() {
  if (tab_drag_state_.kind == TabDragKind::None || !tab_drag_state_.dragging ||
      tab_drag_state_.autoscroll_direction == 0) {
    return false;
  }
  return UpdateDragForPointer();
}

bool TabMouseCoordinator::AutoScrollDragStrip(const DragStrip& d) {
  // VS Code walks an overflowing strip under a pointer parked at its edge. Without
  // it the drop slot now pins to the last VISIBLE tab, which would leave the
  // off-screen end of the strip unreachable by drag.
  if (!d.scroll || !d.overflow || d.tabs == nullptr) {
    tab_drag_state_.autoscroll_direction = 0;
    return false;
  }
  const float x = tab_drag_state_.pointer_x;
  int direction = 0;
  if (x < d.strip.x + kDragAutoScrollEdgePx) {
    direction = -1;
  } else if (x > d.strip.x + d.strip.w - kDragAutoScrollEdgePx) {
    direction = +1;
  }
  if (direction == 0) {
    tab_drag_state_.autoscroll_direction = 0;
    return false;
  }
  const TabStripOverflowControls overflow = d.overflow(d.strip, *d.tabs);
  const std::size_t hidden = direction < 0 ? overflow.hidden_left : overflow.hidden_right;
  if (hidden == 0) {
    tab_drag_state_.autoscroll_direction = 0;
    return false;
  }
  const Uint64 now = SDL_GetTicks();
  if (tab_drag_state_.autoscroll_direction == direction &&
      now - tab_drag_state_.last_autoscroll_ms < kDragAutoScrollIntervalMs) {
    return false;  // armed, but not due yet
  }
  tab_drag_state_.autoscroll_direction = direction;
  tab_drag_state_.last_autoscroll_ms = now;
  return d.scroll(direction);
}

bool TabMouseCoordinator::UpdateDragForPointer() {
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return false;
  }
  DragStrip d = ResolveDragStrip(*layout_state, tab_drag_state_.kind);
  if (!d.valid) {
    return false;
  }
  bool scrolled = false;
  if (AutoScrollDragStrip(d)) {
    // Every tab just moved: the borrowed visible list and the slide bases below
    // both have to come from the post-scroll geometry.
    d = ResolveDragStrip(*layout_state, tab_drag_state_.kind);
    if (!d.valid) {
      return false;
    }
    scrolled = true;
  }

  // Deferred commit: the model is NOT mutated during the drag. We compute the
  // target insertion slot from the dragged tab's own rendered box (the ghost is
  // pinned inside the strip, so a drop past either edge still resolves to the
  // visible start/end) and record it; the floating ghost renders from this. A
  // single reorder is committed on release. `total_for_slot` is the end of this
  // kind's range so a mixed terminal/output strip never lets a slot escape its
  // own kind.
  const std::size_t total_for_slot = d.model_offset + d.count;
  // Probe with the CENTER of the dragged tab, not the pointer — and unclamped, so
  // pulling further past an edge than the pinned ghost can render still reads as
  // "put it first / last". The ghost's own x is clamped for paint only.
  const float probe_x = tab_drag_state_.pointer_x - tab_drag_state_.grab_offset_x +
                        tab_drag_state_.ghost_width * 0.5f;
  const std::size_t model_slot = StripInsertionSlot(d.strip, *d.tabs, probe_x, total_for_slot);
  const std::size_t clamped_model_slot = std::clamp(model_slot, d.model_offset, total_for_slot);
  const bool slot_changed =
      !tab_drag_state_.slide_seeded || clamped_model_slot != tab_drag_state_.target_slot;
  tab_drag_state_.target_slot = clamped_model_slot;
  const std::size_t list_slot = clamped_model_slot - d.model_offset;
  const std::size_t target = MoveTargetIndexForInsertion(list_slot, d.active, d.count);
  tab_drag_state_.reordered = target != d.active;
  // Rebuilding the whole displaced layout on every motion event is wasted work
  // when the pointer moved within one slot, which is most of them; the ease still
  // has to step so the neighbors keep gliding between slot changes.
  if (slot_changed || scrolled) {
    SeedSlideTargets(d, clamped_model_slot);
    tab_drag_state_.slide_seeded = true;
  } else {
    AdvanceSlideOnInputThread();
  }
  return true;
}

void TabMouseCoordinator::SeedSlideTargets(const DragStrip& d, std::size_t insertion_slot) {
  // Offsets are indexed by absolute model index; only the visible subset moves.
  const std::size_t total = d.model_offset + d.count;

  std::vector<SlideTab> slide_tabs;
  slide_tabs.reserve(d.tabs->size());
  for (const auto& tab : *d.tabs) {
    slide_tabs.push_back(SlideTab{.index = tab.index, .x = tab.rect.x, .width = tab.rect.w});
  }
  const std::vector<float> target_xs = ComputeSlideTargetXs(
      slide_tabs, tab_drag_state_.source_index, insertion_slot, tab_drag_state_.ghost_width,
      /*gap=*/1.0f);

  const bool fresh = tab_slide_state_.kind != tab_drag_state_.kind ||
                     tab_slide_state_.settling || tab_slide_state_.current.size() != total;
  if (fresh) {
    tab_slide_state_.current.assign(total, 0.0f);
    tab_slide_state_.last_advance_ms = SDL_GetTicks();
  }
  tab_slide_state_.kind = tab_drag_state_.kind;
  tab_slide_state_.group_index = state_.focused_group_index;
  tab_slide_state_.settling = false;
  tab_slide_state_.target.assign(total, 0.0f);
  for (std::size_t i = 0; i < slide_tabs.size(); ++i) {
    const std::size_t idx = slide_tabs[i].index;
    if (idx < total && idx != tab_drag_state_.source_index) {
      tab_slide_state_.target[idx] = target_xs[i] - slide_tabs[i].x;
    }
  }
  AdvanceSlideOnInputThread();
}

void TabMouseCoordinator::AdvanceSlideOnInputThread() {
  const Uint64 now = SDL_GetTicks();
  const Uint64 raw_dt =
      now >= tab_slide_state_.last_advance_ms ? now - tab_slide_state_.last_advance_ms : 0;
  const float dt_ms = static_cast<float>(std::min<Uint64>(raw_dt, 100));
  AdvanceSlideOffsets(tab_slide_state_.current, tab_slide_state_.target, dt_ms);
  tab_slide_state_.last_advance_ms = now;
}

void TabMouseCoordinator::FinishDrag() {
  // No live drag (a plain click, or a sub-threshold press): nothing to commit and
  // nothing to glide.
  if (!tab_drag_state_.dragging) {
    tab_slide_state_ = TabSlideState{};
    return;
  }
  const TabDragKind kind = tab_drag_state_.kind;
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    tab_slide_state_ = TabSlideState{};
    return;
  }
  const DragStrip before = ResolveDragStrip(*layout_state, kind);
  // The drag moves whichever item is active in its kind's list, because the press
  // activated the tab under it. If that activation was refused or something moved
  // the model mid-drag, the active item is no longer the tab being dragged and a
  // commit here would reorder a bystander.
  if (!before.valid || before.model_offset + before.active != tab_drag_state_.source_index) {
    tab_slide_state_ = TabSlideState{};
    return;
  }

  const std::size_t list_slot = tab_drag_state_.target_slot >= before.model_offset
                                    ? tab_drag_state_.target_slot - before.model_offset
                                    : 0;
  const std::size_t target = MoveTargetIndexForInsertion(list_slot, before.active, before.count);
  const bool moved = tab_drag_state_.reordered && target != before.active && before.move &&
                     before.move(target);
  if (moved) {
    PersistReorderedTabs(kind);
  }

  // Re-resolve against the (possibly reordered) model so the dropped tab's new
  // resting x is known; the dropped tab is the still-active one in its kind list.
  const DragStrip after = ResolveDragStrip(*layout_state, kind);
  if (!after.valid) {
    tab_slide_state_ = TabSlideState{};
    return;
  }

  const std::size_t total = after.model_offset + after.count;
  const std::size_t source_index = tab_drag_state_.source_index;
  const std::size_t dropped_index = after.model_offset + after.active;
  const float ghost_x = ClampedGhostX(after.strip.x, after.strip.w, tab_drag_state_.ghost_width,
                                      tab_drag_state_.pointer_x, tab_drag_state_.grab_offset_x);

  // Carry the neighbors' unfinished ease across the drop. Each was rendered at
  // `base + current`; a committed reorder makes `base + target` its new base, so
  // its residual is `current - target`, and an abandoned drag leaves the base
  // alone, so its residual is `current` and it eases back to rest. Zeroing them
  // here teleported every neighbor that had not finished gliding, which is most
  // of them on a brisk drop.
  const std::vector<float> previous_current = tab_slide_state_.current;
  const std::vector<float> previous_target = tab_slide_state_.target;
  // Pre-reorder model index -> post-reorder model index for the single move that
  // just happened (identity when nothing moved).
  const auto post_index = [moved, source_index, dropped_index](std::size_t i) -> std::size_t {
    if (!moved) {
      return i;
    }
    if (i == source_index) {
      return dropped_index;
    }
    if (source_index < dropped_index) {
      return (i > source_index && i <= dropped_index) ? i - 1 : i;
    }
    return (i >= dropped_index && i < source_index) ? i + 1 : i;
  };

  tab_slide_state_.kind = kind;
  tab_slide_state_.group_index = state_.focused_group_index;
  tab_slide_state_.settling = true;
  tab_slide_state_.target.assign(total, 0.0f);
  tab_slide_state_.current.assign(total, 0.0f);
  tab_slide_state_.last_advance_ms = SDL_GetTicks();
  for (std::size_t i = 0; i < previous_current.size(); ++i) {
    if (i == source_index) {
      continue;
    }
    const float committed_shift =
        moved && i < previous_target.size() ? previous_target[i] : 0.0f;
    if (const std::size_t p = post_index(i); p < total) {
      tab_slide_state_.current[p] = previous_current[i] - committed_shift;
    }
  }
  for (const auto& tab : *after.tabs) {
    if (tab.index == dropped_index) {
      if (dropped_index < total) {
        tab_slide_state_.current[dropped_index] = ghost_x - tab.rect.x;
      }
      break;
    }
  }
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
