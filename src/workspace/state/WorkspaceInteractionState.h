#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <span>
#include <vector>

#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

enum class DragTarget {
  None,
  SidebarDivider,
  RightPaneDivider,
  BottomPanelDivider,
  CompareDivider,
  MergeLeftDivider,
  MergeRightDivider,
  SidebarScrollbar,
  BottomPanelScrollbar,
  DebugPaneScrollbar,
  OverlayScrollbar,
  EditorVerticalScrollbar,
  EditorHorizontalScrollbar,
  EditorSplitDivider,
  TerminalPaneDivider,
  CompareVerticalScrollbar,
  CompareHorizontalScrollbar,
  SettingsScrollbar,
  SettingsCategoryScrollbar,
  SettingsPickerScrollbar,
  SingleLineSelection,
};

inline bool IsDividerDrag(DragTarget target) {
  switch (target) {
    case DragTarget::SidebarDivider:
    case DragTarget::RightPaneDivider:
    case DragTarget::BottomPanelDivider:
    case DragTarget::EditorSplitDivider:
    case DragTarget::TerminalPaneDivider:
    case DragTarget::CompareDivider:
    case DragTarget::MergeLeftDivider:
    case DragTarget::MergeRightDivider:
      return true;
    default:
      return false;
  }
}

enum class TabDragKind {
  None,
  Project,
  Editor,
  Terminal,
};

enum class EditorBodyDropZone : std::uint8_t {
  None,
  Center,
  Left,
  Right,
  Top,
  Bottom,
};

struct TabDragState {
  TabDragKind kind = TabDragKind::None;
  float press_x = 0.0f;
  float press_y = 0.0f;
  bool dragging = false;
  bool reordered = false;
  std::size_t source_index = 0;
  float pointer_x = 0.0f;
  float pointer_y = 0.0f;
  std::size_t target_slot = 0;
  float ghost_width = 0.0f;
  float grab_offset_x = 0.0f;
  std::size_t source_group_index = 0;
  std::size_t target_group_index = 0;
  bool cross_group() const { return kind == TabDragKind::Editor &&
                                    target_group_index != source_group_index; }
  bool slide_seeded = false;
  int autoscroll_direction = 0;
  Uint64 last_autoscroll_ms = 0;
  EditorBodyDropZone body_drop_zone = EditorBodyDropZone::None;
  std::size_t body_drop_group_index = 0;
  SDL_FRect body_drop_rect{0.0f, 0.0f, 0.0f, 0.0f};
  bool body_drop() const {
    return kind == TabDragKind::Editor && body_drop_zone != EditorBodyDropZone::None;
  }
};

struct PathDragState {
  enum class Target : std::uint8_t { None, EditorGroup, TreeFolder, Terminal };
  bool armed = false;
  bool dragging = false;
  std::filesystem::path path;
  bool is_directory = false;
  std::string label;
  float press_x = 0.0f;
  float press_y = 0.0f;
  float pointer_x = 0.0f;
  float pointer_y = 0.0f;
  Target target = Target::None;
  EditorBodyDropZone zone = EditorBodyDropZone::None;
  std::size_t group_index = 0;
  std::filesystem::path folder;
  SDL_FRect drop_rect{0.0f, 0.0f, 0.0f, 0.0f};
};

struct TabStripSlide {
  TabDragKind kind = TabDragKind::None;
  std::size_t group_index = 0;
  std::vector<float> current;
  std::vector<float> target;

  bool idle() const { return kind == TabDragKind::None; }
  bool Is(TabDragKind for_kind, std::size_t for_group) const {
    return kind == for_kind && (kind != TabDragKind::Editor || group_index == for_group);
  }
};

inline constexpr std::size_t kMaxAnimatingTabStrips = 2;

struct TabSlideState {
  std::array<TabStripSlide, kMaxAnimatingTabStrips> strips;
  Uint64 last_advance_ms = 0;
  bool settling = false;

  bool active() const {
    for (const TabStripSlide& strip : strips) {
      if (!strip.idle()) {
        return true;
      }
    }
    return false;
  }
  std::span<const float> OffsetsFor(TabDragKind kind, std::size_t group_index) const {
    for (const TabStripSlide& strip : strips) {
      if (!strip.idle() && strip.Is(kind, group_index)) {
        return strip.current;
      }
    }
    return {};
  }
};

