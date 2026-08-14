#include "workspace/WorkspaceLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace microide::workspace {

namespace {

constexpr float kScrollbarThickness = kWorkspaceScrollbarThickness;
constexpr float kScrollbarInset = kWorkspaceScrollbarInset;

}  // namespace
float ComputeChromeButtonWidth(float measured_label_width) {
  return std::clamp(measured_label_width + 18.0f, 64.0f, 160.0f);
}

void ComputeVisibleStripLayoutsInto(std::vector<StripSlotLayout>& out,
                                    const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t first_index) {
  out.clear();
  if (widths.empty()) {
    return;
  }

  const std::size_t clamped_first = std::min(first_index, widths.size() - 1);
  float x = start_x;
  for (std::size_t i = clamped_first; i < widths.size(); ++i) {
    const float width = widths[i];
    if (x + width > max_x) {
      break;
    }
    out.push_back(StripSlotLayout{
        .index = i,
        .x = x,
        .width = width,
    });
    x += width + gap;
  }
}

std::vector<StripSlotLayout> ComputeVisibleStripLayouts(const std::vector<float>& widths,
                                                        float start_x,
                                                        float gap,
                                                        float max_x,
                                                        std::size_t first_index) {
  std::vector<StripSlotLayout> slots;
  ComputeVisibleStripLayoutsInto(slots, widths, start_x, gap, max_x, first_index);
  return slots;
}

std::size_t EnsureVisibleStripIndex(const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t current_first_index,
                                    std::size_t active_index) {
  if (widths.empty()) {
    return 0;
  }

  const std::size_t clamped_active = std::min(active_index, widths.size() - 1);
  const std::size_t clamped_first = std::min(current_first_index, widths.size() - 1);

  // Check visibility inline without allocating a StripSlotLayout vector.
  {
    float x = start_x;
    for (std::size_t i = clamped_first; i < widths.size(); ++i) {
      if (x + widths[i] > max_x) {
        break;
      }
      if (i == clamped_active) {
        return clamped_first;
      }
      x += widths[i] + gap;
    }
  }

  float used_width = widths[clamped_active];
  std::size_t first_visible = clamped_active;
  while (first_visible > 0) {
    const float candidate_width = used_width + gap + widths[first_visible - 1];
    if (start_x + candidate_width > max_x) {
      break;
    }
    used_width = candidate_width;
    --first_visible;
  }

  return first_visible;
}

void BuildChromeTabRenderItemsInto(std::vector<ChromeTabRenderItem>& out,
                                   std::span<const StripSlotLayout> slots,
                                   float tab_y,
                                   float tab_height,
                                   std::span<const std::size_t> model_indices,
                                   std::size_t active_index,
                                   std::span<const std::string> display_titles,
                                   std::span<const std::string> tooltip_labels,
                                   float close_button_size,
                                   float close_button_right_inset) {
  // resize, not clear+push_back: the elements that survive keep their two string
  // buffers, and `assign` below refills them in place.
  out.resize(slots.size());
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const StripSlotLayout& slot = slots[i];
    const std::size_t model_index =
        slot.index < model_indices.size() ? model_indices[slot.index] : slot.index;
    const SDL_FRect rect = MakeRect(slot.x, tab_y, slot.width, tab_height);
    ChromeTabRenderItem& item = out[i];
    item.index = model_index;
    item.rect = rect;
    item.close_rect = MakeRect(
        rect.x + rect.w - close_button_right_inset - close_button_size,
        rect.y + std::floor(std::max(0.0f, rect.h - close_button_size) * 0.5f), close_button_size,
        close_button_size);
    item.active = model_index == active_index;
    if (slot.index < display_titles.size()) {
      item.display_title.assign(display_titles[slot.index]);
    } else {
      item.display_title.clear();
    }
    if (slot.index < tooltip_labels.size()) {
      item.tooltip_label.assign(tooltip_labels[slot.index]);
    } else {
      item.tooltip_label.clear();
    }
  }
}

