#include "workspace/shell/WorkspaceShell.h"

#include "workspace/MergeToolbarMetrics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "workspace/MergeWrapRows.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kMergeToolbarRowGap = 6.0f;
}

bool WorkspaceShell::ActiveTabIsMerge() const {
  return ActiveMergeTab() != nullptr;
}

WorkspaceShell::MergeTabState* WorkspaceShell::ActiveMergeTab() {
  return const_cast<MergeTabState*>(std::as_const(*this).ActiveMergeTab());
}

const WorkspaceShell::MergeTabState* WorkspaceShell::ActiveMergeTab() const {
  return context_.current_project_state.focused_group().active_merge();
}

workspace::MergeToolbarLayout WorkspaceShell::ComputeMergeToolbarLayout(const SDL_FRect& rect,
                                                                        const MergeSurfaceLayout& surface) const {
  const auto make_button_rect = [&](float x, std::string_view label) {
    const float width = ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth(label));
    return MakeRect(x, surface.button_y, width, kMergeToolbarButtonHeight);
  };

  const SDL_FRect save_rect = make_button_rect(0.0f, "Save");
  const float save_x = rect.x + rect.w - kMergeToolbarEdgeInset - save_rect.w;
  const SDL_FRect aligned_save_rect = make_button_rect(save_x, "Save");
  const SDL_FRect open_rect = make_button_rect(
      aligned_save_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Open Result").w,
      "Open Result");
  const SDL_FRect next_rect = make_button_rect(
      open_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Next").w, "Next");
  const SDL_FRect prev_rect = make_button_rect(
      next_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Prev").w, "Prev");
  const float min_x = rect.x + kMergeToolbarEdgeInset;
  const float max_bottom = rect.y + rect.h;
  const auto fitted = [min_x, max_bottom](const SDL_FRect& button) {
    return button.x >= min_x && button.y + button.h <= max_bottom ? button : SDL_FRect{};
  };
  return {
      .prev_rect = fitted(prev_rect),
      .next_rect = fitted(next_rect),
      .open_rect = fitted(open_rect),
      .save_rect = fitted(aligned_save_rect),
  };
}

std::optional<SDL_FRect> WorkspaceShell::ComputeMergeSecondaryToolbarButtonRect(
    const SDL_FRect& rect,
    const MergeSurfaceLayout& surface,
    std::string_view label) const {
  const float width = ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth(label));
  const float min_x = rect.x + kMergeToolbarEdgeInset;
  const float max_bottom = rect.y + rect.h;
  const auto fitted = [&](float x) -> std::optional<SDL_FRect> {
    if (x < min_x || surface.secondary_button_y + kMergeToolbarButtonHeight > max_bottom) {
      return std::nullopt;
    }
    return MakeRect(x, surface.secondary_button_y, width, kMergeToolbarButtonHeight);
  };
  if (label == "Mark Resolved") {
    return fitted(rect.x + rect.w - kMergeToolbarEdgeInset - width);
  }
  if (label == "Toggle Base") {
    const auto mark = ComputeMergeSecondaryToolbarButtonRect(rect, surface, "Mark Resolved");
    if (!mark.has_value()) {
      return std::nullopt;
    }
    return fitted(mark->x - kMergeToolbarButtonGap - width);
  }
  if (label == "Unresolved") {
    const auto toggle = ComputeMergeSecondaryToolbarButtonRect(rect, surface, "Toggle Base");
    if (!toggle.has_value()) {
      return std::nullopt;
    }
    return fitted(toggle->x - kMergeToolbarButtonGap - width);
  }
  return std::nullopt;
}

