#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/TabStripAnimation.h"

// The drag half of the tab mouse coordinator: strip resolution, the deferred
// commit, drag auto-scroll and the Chrome-like slide animation. Split out of
// WorkspaceTabMouseCoordinator.cpp (which keeps press/release/motion/wheel
// dispatch and the shell factory) to stay under the coordinator TU cap; the
// split is along the seam the two halves already had, not an arbitrary cut.

namespace microide::workspace {

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
      return ResolveEditorDragStrip(layout, tab_drag_state_.source_group_index);
    case TabDragKind::Terminal:
      return ResolveBottomPanelDragStrip(layout);
    case TabDragKind::None:
    default:
      return d;
  }
}

TabMouseCoordinator::DragStrip TabMouseCoordinator::ResolveEditorDragStrip(
    const WorkspaceLayout& layout,
    std::size_t group_index) {
  DragStrip d;
  if (group_index >= state_.editor_groups.size() ||
      state_.editor_groups[group_index].open_tabs.empty()) {
    return d;
  }
  d.group_index = group_index;
  d.strip = layout.tab_strip;
  {
    const EditorGroupTabStrips strips = ResolveEditorGroupTabStrips(layout);
    for (std::size_t s = 0; s < strips.count; ++s) {
      if (strips.entries[s].group_index == group_index) {
        d.strip = strips.entries[s].strip;
        break;
      }
    }
  }
  // The per-group list, not the focused-group one: a cross-group drag resolves
  // the destination strip while focus is still on the source, and asking for the
  // focused group's tabs there would lay the wrong list out over the wrong rect.
  d.tabs = operations_.compute_visible_tabs_for_group
               ? &operations_.compute_visible_tabs_for_group(group_index, d.strip)
               : &operations_.compute_visible_tabs(d.strip);
  d.count = state_.editor_groups[group_index].open_tabs.size();
  d.active = state_.editor_groups[group_index].active_tab_index;
  // `move` reorders within the FOCUSED group, so it is only the right commit for
  // the group that owns focus; a cross-group drop goes through move_tab_to_group
  // instead and never reads this.
  d.move = group_index == state_.focused_group_index ? operations_.move_active_tab_to
                                                     : std::function<bool(std::size_t)>{};
  if (operations_.scroll_editor_tab_strip_for_group) {
    d.scroll = [this, group_index](int direction) {
      return operations_.scroll_editor_tab_strip_for_group(group_index, direction);
    };
  } else if (group_index == state_.focused_group_index) {
    d.scroll = operations_.scroll_editor_tab_strip;
  }
  if (operations_.compute_tab_overflow_controls_for_group) {
    d.overflow = [this, group_index](const SDL_FRect& strip,
                                     const std::vector<WorkspaceShell::VisibleStripTab>& tabs) {
      return operations_.compute_tab_overflow_controls_for_group(group_index, strip, tabs);
    };
  } else {
    d.overflow = operations_.compute_tab_overflow_controls;
  }
  d.valid = true;
  return d;
}

std::size_t TabMouseCoordinator::ResolvePointerEditorGroup(
    const WorkspaceLayout& layout,
    const EditorGroupRectsLayout& rects) const {
  const float x = tab_drag_state_.pointer_x;
  const float y = tab_drag_state_.pointer_y;
  for (std::size_t i = 0; i < rects.groups.size(); ++i) {
    if (Contains(rects.groups[i].tab_strip, x, y)) {
      return i;
    }
  }
  // Same fallback ResolveEditorGroupTabStrips carries for a host that cannot hand
  // out per-group rects: one strip, the focused group's, over the global band.
  if (rects.groups.empty() && Contains(layout.tab_strip, x, y)) {
    return state_.focused_group_index;
  }
  // Off every strip — dragged down into the editor body, or out of the window.
  // VS Code keeps the last strip's insertion feedback in that state rather than
  // snapping the drop back to the source, so the target group is sticky.
  return tab_drag_state_.target_group_index;
}

// How much of a pane's width/height at each edge reads as "split here" rather
// than "move into this group". VS Code uses roughly a fifth; below that the
// center target gets hard to hit on a narrow pane.
constexpr float kBodyDropEdgeFraction = 0.2f;

