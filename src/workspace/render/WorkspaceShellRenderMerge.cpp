#include "workspace/shell/WorkspaceShell.h"

#include "workspace/MergeToolbarMetrics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/ScopedRenderClip.h"
#include "render/Theme.h"
#include "util/PerformanceTrace.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/MergeResolverContext.h"
#include "workspace/render/DiffOverviewMarkers.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/MergeWrapRows.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

namespace {

constexpr float kMergeDiffRowTint = 0.14f;
constexpr float kMergeDiffRowTintSelected = 0.22f;

SDL_Color MergeMarkerColor(const render::Theme& theme,
                           compare::MergeChoice choice,
                           bool valid) {
  if (!valid) {
    return theme.text_disabled;
  }

  switch (choice) {
    case compare::MergeChoice::Incoming:
      return theme.diff_added;
    case compare::MergeChoice::Current:
      return theme.diff_modified;
    case compare::MergeChoice::Both:
    case compare::MergeChoice::BothCurrentFirst:
    case compare::MergeChoice::BothIncomingFirst:
      return theme.accent;
    case compare::MergeChoice::Base:
    default:
      return theme.diff_deleted;
  }
}

}

void EnsureMergeOverviewMarkers(const render::Theme& theme,
                                const SDL_FRect& inner_lane,
                                std::size_t total_rows,
                                std::uint64_t theme_token,
                                MergeTabState& merge_tab) {
  if (merge_tab.scrollbar_marker_cache_valid &&
      merge_tab.scrollbar_marker_cache_revision == merge_tab.model_revision &&
      merge_tab.scrollbar_marker_cache_rows == total_rows &&
      merge_tab.scrollbar_marker_cache_theme_token == theme_token &&
      RectsEqual(merge_tab.scrollbar_marker_cache_track, inner_lane)) {
    return;
  }

  const std::span<const MergeTrackedConflict> conflicts = MergeVisualConflicts(merge_tab);
  std::vector<overview::MarkerInput>& inputs = merge_tab.scrollbar_marker_input_scratch;
  inputs.clear();
  inputs.reserve(conflicts.size());
  for (const auto& conflict : conflicts) {
    const int start_row = static_cast<int>(std::min(
        {conflict.incoming_start_line, conflict.start_line, conflict.current_start_line}));
    const int end_row = static_cast<int>(std::max(
        {std::max(conflict.incoming_end_line, conflict.incoming_start_line + 1),
         std::max(conflict.end_line, conflict.start_line + 1),
         std::max(conflict.current_end_line, conflict.current_start_line + 1)}));
    inputs.push_back(overview::MarkerInput{
        .start_row = start_row,
        .end_row = end_row,
        .color = MergeMarkerColor(theme, conflict.last_choice, conflict.valid),
        .priority = 0});
  }
  overview::BuildMarkersInto(inner_lane, total_rows, inputs, merge_tab.scrollbar_marker_cache);
  merge_tab.scrollbar_marker_cache_track = inner_lane;
  merge_tab.scrollbar_marker_cache_revision = merge_tab.model_revision;
  merge_tab.scrollbar_marker_cache_rows = total_rows;
  merge_tab.scrollbar_marker_cache_theme_token = theme_token;
  merge_tab.scrollbar_marker_cache_valid = true;
}

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildMergeTextInputVisual(
    const SDL_FRect& editor_surface) const {
  const MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return std::nullopt;
  }

  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(editor_surface, *merge_tab);
  const MergeResultInteractionLayout interaction =
      BuildMergeResultInteractionLayout(editor_surface, surface_layout, *merge_tab);
  const float cursor_x =
      TextGridCursorX(interaction.text, merge_tab->result_viewport.cursor_visual_column());
  const float cursor_y =
      TextGridLineY(interaction.text, merge_tab->result_viewport.cursor_line());
  return TextInputVisual{
      .surface = TextInputSurface::Editor,
      .area = MakeRect(cursor_x, cursor_y - 1.0f, interaction.text.char_width,
                       interaction.text.line_height),
      .text_x = cursor_x,
      .text_y = cursor_y,
      .cursor_x = cursor_x,
      .foreground = theme_.text_primary,
      .background = theme_.editor_background,
      .displayed_text = {},
      .selection_bytes = std::nullopt,
  };
}