workspace::MergeSurfaceLayout WorkspaceShell::ComputeMergeSurfaceLayout(const SDL_FRect& rect,
                                                                        const MergeTabState& merge_tab) const {
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    MergeSurfaceLayout layout;
    layout.line_height = editor_text_renderer_.LineHeight();
    const std::size_t max_line_count =
        std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.lines().size(),
                  merge_tab.model.current_lines.size(), std::size_t{1}});
    std::array<char, 20> line_count_buf;
    const auto [line_count_end, _] =
        std::to_chars(line_count_buf.data(), line_count_buf.data() + line_count_buf.size(),
                      max_line_count + 1);
    layout.gutter_width =
        std::max(28.0f,
                 editor_text_renderer_.MeasureWidth(std::string_view{
                     line_count_buf.data(),
                     static_cast<std::size_t>(line_count_end - line_count_buf.data())}) +
                     12.0f);
    layout.divider_width = 16.0f;
    layout.button_y = rect.y + 6.0f;
    layout.secondary_button_y =
        layout.button_y + kMergeToolbarButtonHeight + kMergeToolbarRowGap;
    const float base_band_height =
        merge_tab.base_pane_visible && !merge_tab.model.base_lines.empty()
            ? layout.line_height *
                      static_cast<float>(std::min<std::size_t>(4, merge_tab.model.base_lines.size()) + 1) +
                  8.0f
            : 0.0f;
    layout.header_y = layout.secondary_button_y + kMergeToolbarButtonHeight + kMergeHeaderRowGap +
                      base_band_height;
    layout.rows_y = layout.header_y + layout.line_height + 8.0f;

    const float reserved_width = reserve_vertical
                                     ? (kWorkspaceScrollbarThickness +
                                        kWorkspaceScrollbarInset)
                                     : 0.0f;
    const float reserved_height = reserve_horizontal
                                      ? (kWorkspaceScrollbarThickness +
                                         kWorkspaceScrollbarInset)
                                      : 0.0f;
    const float char_width = std::max(1.0f, editor_text_renderer_.CharWidth());
    const float min_pane_width = 8.0f + char_width;
    layout.divider_width = std::min(layout.divider_width, std::max(1.0f, (rect.w - reserved_width) * 0.5f));
    const float margin =
        std::clamp((rect.w - reserved_width - layout.divider_width * 2.0f) * 0.5f, 0.0f, 8.0f);
    layout.left_x = rect.x + margin;
    const float fixed_width = reserved_width + layout.divider_width * 2.0f + margin * 2.0f;
    layout.gutter_width = std::min(layout.gutter_width, std::max(0.0f, (rect.w - fixed_width) / 3.0f));
    const float content_width = std::max(0.0f, rect.w - fixed_width - layout.gutter_width * 3.0f);
    const float min_fraction = content_width >= min_pane_width * 3.0f
                                   ? min_pane_width / content_width
                                   : 1.0f / 3.0f;
    const float left_hi = std::max(min_fraction, 1.0f - min_fraction * 2.0f);
    const float left_fraction =
        std::clamp(merge_tab.left_divider_fraction, min_fraction, left_hi);
    const float right_lo = left_fraction + min_fraction;
    const float right_hi = std::max(right_lo, 1.0f - min_fraction);
    const float right_fraction =
        std::clamp(merge_tab.right_divider_fraction, right_lo, right_hi);
    layout.min_divider_fraction = min_fraction;
    layout.left_width = std::floor(content_width * left_fraction);
    layout.center_width = std::floor(content_width * (right_fraction - left_fraction));
    layout.right_width = std::max(0.0f, content_width - layout.left_width - layout.center_width);
    layout.center_x = layout.left_x + layout.gutter_width + layout.left_width + layout.divider_width;
    layout.right_x =
        layout.center_x + layout.gutter_width + layout.center_width + layout.divider_width;

    const float row_region_height = rect.h - reserved_height - (layout.rows_y - rect.y) - 8.0f;
    layout.visible_rows = std::max(
        1, static_cast<int>(row_region_height / std::max(1.0f, layout.line_height)));

    const float pane_text_width = std::max(
        0.0f, std::min({layout.left_width, layout.center_width, layout.right_width}) - 8.0f);
    layout.visible_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::floor(pane_text_width / std::max(1.0f, editor_text_renderer_.CharWidth()))));
    layout.show_vertical = reserve_vertical;
    layout.show_horizontal = reserve_horizontal;
    return layout;
  };

  if (merge_tab.result_viewport.soft_wrap()) {
    const MergeSurfaceLayout layout =
        measure(true, false);
    EnsureMergeWrapLayout(merge_tab, true, layout.visible_columns);
    return layout;
  }
  EnsureMergeWrapLayout(merge_tab, false, 0);

  bool show_vertical = false;
  bool show_horizontal = false;
  for (int iteration = 0; iteration < 4; ++iteration) {
    const MergeSurfaceLayout layout = measure(show_vertical, show_horizontal);
    const std::size_t line_count =
        std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.line_count(),
                  merge_tab.model.current_lines.size(), std::size_t{1}});
    const bool next_vertical = line_count > static_cast<std::size_t>(layout.visible_rows);
    const bool next_horizontal = merge_tab.max_visual_columns > layout.visible_columns;
    if (next_vertical == show_vertical && next_horizontal == show_horizontal) {
      return layout;
    }
    show_vertical = next_vertical;
    show_horizontal = next_horizontal;
  }

  return measure(show_vertical, show_horizontal);
}

