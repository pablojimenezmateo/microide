#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/EditorBodyDrop.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/TabStripAnimation.h"
#include "workspace/services/TerminalPanelService.h"

namespace microide::workspace {

constexpr float kDragAutoScrollEdgePx = 28.0f;
constexpr Uint64 kDragAutoScrollIntervalMs = 90;

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
  d.tabs = operations_.compute_visible_tabs_for_group
               ? &operations_.compute_visible_tabs_for_group(group_index, d.strip)
               : &operations_.compute_visible_tabs(d.strip);
  d.count = state_.editor_groups[group_index].open_tabs.size();
  d.active = state_.editor_groups[group_index].active_tab_index;
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
  if (rects.groups.empty() && Contains(layout.tab_strip, x, y)) {
    return state_.focused_group_index;
  }
  return tab_drag_state_.target_group_index;
}

void TabMouseCoordinator::ResolveEditorBodyDrop(const EditorGroupRectsLayout& rects) {
  tab_drag_state_.body_drop_zone = EditorBodyDropZone::None;
  if (tab_drag_state_.kind != TabDragKind::Editor || !tab_drag_state_.dragging) {
    return;
  }
  const float x = tab_drag_state_.pointer_x;
  const float y = tab_drag_state_.pointer_y;
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
    const bool source_gives_up_its_pane =
        gi != tab_drag_state_.source_group_index &&
        tab_drag_state_.source_group_index < state_.editor_groups.size() &&
        state_.editor_groups[tab_drag_state_.source_group_index].open_tabs.size() == 1;
    const bool can_split = (!state_.editor_split.full() || source_gives_up_its_pane) &&
                           (gi != tab_drag_state_.source_group_index ||
                            state_.editor_groups[gi].open_tabs.size() >= 2);
    const auto [zone, region] = EditorBodyDropAt(pane, x, y, can_split);
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

  const std::size_t terminal_count = state_.terminal_tabs.size();
  if (state_.panel.content == PanelContentKind::Output) {
    std::vector<std::string> output_ids = state_.panel.output.open_channel_ids;
    if (!state_.panel.output.channel_id.empty() &&
        std::find(output_ids.begin(), output_ids.end(), state_.panel.output.channel_id) ==
            output_ids.end()) {
      output_ids.push_back(state_.panel.output.channel_id);
    }
    if (output_ids.size() < 2) {
      return d;
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
  d.move = [this](std::size_t index) {
    return services_.terminal_panel.MoveActiveTerminalTabTo(index);
  };
  d.valid = true;
  return d;
}

bool TabMouseCoordinator::CancelDrag() {
  if (tab_drag_state_.kind == TabDragKind::None) {
    return false;
  }
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
    return false;
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
  const std::size_t previous_target_group = tab_drag_state_.target_group_index;
  const EditorBodyDropZone previous_zone = tab_drag_state_.body_drop_zone;
  const std::size_t previous_zone_group = tab_drag_state_.body_drop_group_index;
  if (tab_drag_state_.kind == TabDragKind::Editor) {
    const EditorGroupRectsLayout rects = operations_.compute_editor_group_rects
                                             ? operations_.compute_editor_group_rects(*layout_state)
                                             : EditorGroupRectsLayout{};
    ResolveEditorBodyDrop(rects);
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
    tab_drag_state_.reordered = true;
    tab_drag_state_.autoscroll_direction = 0;
    ClearDestinationSlide();
    if (zone_changed || group_changed || !tab_drag_state_.slide_seeded) {
      SeedSlideTargets(d, kNoInsertionSlot, 0, tab_drag_state_.source_index);
      tab_drag_state_.slide_seeded = true;
    } else {
      AdvanceSlideOnInputThread();
    }
    return zone_changed || group_changed;
  }

  bool scrolled = false;
  if (AutoScrollDragStrip(d)) {
    d = tab_drag_state_.cross_group()
            ? ResolveEditorDragStrip(*layout_state, tab_drag_state_.target_group_index)
            : ResolveDragStrip(*layout_state, tab_drag_state_.kind);
    if (!d.valid) {
      return false;
    }
    scrolled = true;
  }

  const std::size_t total_for_slot = d.model_offset + d.count;
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
    tab_drag_state_.reordered = true;
  } else {
    const std::size_t target = MoveTargetIndexForInsertion(list_slot, d.active, d.count);
    tab_drag_state_.reordered = target != d.active;
  }
  if (slot_changed || scrolled) {
    if (cross_group) {
      const DragStrip source =
          ResolveEditorDragStrip(*layout_state, tab_drag_state_.source_group_index);
      if (source.valid) {
        SeedSlideTargets(source, kNoInsertionSlot, 0,
                         tab_drag_state_.source_index);
      }
      SeedSlideTargets(d, clamped_model_slot, 1, kNoLiftedTab);
    } else {
      ClearDestinationSlide();
      SeedSlideTargets(d, clamped_model_slot, 0, tab_drag_state_.source_index);
    }
    tab_drag_state_.slide_seeded = true;
  } else {
    AdvanceSlideOnInputThread();
  }
  return slot_changed || scrolled;
}

void TabMouseCoordinator::ClearDestinationSlide() {
  TabStripSlide& destination = tab_slide_state_.strips[1];
  if (destination.idle()) {
    return;
  }
  destination.kind = TabDragKind::None;
  destination.current.clear();
  destination.target.clear();
}

void TabMouseCoordinator::SeedSlideTargets(const DragStrip& d,
                                           std::size_t insertion_slot,
                                           std::size_t slide_slot,
                                           std::size_t lifted_index) {
  const std::size_t total = d.model_offset + d.count;
  TabStripSlide& slide = tab_slide_state_.strips[slide_slot];

  std::vector<SlideTab> slide_tabs;
  slide_tabs.reserve(d.tabs->size());
  for (const auto& tab : *d.tabs) {
    slide_tabs.push_back(SlideTab{.index = tab.index, .x = tab.rect.x, .width = tab.rect.w});
  }
  const std::vector<float> target_xs = ComputeSlideTargetXs(
      slide_tabs, lifted_index, insertion_slot, tab_drag_state_.ghost_width, 1.0f);

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

  TabStripSlide& slide = tab_slide_state_.strips[0];
  const std::vector<float> previous_current = slide.current;
  const std::vector<float> previous_target = slide.target;
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
  tab_slide_state_ = TabSlideState{};

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
  if (!source.valid || !destination.valid || source.active != tab_drag_state_.source_index ||
      !operations_.move_tab_to_group) {
    tab_slide_state_ = TabSlideState{};
    return;
  }

  const std::size_t slot = std::min(tab_drag_state_.target_slot, destination.count);
  if (!operations_.move_tab_to_group(source_group, tab_drag_state_.source_index,
                                     destination_group, slot)) {
    tab_slide_state_ = TabSlideState{};
    return;
  }
  PersistReorderedTabs(TabDragKind::Editor);

  tab_slide_state_ = TabSlideState{};
  const std::size_t landed_group = state_.focused_group_index;
  const DragStrip after = ResolveEditorDragStrip(layout, landed_group);
  if (!after.valid) {
    return;
  }
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


}