void WorkspaceShell::RenderMergeScrollbars(SDL_Renderer* renderer,
                                           const SDL_FRect& editor_surface,
                                           MergeTabState& merge_tab_state,
                                           const DiffSurfacePaint& paint) {
  MergeTabState* merge_tab = &merge_tab_state;
  if (renderer == nullptr) {
    return;
  }

  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(editor_surface, *merge_tab);
  const auto scroll_layout =
      ComputeMergeScrollLayout(editor_surface, surface_layout, *merge_tab);

  if (scroll_layout.vertical_scrollbar.has_value()) {
    const std::size_t line_count = MergeTotalVisualRowCount(*merge_tab);
    const SDL_FRect track = scroll_layout.vertical_scrollbar->track;
    const SDL_FRect lane = overview::LaneRect(track, editor_surface.x);
    const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
    EnsureMergeOverviewMarkers(theme_, inner_lane, line_count, overview::ThemeMarkerToken(theme_),
                               *merge_tab);
    overview::DrawLane(renderer, theme_, lane, merge_tab->scrollbar_marker_cache);
    detail::DrawScrollbarTrack(renderer, theme_, track);
    detail::DrawScrollbarThumb(renderer, theme_, scroll_layout.vertical_scrollbar->thumb,
                       paint.vertical_bar_dragged);
  }

  if (scroll_layout.horizontal_scrollbar.has_value()) {
    detail::DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb,
                  paint.horizontal_bar_dragged);
  }
}