void TabMouseCoordinator::ResolveEditorBodyDrop(const EditorGroupRectsLayout& rects) {
  tab_drag_state_.body_drop_zone = EditorBodyDropZone::None;
  if (tab_drag_state_.kind != TabDragKind::Editor || !tab_drag_state_.dragging) {
    return;
  }
  const float x = tab_drag_state_.pointer_x;
  const float y = tab_drag_state_.pointer_y;
  // A pointer still on a strip is a strip drop, which owns its own feedback; the
  // body zones start below the strip the pane belongs to.
  for (std::size_t gi = 0; gi < rects.groups.size(); ++gi) {
    if (Contains(rects.groups[gi].tab_strip, x, y)) {
      return;
    }
  }

  for (std::size_t gi = 0; gi < rects.groups.size() && gi < state_.editor_groups.size(); ++gi) {
    const SDL_FRect pane = rects.groups[gi].editor_surface;
    if (pane.w <= 0.0f || pane.h <= 0.0f || !Contains(pane, x, y)) {
      continue;
    }
    // Splitting ADDS a pane, so it is offered exactly while the editor area is
    // under the cap. Splitting the source pane with its own only tab is the one
    // no-op: the carved group would be that group renamed, and the emptied source
    // would collapse straight back. Dropping another pane's last tab on an edge is
    // a real move -- that pane goes away and this one gains a neighbour.
    const bool can_split = !state_.editor_split.full() &&
                           (gi != tab_drag_state_.source_group_index ||
                            state_.editor_groups[gi].open_tabs.size() >= 2);
    EditorBodyDropZone zone = EditorBodyDropZone::Center;
    SDL_FRect region = pane;
    if (can_split) {
      const float left = (x - pane.x) / pane.w;
      const float top = (y - pane.y) / pane.h;
      const float right = 1.0f - left;
      const float bottom = 1.0f - top;
      const float nearest = std::min(std::min(left, right), std::min(top, bottom));
      if (nearest < kBodyDropEdgeFraction) {
        const float half_w = pane.w * 0.5f;
        const float half_h = pane.h * 0.5f;
        if (nearest == left) {
          zone = EditorBodyDropZone::Left;
          region = MakeRect(pane.x, pane.y, half_w, pane.h);
        } else if (nearest == right) {
          zone = EditorBodyDropZone::Right;
          region = MakeRect(pane.x + pane.w - half_w, pane.y, half_w, pane.h);
        } else if (nearest == top) {
          zone = EditorBodyDropZone::Top;
          region = MakeRect(pane.x, pane.y, pane.w, half_h);
        } else {
          zone = EditorBodyDropZone::Bottom;
          region = MakeRect(pane.x, pane.y + pane.h - half_h, pane.w, half_h);
        }
      }
    }
    // Dropping a tab into the middle of the pane it already lives in changes
    // nothing, so no target is offered and no overlay is painted.
    if (zone == EditorBodyDropZone::Center && gi == tab_drag_state_.source_group_index) {
      return;
    }
    tab_drag_state_.body_drop_zone = zone;
    tab_drag_state_.body_drop_group_index = gi;
    tab_drag_state_.body_drop_rect = region;
    return;
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
  // Which strip is under the pointer decides everything below: an editor drag
  // that has crossed into the other group's strip targets THAT strip's slots,
  // auto-scrolls THAT strip, and opens its gap there while the source strip
  // closes the hole the lifted tab left (TD-2026-08-14-213).
  const std::size_t previous_target_group = tab_drag_state_.target_group_index;
  const EditorBodyDropZone previous_zone = tab_drag_state_.body_drop_zone;
  const std::size_t previous_zone_group = tab_drag_state_.body_drop_group_index;
  if (tab_drag_state_.kind == TabDragKind::Editor) {
    // One editor-layout computation feeds both probes; they used to be two calls
    // that each rebuilt it, on every motion event of every drag.
    const EditorGroupRectsLayout rects = operations_.compute_editor_group_rects
                                             ? operations_.compute_editor_group_rects(*layout_state)
                                             : EditorGroupRectsLayout{};
    ResolveEditorBodyDrop(rects);
    // A live body drop owns the gesture: the destination is a pane, not the other
    // group's strip, so the cross-group strip feedback stands down.
    tab_drag_state_.target_group_index =
        tab_drag_state_.body_drop() ? tab_drag_state_.source_group_index
                                    : ResolvePointerEditorGroup(*layout_state, rects);
  }
  const bool zone_changed = tab_drag_state_.body_drop_zone != previous_zone ||
                            tab_drag_state_.body_drop_group_index != previous_zone_group;
  const bool group_changed = tab_drag_state_.target_group_index != previous_target_group;
  const bool cross_group = tab_drag_state_.cross_group();

  DragStrip d = cross_group
                    ? ResolveEditorDragStrip(*layout_state, tab_drag_state_.target_group_index)
                    : ResolveDragStrip(*layout_state, tab_drag_state_.kind);
  if (!d.valid) {
    // The destination group vanished (its last tab closed mid-drag); fall back to
    // the source strip rather than freezing the gesture.
    if (!cross_group) {
      return false;
    }
    tab_drag_state_.target_group_index = tab_drag_state_.source_group_index;
    ClearDestinationSlide();
    d = ResolveDragStrip(*layout_state, tab_drag_state_.kind);
    if (!d.valid) {
      return false;
    }
  }
  if (tab_drag_state_.body_drop()) {
    // Nothing lands in a strip, so there is no insertion slot to compute and no
    // gap to open: the source strip just closes ranks over the lifted tab while
    // the pane overlay carries the feedback. Auto-scroll stands down too -- the
    // pointer is nowhere near a strip edge.
    tab_drag_state_.reordered = true;
    tab_drag_state_.autoscroll_direction = 0;
    ClearDestinationSlide();
    if (zone_changed || group_changed || !tab_drag_state_.slide_seeded) {
      SeedSlideTargets(d, kNoInsertionSlot, /*slide_slot=*/0, tab_drag_state_.source_index);
      tab_drag_state_.slide_seeded = true;
    } else {
      AdvanceSlideOnInputThread();
    }
    return zone_changed || group_changed;
  }

  bool scrolled = false;
  if (AutoScrollDragStrip(d)) {
    // Every tab just moved: the borrowed visible list and the slide bases below
    // both have to come from the post-scroll geometry.
    d = tab_drag_state_.cross_group()
            ? ResolveEditorDragStrip(*layout_state, tab_drag_state_.target_group_index)
            : ResolveDragStrip(*layout_state, tab_drag_state_.kind);
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
  const bool slot_changed = !tab_drag_state_.slide_seeded ||
                            clamped_model_slot != tab_drag_state_.target_slot || group_changed ||
                            zone_changed;
  tab_drag_state_.target_slot = clamped_model_slot;
  const std::size_t list_slot = clamped_model_slot - d.model_offset;
  if (cross_group) {
    // A move between groups always changes something — the tab leaves a list it
    // was in — so there is no "landed back where it started" case to suppress,
    // and the destination list has no active index to displace: `list_slot` IS
    // the insertion point.
    tab_drag_state_.reordered = true;
  } else {
    const std::size_t target = MoveTargetIndexForInsertion(list_slot, d.active, d.count);
    tab_drag_state_.reordered = target != d.active;
  }
  // Rebuilding the whole displaced layout on every motion event is wasted work
  // when the pointer moved within one slot, which is most of them; the ease still
  // has to step so the neighbors keep gliding between slot changes.
  if (slot_changed || scrolled) {
    if (cross_group) {
      // Two strips animate. The source has no gap to open — its tabs just pack
      // left over the lifted tab's hole, which `kNoInsertionSlot` expresses — and
      // the destination opens one but lifts nothing, because the dragged tab is
      // not in its list yet.
      const DragStrip source =
          ResolveEditorDragStrip(*layout_state, tab_drag_state_.source_group_index);
      if (source.valid) {
        SeedSlideTargets(source, kNoInsertionSlot, /*slide_slot=*/0,
                         tab_drag_state_.source_index);
      }
      SeedSlideTargets(d, clamped_model_slot, /*slide_slot=*/1, kNoLiftedTab);
    } else {
      ClearDestinationSlide();
      SeedSlideTargets(d, clamped_model_slot, /*slide_slot=*/0, tab_drag_state_.source_index);
    }
    tab_drag_state_.slide_seeded = true;
  } else {
    AdvanceSlideOnInputThread();
  }
  // Reports whether the STRIP changed, not whether the pointer moved: the
  // auto-scroll tick uses this as its repaint signal, and it fires at ~60fps
  // against a step that lands at ~11fps.
  return slot_changed || scrolled;
}

void TabMouseCoordinator::ClearDestinationSlide() {
  TabStripSlide& destination = tab_slide_state_.strips[1];
  if (destination.idle()) {
    return;
  }
  destination.kind = TabDragKind::None;
  // Released, not cleared: a drag that leaves and re-enters the other group would
  // otherwise reallocate both vectors on every crossing.
  destination.current.clear();
  destination.target.clear();
}

void TabMouseCoordinator::SeedSlideTargets(const DragStrip& d,
                                           std::size_t insertion_slot,
                                           std::size_t slide_slot,
                                           std::size_t lifted_index) {
  // Offsets are indexed by absolute model index; only the visible subset moves.
  const std::size_t total = d.model_offset + d.count;
  TabStripSlide& slide = tab_slide_state_.strips[slide_slot];

  // Local, and deliberately: the coordinator is rebuilt per event, so a member
  // scratch would not survive between them. This runs on a slot CHANGE, not on
  // every motion event, so the cost is bounded by the number of slots the pointer
  // crosses rather than by the event rate.
  std::vector<SlideTab> slide_tabs;
  slide_tabs.reserve(d.tabs->size());
  for (const auto& tab : *d.tabs) {
    slide_tabs.push_back(SlideTab{.index = tab.index, .x = tab.rect.x, .width = tab.rect.w});
  }
  const std::vector<float> target_xs = ComputeSlideTargetXs(
      slide_tabs, lifted_index, insertion_slot, tab_drag_state_.ghost_width, /*gap=*/1.0f);

  const bool fresh = slide.kind != tab_drag_state_.kind || tab_slide_state_.settling ||
                     slide.group_index != d.group_index || slide.current.size() != total;
  if (fresh) {
    slide.current.assign(total, 0.0f);
    tab_slide_state_.last_advance_ms = SDL_GetTicks();
  }
  slide.kind = tab_drag_state_.kind;
  slide.group_index = d.group_index;
  tab_slide_state_.settling = false;
  slide.target.assign(total, 0.0f);
  for (std::size_t i = 0; i < slide_tabs.size(); ++i) {
    const std::size_t idx = slide_tabs[i].index;
    if (idx < total && idx != lifted_index) {
      slide.target[idx] = target_xs[i] - slide_tabs[i].x;
    }
  }
  AdvanceSlideOnInputThread();
}

void TabMouseCoordinator::AdvanceSlideOnInputThread() {
  const Uint64 now = SDL_GetTicks();
  const Uint64 raw_dt =
      now >= tab_slide_state_.last_advance_ms ? now - tab_slide_state_.last_advance_ms : 0;
  const float dt_ms = static_cast<float>(std::min<Uint64>(raw_dt, 100));
  for (TabStripSlide& slide : tab_slide_state_.strips) {
    if (!slide.idle()) {
      AdvanceSlideOffsets(slide.current, slide.target, dt_ms);
    }
  }
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
  // A drop over the other group's strip is a different operation from a reorder,
  // not a reorder with an extra argument; it gets its own commit path. `reordered`
  // is what Escape/focus-loss clear to abandon a gesture, so it gates this too.
  // A drop onto a pane BODY is a third commit shape: it either moves the tab into
  // that pane's group or carves a new group out on the side that was highlighted.
  // Gated on `reordered` like the others so Escape still abandons the gesture.
  if (tab_drag_state_.body_drop() && tab_drag_state_.reordered) {
    FinishEditorBodyDrop(*layout_state);
    return;
  }
  if (tab_drag_state_.cross_group() && tab_drag_state_.reordered) {
    FinishCrossGroupDrag(*layout_state);
    return;
  }
  ClearDestinationSlide();
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
  TabStripSlide& slide = tab_slide_state_.strips[0];
  const std::vector<float> previous_current = slide.current;
  const std::vector<float> previous_target = slide.target;
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

  slide.kind = kind;
  slide.group_index = after.group_index;
  tab_slide_state_.settling = true;
  slide.target.assign(total, 0.0f);
  slide.current.assign(total, 0.0f);
  tab_slide_state_.last_advance_ms = SDL_GetTicks();
  for (std::size_t i = 0; i < previous_current.size(); ++i) {
    if (i == source_index) {
      continue;
    }
    const float committed_shift =
        moved && i < previous_target.size() ? previous_target[i] : 0.0f;
    if (const std::size_t p = post_index(i); p < total) {
      slide.current[p] = previous_current[i] - committed_shift;
    }
  }
  for (const auto& tab : *after.tabs) {
    if (tab.index == dropped_index) {
      if (dropped_index < total) {
        slide.current[dropped_index] = ghost_x - tab.rect.x;
      }
      break;
    }
  }
}

void TabMouseCoordinator::FinishEditorBodyDrop(const WorkspaceLayout& layout) {
  const std::size_t source_group = tab_drag_state_.source_group_index;
  const std::size_t target_group = tab_drag_state_.body_drop_group_index;
  const EditorBodyDropZone zone = tab_drag_state_.body_drop_zone;
  // The dropped tab lands in a different pane than the one it was lifted from, so
  // there is no strip glide to carry: the whole editor area repaints anyway.
  tab_slide_state_ = TabSlideState{};

  // Same guard the two strip commits use: the press activated the dragged tab in
  // its group, so if it is no longer that group's active tab the model moved under
  // the gesture and committing would relocate a bystander.
  const DragStrip source = ResolveEditorDragStrip(layout, source_group);
  if (!source.valid || source.active != tab_drag_state_.source_index) {
    return;
  }

  bool committed = false;
  if (zone == EditorBodyDropZone::Center) {
    if (operations_.move_tab_to_group && target_group < state_.editor_groups.size()) {
      committed = operations_.move_tab_to_group(
          source_group, tab_drag_state_.source_index, target_group,
          state_.editor_groups[target_group].open_tabs.size());
    }
  } else if (operations_.move_tab_to_new_group) {
    const bool side_by_side =
        zone == EditorBodyDropZone::Left || zone == EditorBodyDropZone::Right;
    const bool insert_before =
        zone == EditorBodyDropZone::Left || zone == EditorBodyDropZone::Top;
    // The pane under the pointer is the one that gets split, which is what lets an
    // edge drop land beside a pane that is already half of a split.
    committed = operations_.move_tab_to_new_group(
        source_group, tab_drag_state_.source_index, target_group,
        side_by_side ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal,
        insert_before);
  }
  if (committed) {
    PersistReorderedTabs(TabDragKind::Editor);
  }
}

void TabMouseCoordinator::FinishCrossGroupDrag(const WorkspaceLayout& layout) {
  const std::size_t source_group = tab_drag_state_.source_group_index;
  const std::size_t destination_group = tab_drag_state_.target_group_index;
  const DragStrip source = ResolveEditorDragStrip(layout, source_group);
  const DragStrip destination = ResolveEditorDragStrip(layout, destination_group);
  // Same guard as the within-strip commit: the press activated the dragged tab in
  // its group, so if it is no longer that group's active tab the model moved under
  // the gesture and committing would relocate a bystander.
  if (!source.valid || !destination.valid || source.active != tab_drag_state_.source_index ||
      !operations_.move_tab_to_group) {
    tab_slide_state_ = TabSlideState{};
    return;
  }

  // `target_slot` is already an index into the DESTINATION list (the drag resolved
  // it against that strip), and nothing is removed from that list first, so it is
  // the raw insertion point — no MoveTargetIndexForInsertion adjustment.
  const std::size_t slot = std::min(tab_drag_state_.target_slot, destination.count);
  if (!operations_.move_tab_to_group(source_group, tab_drag_state_.source_index,
                                     destination_group, slot)) {
    tab_slide_state_ = TabSlideState{};
    return;
  }
  PersistReorderedTabs(TabDragKind::Editor);

  // The move may have emptied the source group, which collapses the split and
  // reindexes the survivor — so the destination group's index is re-read from the
  // model rather than reused, and the settle runs against the post-move layout.
  tab_slide_state_ = TabSlideState{};
  const std::size_t landed_group = state_.focused_group_index;
  const DragStrip after = ResolveEditorDragStrip(layout, landed_group);
  if (!after.valid) {
    return;
  }
  // Glide the dropped tab from where the ghost was released into its new slot.
  // Its neighbours have no carried ease to preserve: they were animating in a
  // strip that just changed length, so their offsets are meaningless now and the
  // dropped tab is the only thing with a position the user was tracking.
  TabStripSlide& slide = tab_slide_state_.strips[0];
  slide.kind = TabDragKind::Editor;
  slide.group_index = landed_group;
  slide.current.assign(after.count, 0.0f);
  slide.target.assign(after.count, 0.0f);
  tab_slide_state_.settling = true;
  tab_slide_state_.last_advance_ms = SDL_GetTicks();
  const float ghost_x = ClampedGhostX(after.strip.x, after.strip.w, tab_drag_state_.ghost_width,
                                      tab_drag_state_.pointer_x, tab_drag_state_.grab_offset_x);
  for (const auto& tab : *after.tabs) {
    if (tab.index == after.active && after.active < slide.current.size()) {
      slide.current[after.active] = ghost_x - tab.rect.x;
      break;
    }
  }
}


}  // namespace microide::workspace