std::vector<ChromeTabRenderItem> BuildChromeTabRenderItems(
    std::span<const StripSlotLayout> slots,
    float tab_y,
    float tab_height,
    std::span<const std::size_t> model_indices,
    std::size_t active_index,
    std::span<const std::string> display_titles,
    std::span<const std::string> tooltip_labels,
    float close_button_size,
    float close_button_right_inset) {
  std::vector<ChromeTabRenderItem> items;
  BuildChromeTabRenderItemsInto(items, slots, tab_y, tab_height, model_indices, active_index,
                                display_titles, tooltip_labels, close_button_size,
                                close_button_right_inset);
  return items;
}

SDL_FRect ComputeMergeResultViewportRect(const SDL_FRect& editor_surface,
                                         float center_x,
                                         float rows_y,
                                         float gutter_width,
                                         float center_width,
                                         bool show_horizontal) {
  const float bottom_reserved = show_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  const float content_height = std::max(0.0f, editor_surface.h - bottom_reserved);
  return MakeRect(center_x, rows_y - 8.0f, gutter_width + center_width,
                  std::max(0.0f, editor_surface.y + content_height - (rows_y - 8.0f)));
}

std::optional<SDL_FRect> ComputeVisibleLineRangeRect(const SDL_FRect& viewport_rect,
                                                     const VisibleLineRangeLayout& layout,
                                                     std::size_t start_line,
                                                     std::size_t end_line) {
  if (viewport_rect.w <= 0.0f || viewport_rect.h <= 0.0f || end_line <= start_line ||
      layout.visible_rows == 0 || layout.line_height <= 0.0f) {
    return std::nullopt;
  }

  const std::size_t visible_end_line = layout.scroll_line + layout.visible_rows;
  const std::size_t rect_start = std::max(start_line, layout.scroll_line);
  const std::size_t rect_end = std::max(end_line, start_line + 1);
  if (rect_end <= layout.scroll_line || rect_start >= visible_end_line) {
    return std::nullopt;
  }

  const float y =
      layout.first_line_y + static_cast<float>(rect_start - layout.scroll_line) * layout.line_height;
  const float h =
      static_cast<float>(std::min(rect_end, visible_end_line) - rect_start) * layout.line_height;
  // The diff surfaces paint their row bands one pixel above the nominal text grid
  // (see WorkspaceShellRenderCompare/Merge), so this returns the band as painted.
  // Redraw callers wrap it in DirtyRectWithHalo; paint callers use it as-is.
  return MakeRect(viewport_rect.x, y - 1.0f, viewport_rect.w, h);
}

SDL_FRect ComputeMergeSourceActionButtonRect(float pane_x,
                                             float gutter_width,
                                             float rows_y,
                                             float line_height,
                                             int scroll_row,
                                             std::size_t end_line,
                                             float content_bottom,
                                             float button_width,
                                             float button_height) {
  float y = rows_y +
            static_cast<float>(static_cast<long long>(end_line) - scroll_row) * line_height + 2.0f;
  y = std::min(y, content_bottom - button_height - 4.0f);
  return MakeRect(pane_x + gutter_width, y, button_width, button_height);
}

std::array<SDL_FRect, 4> ComputeMergeResultActionButtonRects(
    float start_x,
    float rows_y,
    float content_bottom,
    const std::optional<SDL_FRect>& conflict_rect,
    const std::array<float, 4>& widths,
    float button_height,
    float button_gap) {
  float y = conflict_rect.has_value() ? conflict_rect->y + conflict_rect->h + 2.0f : rows_y + 2.0f;
  if (y + button_height > content_bottom - 4.0f && conflict_rect.has_value()) {
    y = std::max(rows_y + 2.0f, conflict_rect->y - button_height - 2.0f);
  }

  float x = start_x;
  std::array<SDL_FRect, 4> rects{};
  for (std::size_t i = 0; i < rects.size(); ++i) {
    rects[i] = MakeRect(x, y, widths[i], button_height);
    x += widths[i] + button_gap;
  }
  return rects;
}