TextGridInteractionLayout WorkspaceShell::BuildMergeSourceInteractionLayout(
    const MergeSurfaceLayout& surface,
    const MergeTabState& merge_tab,
    bool incoming) const {
  const float pane_x = incoming ? surface.left_x : surface.right_x;
  const float pane_width = incoming ? surface.left_width : surface.right_width;
  const std::size_t line_count =
      merge_tab.wrap_layout.active()
          ? MergeSourceVisualRowCount(merge_tab)
          : (incoming ? merge_tab.model.incoming_lines.size()
                      : merge_tab.model.current_lines.size());
  return ComputeTextGridInteractionLayout(
      MakeRect(pane_x, surface.rows_y, surface.gutter_width + pane_width,
               static_cast<float>(surface.visible_rows) * surface.line_height),
      pane_x + surface.gutter_width, surface.rows_y, surface.line_height,
      editor_text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, merge_tab.scroll_row)),
      line_count, merge_tab.horizontal_scroll, static_cast<std::size_t>(surface.visible_rows),
      surface.visible_columns);
}

workspace::MergeResultInteractionLayout WorkspaceShell::BuildMergeResultInteractionLayout(
    const SDL_FRect& rect, const MergeSurfaceLayout& surface, const MergeTabState& merge_tab) const {
  const SDL_FRect result_rect = ComputeMergeResultViewportRect(
      rect, surface.center_x, surface.rows_y, surface.gutter_width, surface.center_width,
      surface.show_horizontal);
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      editor_text_renderer_, merge_tab.result_viewport, result_rect, editor::kNoStickyBand,
      LineNumbersEnabled());
  const VisibleLineRangeLayout lines = {
      .first_line_y = metrics.first_line_y,
      .line_height = metrics.line_height,
      .scroll_line = merge_tab.result_viewport.scroll_line(),
      .visible_rows = metrics.visible_rows,
  };
  return MergeResultInteractionLayout{
      .rect = result_rect,
      .metrics = metrics,
      .lines = lines,
      .text = ComputeTextGridInteractionLayout(
          result_rect, metrics.text_x, metrics.first_line_y, metrics.line_height,
          editor_text_renderer_.CharWidth(), merge_tab.result_viewport.scroll_line(),
          static_cast<std::size_t>(std::max(0, merge_tab.result_viewport.VisualRowCount())),
          merge_tab.result_viewport.horizontal_scroll(), metrics.visible_rows,
          metrics.visible_columns),
  };
}

workspace::MergeInteractionLayout WorkspaceShell::BuildMergeInteractionLayout(const SDL_FRect& rect,
                                                                              const MergeSurfaceLayout& surface,
                                                                              MergeTabState& merge_tab) const {
  const float bottom_reserved = surface.show_horizontal
                                    ? (kWorkspaceScrollbarThickness +
                                       kWorkspaceScrollbarInset)
                                    : 0.0f;
  MergeResultInteractionLayout result = BuildMergeResultInteractionLayout(rect, surface, merge_tab);
  const std::size_t scroll_before_sizing = merge_tab.result_viewport.scroll_line();
  merge_tab.result_viewport.SetViewportSize(result.metrics.visible_rows,
                                            result.metrics.visible_columns);
  if (merge_tab.result_viewport.scroll_line() != scroll_before_sizing) {
    result = BuildMergeResultInteractionLayout(rect, surface, merge_tab);
  }
  return MergeInteractionLayout{
      .content_bottom = rect.y + std::max(0.0f, rect.h - bottom_reserved),
      .result = result,
      .incoming_accept_button_width =
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Accept Remote")),
      .current_accept_button_width =
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Accept Local")),
      .result_action_widths = {
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Base")),
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Remote")),
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Local")),
          ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Both")),
      },
  };
}

std::optional<SDL_FRect> WorkspaceShell::CurrentMergeConflictRect(std::size_t conflict_index) const {
  const auto layout = CurrentWorkspaceLayout();
  MergeTabState* merge_tab = const_cast<WorkspaceShell*>(this)->ActiveMergeTab();
  if (!layout.has_value() || merge_tab == nullptr || conflict_index >= merge_tab->conflicts.size()) {
    return std::nullopt;
  }

  const SDL_FRect surface = ActiveTabSurface(*layout);
  const MergeSurfaceLayout surface_layout = ComputeMergeSurfaceLayout(surface, *merge_tab);
  const MergeInteractionLayout interaction =
      BuildMergeInteractionLayout(surface, surface_layout, *merge_tab);
  const MergeTrackedConflict& conflict = MergeVisualConflicts(*merge_tab)[conflict_index];
  const VisibleLineRangeLayout source_lines = {
      .first_line_y = surface_layout.rows_y,
      .line_height = surface_layout.line_height,
      .scroll_line = static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)),
      .visible_rows = static_cast<std::size_t>(surface_layout.visible_rows),
  };
  const TextGridInteractionLayout incoming_layout =
      BuildMergeSourceInteractionLayout(surface_layout, *merge_tab, true);
  const TextGridInteractionLayout current_layout =
      BuildMergeSourceInteractionLayout(surface_layout, *merge_tab, false);

  std::optional<SDL_FRect> rect = ComputeVisibleLineRangeRect(
      incoming_layout.rect, source_lines, conflict.incoming_start_line,
      std::max(conflict.incoming_end_line, conflict.incoming_start_line + std::size_t{1}));
  if (const auto current_rect = ComputeVisibleLineRangeRect(
          current_layout.rect, source_lines, conflict.current_start_line,
          std::max(conflict.current_end_line, conflict.current_start_line + std::size_t{1}));
      current_rect.has_value()) {
    rect = UnionOptionalRects(rect, *current_rect);
  }
  if (const auto result_rect = ComputeVisibleLineRangeRect(
          interaction.result.rect, interaction.result.lines, conflict.start_line,
          std::max(conflict.end_line, conflict.start_line + std::size_t{1}));
      result_rect.has_value()) {
    rect = UnionOptionalRects(rect, *result_rect);
  }
  rect = UnionOptionalRects(rect,
                            BuildMergeSourceActionButtonRect(surface_layout, interaction, conflict, true));
  rect = UnionOptionalRects(rect,
                            BuildMergeSourceActionButtonRect(surface_layout, interaction, conflict, false));
  if (conflict.valid) {
    for (const SDL_FRect action_rect :
         BuildMergeResultActionButtonRects(surface_layout, interaction, conflict)) {
      rect = UnionOptionalRects(rect, action_rect);
    }
  }
  return rect;
}