void WorkspaceShell::RenderMergeSurface(SDL_Renderer* renderer,
                                        const SDL_FRect& rect,
                                        MergeTabState& merge_tab_state,
                                        bool focused,
                                        const DiffSurfacePaint& paint) {
  MergeTabState* merge_tab = &merge_tab_state;
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::RenderMergeSurface");
  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;
  static const editor::DecoratedTextGridRenderer kDecoratedRowRenderer;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(rect, *merge_tab);
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const MergeInteractionLayout interaction = BuildMergeInteractionLayout(rect, surface, *merge_tab);
  const std::size_t selected_hunk =
      merge_tab->conflicts.empty() ? 0 : std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);

  const auto draw_button = [&](const SDL_FRect& button_rect,
                               std::string_view label,
                               bool selected,
                               bool primary = false) {
    if (button_rect.w <= 0.0f || button_rect.h <= 0.0f) {
      return;
    }
    const std::string_view display = TruncateLabelView(label, button_rect.w - 18.0f);
    detail::DrawButtonCentered(editor_text_renderer_, renderer, theme_, button_rect, display,
                               detail::ButtonTone::Neutral,
                               detail::ButtonVisualState{
                                   .enabled = primary,
                                   .hovered = paint.pointer.has_value() && Contains(button_rect, paint.pointer->x,
                                                                                    paint.pointer->y),
                                   .active = selected,
                               });
  };
  const std::span<const MergeTrackedConflict> visual_conflicts = MergeVisualConflicts(*merge_tab);
  const auto conflict_at_source_row =
      [&](std::size_t visual_row, bool incoming) -> std::optional<std::size_t> {
    const auto conflict_index =
        FindMergeTrackedConflictAtSourceLine(*merge_tab, visual_row, incoming);
    return conflict_index.has_value() && *conflict_index < visual_conflicts.size()
               ? conflict_index
               : std::nullopt;
  };
  const auto source_button_rect = [&](const MergeTrackedConflict& conflict, bool incoming) {
    return BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
  };
  const auto result_action_rects =
      [&](const MergeTrackedConflict& conflict) -> std::array<SDL_FRect, 4> {
    return BuildMergeResultActionButtonRects(surface, interaction, conflict);
  };
  const auto preview_choice = [&]() -> std::optional<std::pair<std::size_t, compare::MergeChoice>> {
    if (!merge_tab->hover_state.has_value()) {
      return std::nullopt;
    }
    switch (merge_tab->hover_state->kind) {
      case MergeHoverState::Kind::IncomingConflict:
        case MergeHoverState::Kind::IncomingAccept:
        case MergeHoverState::Kind::CurrentConflict:
        case MergeHoverState::Kind::CurrentAccept:
        case MergeHoverState::Kind::ResultAction:
        return std::pair<std::size_t, compare::MergeChoice>{
            merge_tab->hover_state->conflict_index,
            merge_tab->hover_state->preview_choice,
        };
      case MergeHoverState::Kind::None:
      case MergeHoverState::Kind::ResultConflict:
      default:
        return std::nullopt;
    }
  }();

  {
  util::PerformanceTrace::Scope chrome_scope("WorkspaceShell::RenderMergeSurface::Chrome");
  const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(rect, surface);
  draw_button(toolbar.prev_rect, "Prev", false, !merge_tab->conflicts.empty());
  draw_button(toolbar.next_rect, "Next", false, !merge_tab->conflicts.empty());
  draw_button(toolbar.save_rect, "Save", false, true);
  draw_button(toolbar.open_rect, "Open Result", false, true);

  const SDL_FRect mark_resolved_rect =
      ComputeMergeSecondaryToolbarButtonRect(rect, surface, "Mark Resolved")
          .value_or(SDL_FRect{});
  const SDL_FRect toggle_base_rect =
      ComputeMergeSecondaryToolbarButtonRect(rect, surface, "Toggle Base").value_or(SDL_FRect{});
  const SDL_FRect unresolved_rect =
      ComputeMergeSecondaryToolbarButtonRect(rect, surface, "Unresolved").value_or(SDL_FRect{});
  const MergeResolverStatus resolver_status = BuildMergeResolverStatus(
      merge_tab->resolver_progress_buffer, *merge_tab, merge_tab->remaining_conflicted_files);
  const std::string_view status_text = merge_tab->status_message.empty()
                                           ? resolver_status.progress_label
                                           : std::string_view(merge_tab->status_message);
  float status_right = rect.x + rect.w - kMergeToolbarEdgeInset + kMergeToolbarButtonGap;
  for (const SDL_FRect& button : {mark_resolved_rect, toggle_base_rect, unresolved_rect}) {
    if (button.w > 0.0f) {
      status_right = button.x;
    }
  }
  const float status_max_width =
      std::max(0.0f, status_right - kMergeToolbarButtonGap - surface.left_x);
  editor_text_renderer_.DrawString(renderer, surface.left_x, surface.secondary_button_y,
                            theme_.text_secondary,
                            TruncateLabelView(status_text, status_max_width));
  draw_button(unresolved_rect, "Unresolved", false, !merge_tab->conflicts.empty());
  draw_button(toggle_base_rect, "Toggle Base", merge_tab->base_pane_visible, true);
  draw_button(mark_resolved_rect, "Mark Resolved", false, true);

  const float base_y = surface.secondary_button_y + kMergeToolbarButtonHeight + kMergeHeaderRowGap;
  const float base_band_height = surface.header_y - base_y;
  if (base_band_height > 0.5f) {
    DrawFilledRect(renderer, MakeRect(surface.left_x, base_y, content_width, base_band_height),
                   theme_.surface_raised);
    editor_text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, base_y + 2.0f,
                              theme_.text_secondary,
                              TruncateLabelView(merge_tab->base_label, content_width - 16.0f));
    for (std::size_t row = 0; row < merge_tab->model.base_lines.size() && row < 4; ++row) {
      const float y = base_y + 2.0f + surface.line_height * static_cast<float>(row + 1);
      editor_text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, y,
                                theme_.text_secondary,
                                TruncateLabelView(merge_tab->model.base_lines[row],
                                                  content_width - 16.0f));
    }
  }

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f),
                 theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 paint.left_divider_dragged ? theme_.accent : theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.right_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 paint.right_divider_dragged ? theme_.accent : theme_.border);

  }
  editor_text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->incoming_label, surface.left_width - 8.0f));
  editor_text_renderer_.DrawString(renderer, surface.center_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->result_label, surface.center_width - 8.0f));
  editor_text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->current_label, surface.right_width - 8.0f));

  {
  util::PerformanceTrace::Scope side_rows_scope("WorkspaceShell::RenderMergeSurface::SideRows");
  const bool wrapped = merge_tab->wrap_layout.active();
  const float char_width_px = editor_text_renderer_.CharWidth();
  struct MergeSideRow {
    const std::vector<std::string_view>* lines = nullptr;
    const MergeTrackedConflict* conflict = nullptr;
    bool selected = false;
    bool present = false;
    float pane_x = 0.0f;
    float pane_width = 0.0f;
    std::size_t indent = 0;
    std::size_t visual_start = 0;
    std::size_t visual_end = 0;
    SDL_Color valid_conflict_tint{};
  };
  const auto paint_side_row = [&](const MergeSideRow& side, const SurfaceTokenWindow& token_window,
                                  editor::DecoratedTextRow& scratch_row, std::size_t line_index,
                                  bool first_wrap_row, float y) {
    if (line_index >= side.lines->size() || !side.present) {
      return;
    }
    const SDL_Color base = side.selected ? theme_.row_highlight : theme_.editor_background;
    const SDL_Color background =
        side.conflict != nullptr
            ? render::BlendColors(base, side.conflict->valid ? side.valid_conflict_tint : theme_.diff_deleted,
                                  side.selected ? kMergeDiffRowTintSelected : kMergeDiffRowTint)
            : base;
    editor::RowDecorationInput input;
    input.text_x = side.pane_x + surface.gutter_width + static_cast<float>(side.indent) * char_width_px;
    input.y = y;
    input.char_width = char_width_px;
    input.line_height = surface.line_height;
    input.row_visual_start = side.visual_start;
    input.row_visual_end = side.visual_end;
    input.text = (*side.lines)[line_index];
    input.line_length = input.text.size();
    input.tokens = &token_window.Tokens(line_index);
    input.plain_color = side.selected ? theme_.text_primary : theme_.text_secondary;
    input.has_background_fill = true;
    input.background_fill = editor::DecoratedTextFill{
        .rect = MakeRect(side.pane_x, y - 1.0f, surface.gutter_width + side.pane_width,
                         surface.line_height),
        .color = background,
    };
    input.text_renderer = &editor_text_renderer_;
    input.theme = &theme_;
    editor::BuildDecoratedRow(scratch_row, input);
    kDecoratedRowRenderer.RenderRow(renderer, editor_text_renderer_, scratch_row);
    if (first_wrap_row) {
      std::array<char, 20> line_number_buf;
      editor_text_renderer_.DrawString(
          renderer, side.pane_x, y, side.selected ? theme_.current_line_number : theme_.line_number,
          FormatLineNumber(line_index + 1, line_number_buf));
    }
  };
  for (int row = 0; row < surface.visible_rows; ++row) {
    const std::size_t visual_row =
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row + row));
    if (visual_row >= MergeSourceVisualRowCount(*merge_tab)) {
      break;
    }
    const DiffWrapRow wrap_row = merge_tab->wrap_layout.RowAt(visual_row);
    const std::size_t line_index = wrap_row.unit;
    const float y = surface.rows_y + static_cast<float>(row) * surface.line_height;
    const std::optional<std::size_t> incoming_conflict_index =
        conflict_at_source_row(visual_row, true);
    const std::optional<std::size_t> current_conflict_index =
        conflict_at_source_row(visual_row, false);
    const MergeSideRow incoming{
        .lines = &merge_tab->model.incoming_lines,
        .conflict = incoming_conflict_index.has_value()
                        ? &merge_tab->conflicts[*incoming_conflict_index]
                        : nullptr,
        .selected = incoming_conflict_index.has_value() && *incoming_conflict_index == selected_hunk,
        .present = wrap_row.left_present,
        .pane_x = surface.left_x,
        .pane_width = surface.left_width,
        .indent = wrap_row.left_indent,
        .visual_start = wrapped ? wrap_row.left_start : merge_tab->horizontal_scroll,
        .visual_end = wrapped ? wrap_row.left_end
                              : merge_tab->horizontal_scroll + surface.visible_columns,
        .valid_conflict_tint = theme_.diff_added,
    };
    const MergeSideRow current{
        .lines = &merge_tab->model.current_lines,
        .conflict = current_conflict_index.has_value()
                        ? &merge_tab->conflicts[*current_conflict_index]
                        : nullptr,
        .selected = current_conflict_index.has_value() && *current_conflict_index == selected_hunk,
        .present = wrap_row.right_present,
        .pane_x = surface.right_x,
        .pane_width = surface.right_width,
        .indent = wrap_row.right_indent,
        .visual_start = wrapped ? wrap_row.right_start : merge_tab->horizontal_scroll,
        .visual_end = wrapped ? wrap_row.right_end
                              : merge_tab->horizontal_scroll + surface.visible_columns,
        .valid_conflict_tint = theme_.diff_modified,
    };
    paint_side_row(incoming, merge_tab->incoming_token_window, merge_incoming_scratch_row_,
                   line_index, wrap_row.first, y);
    paint_side_row(current, merge_tab->current_token_window, merge_current_scratch_row_,
                   line_index, wrap_row.first, y);
  }
  }

  const std::optional<editor::EditorBlameOverlay>& merge_blame_overlay = paint.blame;
  const bool bracket_match_highlight_enabled = paint.bracket_match;
  const bool indent_guides_enabled = paint.indent_guides;
  const bool render_whitespace_enabled = paint.render_whitespace;
  const bool line_numbers_enabled = paint.line_numbers;
  {
  const render::ScopedRenderClip result_clip(renderer, interaction.result.rect);
  editor_view_renderer_.Render(renderer, editor_text_renderer_, theme_, merge_tab->result_viewport,
                               interaction.result.rect,
                               focused && prepared_frame_draw_editor_caret_, "", std::nullopt,
                               merge_blame_overlay,
                               paint.diagnostics,
                               nullptr,
                               bracket_match_highlight_enabled,
                               indent_guides_enabled, render_whitespace_enabled,
                               nullptr,
                               nullptr,
                               nullptr,
                               line_numbers_enabled);
  }

  for (std::size_t i = 0; i < visual_conflicts.size(); ++i) {
    const auto& conflict = visual_conflicts[i];
    const std::optional<SDL_FRect> conflict_rect = ComputeVisibleLineRangeRect(
        interaction.result.rect, interaction.result.lines, conflict.start_line,
        std::max(conflict.end_line, conflict.start_line + std::size_t{1}));
    if (!conflict_rect.has_value()) {
      continue;
    }
    const SDL_Color border =
        !conflict.valid ? theme_.diff_deleted
        : i == selected_hunk ? theme_.accent
                             : theme_.border;
    DrawRect(renderer, *conflict_rect, border);
  }

  if (preview_choice.has_value() && preview_choice->first < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[preview_choice->first];
    const std::size_t preview_first_line =
        merge_tab->conflicts[preview_choice->first].start_line;
    const std::span<const std::string> preview_lines =
        EnsureMergePreviewLines(*merge_tab, preview_choice->first, preview_choice->second);
    if (!preview_lines.empty()) {
      const std::size_t preview_height_lines =
          std::max(preview_lines.size(), conflict.end_line > conflict.start_line
                                          ? conflict.end_line - conflict.start_line
                                          : std::size_t{1});
      const std::optional<SDL_FRect> preview_rect = ComputeVisibleLineRangeRect(
          interaction.result.rect, interaction.result.lines, conflict.start_line,
          conflict.start_line + preview_height_lines);
      if (preview_rect.has_value()) {
        DrawFilledRect(renderer, *preview_rect,
                       render::BlendColors(theme_.editor_background, theme_.diff_modified, 0.18f));
        for (std::size_t line = 0; line < preview_lines.size(); ++line) {
          std::array<char, 20> line_number_buf;
          const std::size_t content_line = conflict.start_line + line;
          if (content_line < interaction.result.lines.scroll_line) {
            continue;
          }
          const float y =
              interaction.result.lines.first_line_y +
              static_cast<float>(content_line - interaction.result.lines.scroll_line) *
                  interaction.result.lines.line_height;
          if (y >= interaction.result.rect.y + interaction.result.rect.h) {
            break;
          }
          editor_text_renderer_.DrawStringOn(renderer, interaction.result.rect.x, y, theme_.line_number,
                                      theme_.editor_background,
                                      FormatLineNumber(preview_first_line + line + 1,
                                                       line_number_buf));
          const std::string& preview_text = preview_lines[line];
          const editor::LayoutLine preview_layout = editor::TextLayout::BuildVisibleLine(
              preview_text, merge_tab->horizontal_scroll,
              interaction.result.metrics.visible_columns, merge_tab->result_viewport.tab_size());
          editor::RowDecorationInput preview_input;
          preview_input.text_x = interaction.result.metrics.text_x;
          preview_input.y = y;
          preview_input.char_width = editor_text_renderer_.CharWidth();
          preview_input.line_height = interaction.result.lines.line_height;
          preview_input.row_visual_start = merge_tab->horizontal_scroll;
          preview_input.row_visual_end =
              merge_tab->horizontal_scroll + interaction.result.metrics.visible_columns;
          preview_input.text = preview_text;
          preview_input.line_length = preview_input.text.size();
          preview_input.tokens = &kEmptyTokens;
          preview_input.plain_color = theme_.text_primary;
          preview_input.layout = &preview_layout;
          preview_input.tab_size = merge_tab->result_viewport.tab_size();
          preview_input.has_background_fill = false;
          preview_input.text_renderer = &editor_text_renderer_;
          preview_input.theme = &theme_;
          editor::DecoratedTextRow& preview_row = merge_incoming_scratch_row_;
          editor::BuildDecoratedRow(preview_row, preview_input);
          kDecoratedRowRenderer.RenderRow(renderer, editor_text_renderer_, preview_row);
        }
        DrawRect(renderer, *preview_rect, theme_.accent);
      }
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, true), "Accept Remote",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, false), "Accept Local",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      const auto action_rects = result_action_rects(conflict);
      draw_button(action_rects[0], "Base",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Base,
                  true);
      draw_button(action_rects[1], "Remote",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Incoming,
                  true);
      draw_button(action_rects[2], "Local",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Current,
                  true);
      draw_button(action_rects[3], "Both",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Both,
                  true);
    }
  }
}

}