std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line,
    bool incoming) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    const std::size_t start = incoming ? conflict.incoming_start_line : conflict.current_start_line;
    const std::size_t raw_end = incoming ? conflict.incoming_end_line : conflict.current_end_line;
    // Normalize zero-length source spans (pure insertion/deletion conflicts where
    // start == end and the side contributes no source lines) exactly the way the
    // result-side lookup does, so a zero-height span still owns its anchor row for
    // hover, tint, and accept-button hit-testing instead of matching nothing.
    const std::size_t end = std::max(raw_end, start + std::size_t{1});
    if (line >= start && line < end) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    const std::size_t end = std::max(conflict.end_line, conflict.start_line + std::size_t{1});
    if (line >= conflict.start_line && line < end) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<MergeHoverState> ClassifyMergeHoverState(
    const MergeHoverSurfaceLayout& surface,
    const MergeHoverInteractionLayout& interaction,
    std::span<const MergeTrackedConflict> conflicts,
    float x,
    float y) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    if (!conflict.valid) {
      continue;
    }

    // Source-pane accept buttons anchor to the SOURCE pane's own scroll, not the
    // result pane's. When a source pane is longer than the result the result scroll
    // clamps lower, so using it here would drift the button off its rendered row.
    if (Contains(ComputeMergeSourceActionButtonRect(surface.left_x, surface.gutter_width,
                                                    surface.rows_y, surface.line_height,
                                                    static_cast<int>(interaction.incoming.scroll_line),
                                                    conflict.incoming_end_line,
                                                    interaction.content_bottom,
                                                    interaction.incoming_accept_button_width,
                                                    interaction.button_height),
                 x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::IncomingAccept,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Incoming,
      };
    }
    if (Contains(ComputeMergeSourceActionButtonRect(surface.right_x, surface.gutter_width,
                                                    surface.rows_y, surface.line_height,
                                                    static_cast<int>(interaction.current.scroll_line),
                                                    conflict.current_end_line,
                                                    interaction.content_bottom,
                                                    interaction.current_accept_button_width,
                                                    interaction.button_height),
                 x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::CurrentAccept,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Current,
      };
    }

    const auto action_rects = ComputeMergeResultActionButtonRects(
        surface.center_x + surface.gutter_width, surface.rows_y, interaction.content_bottom,
        ComputeVisibleLineRangeRect(interaction.result.rect, interaction.result.lines,
                                    conflict.start_line,
                                    std::max(conflict.end_line, conflict.start_line + std::size_t{1})),
        interaction.result_action_widths, interaction.button_height, interaction.button_gap);
    if (Contains(action_rects[0], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Base,
      };
    }
    if (Contains(action_rects[1], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Incoming,
      };
    }
    if (Contains(action_rects[2], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Current,
      };
    }
    if (Contains(action_rects[3], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Both,
      };
    }
  }

  if (x < surface.center_x) {
    if (const auto line = VisibleTextGridLineAtY(interaction.incoming, y); line.has_value()) {
      if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(conflicts, *line, true);
          conflict_index.has_value()) {
        return MergeHoverState{
            .kind = MergeHoverState::Kind::IncomingConflict,
            .conflict_index = *conflict_index,
            .preview_choice = compare::MergeChoice::Incoming,
        };
      }
    }
  } else if (x >= surface.right_x) {
    if (const auto line = VisibleTextGridLineAtY(interaction.current, y); line.has_value()) {
      if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(conflicts, *line, false);
          conflict_index.has_value()) {
        return MergeHoverState{
            .kind = MergeHoverState::Kind::CurrentConflict,
            .conflict_index = *conflict_index,
            .preview_choice = compare::MergeChoice::Current,
        };
      }
    }
  }

  if (Contains(interaction.result.rect, x, y)) {
    const std::size_t line = ClampTextGridLineAtY(interaction.result.text, y);
    if (const auto conflict_index = FindMergeTrackedConflictAtResultLine(conflicts, line);
        conflict_index.has_value()) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultConflict,
          .conflict_index = *conflict_index,
          .preview_choice = conflicts[*conflict_index].last_choice,
      };
    }
  }

  return std::nullopt;
}

}  // namespace microide::workspace