SDL_FRect WorkspaceShell::BuildMergeSourceActionButtonRect(
    const MergeSurfaceLayout& surface,
    const MergeInteractionLayout& interaction,
    const MergeTrackedConflict& conflict,
    bool incoming) const {
  const MergeTabState* merge_tab = const_cast<WorkspaceShell*>(this)->ActiveMergeTab();
  const int source_scroll_row =
      merge_tab != nullptr ? std::max(0, merge_tab->scroll_row)
                           : static_cast<int>(interaction.result.text.scroll_line);
  return ComputeMergeSourceActionButtonRect(
      incoming ? surface.left_x : surface.right_x, surface.gutter_width, surface.rows_y,
      surface.line_height, source_scroll_row,
      incoming ? conflict.incoming_end_line : conflict.current_end_line, interaction.content_bottom,
      incoming ? interaction.incoming_accept_button_width : interaction.current_accept_button_width,
      kMergeToolbarButtonHeight,
      incoming ? surface.left_x + surface.left_width : surface.right_x + surface.right_width);
}

std::optional<WorkspaceShell::MergeHoverState> WorkspaceShell::ClassifyMergeHoverState(
    const MergeSurfaceLayout& surface,
    const MergeInteractionLayout& interaction,
    const MergeTabState& merge_tab,
    float x,
    float y) const {
  return microide::workspace::ClassifyMergeHoverState(
      MergeHoverSurfaceLayout{
          .gutter_width = surface.gutter_width,
          .left_x = surface.left_x,
          .center_x = surface.center_x,
          .right_x = surface.right_x,
          .rows_y = surface.rows_y,
          .line_height = surface.line_height,
          .left_width = surface.left_width,
          .center_width = surface.center_width,
          .right_width = surface.right_width,
      },
      MergeHoverInteractionLayout{
          .content_bottom = interaction.content_bottom,
          .incoming = BuildMergeSourceInteractionLayout(surface, merge_tab, true),
          .current = BuildMergeSourceInteractionLayout(surface, merge_tab, false),
          .result =
              MergeHoverResultLayout{
                  .rect = interaction.result.rect,
                  .lines = interaction.result.lines,
                  .text = interaction.result.text,
              },
          .incoming_accept_button_width = interaction.incoming_accept_button_width,
          .current_accept_button_width = interaction.current_accept_button_width,
          .result_action_widths = interaction.result_action_widths,
          .button_height = kMergeToolbarButtonHeight,
          .button_gap = kMergeToolbarButtonGap,
      },
      MergeVisualConflicts(merge_tab), x, y);
}

void WorkspaceShell::RevealActiveMergeSelection() {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(ActiveTabSurface(layout), *merge_tab);
  ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
  ClampMergeHorizontalScroll(*merge_tab, surface_layout.visible_columns);
  const auto& selected_conflict =
      merge_tab->conflicts[std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1)];
  const int start_row = static_cast<int>(std::min(
      {selected_conflict.incoming_start_line, selected_conflict.start_line,
       selected_conflict.current_start_line}));
  const int end_row = static_cast<int>(std::max(
      {selected_conflict.incoming_end_line, selected_conflict.end_line,
       selected_conflict.current_end_line}));
  if (start_row < merge_tab->scroll_row) {
    merge_tab->scroll_row = start_row;
  } else if (end_row > merge_tab->scroll_row + surface_layout.visible_rows) {
    merge_tab->scroll_row = end_row - surface_layout.visible_rows;
  }
  ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
  merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
}

}