struct InteractionState {
  struct CaretHover {
    bool active = false;
    const void* viewport = nullptr;
    std::size_t line = 0;
    std::size_t column = 0;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_valid = false;
  };
  CaretHover caret_hover;
  bool window_has_input_focus = true;
  std::uint64_t last_desktop_notification_ms = 0;
  std::uint32_t desktop_notifications_posted = 0;
  std::uint32_t window_flags_requested = 0;
  SDL_Keymod key_modifiers = SDL_KMOD_NONE;
  bool mouse_selecting = false;
  DragTarget drag_target = DragTarget::None;
  float drag_scrollbar_offset = 0.0f;
  std::uint8_t drag_editor_split_node = 0;
  std::uint8_t drag_editor_split_boundary = 0;
  std::uint8_t drag_terminal_pane_boundary = 0;
  TabDragState tab_drag;
  PathDragState path_drag;
  TabSlideState tab_slide;
  float wheel_accumulator_y = 0.0f;
  float wheel_accumulator_x = 0.0f;
  TextInputSurface single_line_drag_surface = TextInputSurface::None;
  bool editor_box_selecting = false;
  std::size_t editor_box_anchor_line = 0;
  std::size_t editor_box_anchor_column = 0;
  enum class TextDragState : std::uint8_t { None, Pending, Dragging };
  TextDragState text_drag = TextDragState::None;
  float text_drag_press_x = 0.0f;
  float text_drag_press_y = 0.0f;
  std::size_t text_drag_press_line = 0;
  std::size_t text_drag_press_column = 0;
  std::size_t text_drag_source_start_line = 0;
  std::size_t text_drag_source_start_column = 0;
  std::size_t text_drag_source_end_line = 0;
  std::size_t text_drag_source_end_column = 0;
  const void* text_drag_viewport = nullptr;
  std::uint64_t text_drag_content_revision = 0;
  const void* editor_gesture_viewport = nullptr;
  std::size_t text_drag_drop_line = 0;
  std::size_t text_drag_drop_column = 0;
  bool text_drag_has_drop = false;
  bool text_dragging() const { return text_drag == TextDragState::Dragging; }
  int selection_autoscroll_rows = 0;
  int selection_autoscroll_columns = 0;
  enum class SelectionAutoscrollOwner : std::uint8_t { None, Text, Terminal };
  SelectionAutoscrollOwner selection_autoscroll_owner = SelectionAutoscrollOwner::None;
  float selection_pointer_x = 0.0f;
  float selection_pointer_y = 0.0f;
  enum class SelectionGranularity : std::uint8_t { Character, Word, Line };
  SelectionGranularity selection_granularity = SelectionGranularity::Character;
  std::size_t selection_seed_start_line = 0;
  std::size_t selection_seed_start_column = 0;
  std::size_t selection_seed_end_line = 0;
  std::size_t selection_seed_end_column = 0;
  bool selection_autoscroll_active() const {
    return selection_autoscroll_owner != SelectionAutoscrollOwner::None &&
           (selection_autoscroll_rows != 0 || selection_autoscroll_columns != 0);
  }
};

struct WheelTicks {
  int vertical = 0;
  int horizontal = 0;
};

inline WheelTicks AccumulateWheelEvent(InteractionState& interaction,
                                       float raw_y,
                                       float raw_x,
                                       bool flipped) {
  const float flip_sign = flipped ? -1.0f : 1.0f;
  const float dy = std::isfinite(raw_y) ? raw_y * flip_sign : 0.0f;
  const float dx = std::isfinite(raw_x) ? raw_x * flip_sign : 0.0f;
  float& accum_y = interaction.wheel_accumulator_y;
  float& accum_x = interaction.wheel_accumulator_x;
  if ((dy > 0.0f && accum_y < 0.0f) || (dy < 0.0f && accum_y > 0.0f)) {
    accum_y = 0.0f;
  }
  if ((dx > 0.0f && accum_x < 0.0f) || (dx < 0.0f && accum_x > 0.0f)) {
    accum_x = 0.0f;
  }
  accum_y += dy;
  accum_x += dx;
  const int vertical = static_cast<int>(std::trunc(accum_y));
  const int horizontal = static_cast<int>(std::trunc(accum_x));
  accum_y -= static_cast<float>(vertical);
  accum_x -= static_cast<float>(horizontal);
  return WheelTicks{.vertical = vertical, .horizontal = horizontal};
}

}
