#include "workspace/shell/WorkspaceShell.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsRender.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/Theme.h"
#include "util/PerformanceTrace.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/render/CompareVisibleLayoutCache.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

namespace {

// Changed rows carry a faint full-row wash plus a saturated left-edge stripe. The wash
// alone (previously 0.12) read almost flush with unchanged rows on the dark theme; the
// stripe gives a scannable, full-saturation marker like the sidebar selection strip.
constexpr float kDiffRowTint = 0.17f;
constexpr float kDiffRowTintSelected = 0.26f;
constexpr float kDiffEdgeStripeWidth = 3.0f;
// Line numbers are inset past the edge stripe so the saturated change marker never
// sits underneath the gutter digits. Stripe width plus a small breathing gap.
constexpr float kDiffGutterNumberInset = kDiffEdgeStripeWidth + 3.0f;

SDL_Color CompareMarkerColor(const render::Theme& theme, compare::CompareRowKind kind) {
  switch (kind) {
    case compare::CompareRowKind::Added:
      return theme.diff_added;
    case compare::CompareRowKind::Deleted:
      return theme.diff_deleted;
    case compare::CompareRowKind::Modified:
      return theme.diff_modified;
    case compare::CompareRowKind::Unchanged:
    default:
      return theme.text_muted;
  }
}

// Rebuilds the cached overview markers for `compare_tab` when the presentation or the
// lane geometry changed. Markers are pre-mapped to `inner_lane` so the render path is a
// plain draw.
void EnsureCompareOverviewMarkers(const render::Theme& theme,
                                  const SDL_FRect& inner_lane,
                                  std::uint64_t theme_token,
                                  CompareTabState& compare_tab) {
  // The wrapped row count is part of what the markers are scaled against, so it is
  // part of the key: a divider drag that re-wraps must not keep stale marker rows.
  const std::uint64_t marker_revision =
      compare_tab.presentation_revision ^
      (static_cast<std::uint64_t>(CompareTabVisualRowCount(compare_tab)) << 32);
  if (compare_tab.scrollbar_marker_cache_valid &&
      compare_tab.scrollbar_marker_cache_revision == marker_revision &&
      compare_tab.scrollbar_marker_cache_theme_token == theme_token &&
      RectsEqual(compare_tab.scrollbar_marker_cache_track, inner_lane)) {
    return;
  }

  const std::vector<CompareScrollbarRun> runs =
      BuildCompareScrollbarRuns(compare_tab.presentation, compare_tab.model);
  std::vector<overview::MarkerInput> inputs;
  inputs.reserve(runs.size());
  for (const CompareScrollbarRun& run : runs) {
    // Runs are presentation rows; the lane is scaled to on-screen rows, which soft
    // wrap makes a different (larger) space. Project both ends through the wrap
    // table so a marker keeps pointing at its hunk.
    inputs.push_back(overview::MarkerInput{
        .start_row = static_cast<int>(ComparePresentationRowToVisualRow(
            compare_tab, static_cast<std::size_t>(std::max(0, run.start_row)))),
        .end_row = static_cast<int>(ComparePresentationRowToVisualRow(
            compare_tab, static_cast<std::size_t>(std::max(0, run.end_row)))),
        .color = CompareMarkerColor(theme, run.kind),
        .priority = 0});
  }
  compare_tab.scrollbar_marker_cache =
      overview::BuildMarkers(inner_lane, CompareTabVisualRowCount(compare_tab), inputs);
  compare_tab.scrollbar_marker_cache_track = inner_lane;
  compare_tab.scrollbar_marker_cache_revision = marker_revision;
  compare_tab.scrollbar_marker_cache_theme_token = theme_token;
  compare_tab.scrollbar_marker_cache_valid = true;
}

}  // namespace

void WorkspaceShell::PopulateCompareSyntaxTokensForWindow(CompareTabState& compare_tab,
                                                          std::size_t visible_start_row,
                                                          std::size_t visible_end_row) {
  if (!compare_tab.syntax_highlighting_enabled || compare_tab.model.rows.empty()) {
    return;
  }

  const std::size_t row_count = compare_tab.model.rows.size();
  const std::size_t clamped_end = std::min(visible_end_row, row_count);
  if (visible_start_row >= clamped_end) {
    return;
  }

  constexpr std::size_t kCompareSyntaxRowsPerFrame = 256;
  std::size_t target_row = std::min(clamped_end, compare_tab.syntax_rows_tokenized + kCompareSyntaxRowsPerFrame);
  while (compare_tab.syntax_rows_tokenized < target_row) {
    const std::size_t index = compare_tab.syntax_rows_tokenized;
    const auto& compare_row = compare_tab.model.rows[index];
    auto& left_tokens = compare_tab.left_tokens_by_row[index];
    auto& right_tokens = compare_tab.right_tokens_by_row[index];

    const bool reuse_tokens =
        compare_row.kind == compare::CompareRowKind::Unchanged && compare_row.left_line > 0 &&
        compare_row.right_line > 0 && compare_row.left_text == compare_row.right_text &&
        compare_tab.left_current_syntax_state == compare_tab.right_current_syntax_state;
    const bool has_alias_flags =
        index < compare_tab.right_tokens_alias_left_by_row.size();
    if (reuse_tokens && has_alias_flags) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.left_text, compare_tab.path, compare_tab.left_current_syntax_state);
      compare_tab.left_current_syntax_state = highlighted.end_state;
      compare_tab.right_current_syntax_state = highlighted.end_state;
      // One vector, two panes. This used to copy it into the left cache and move
      // it into the right, so an identical unchanged row cost two owned token
      // vectors to paint the same run twice (TD-2026-08-06-159).
      left_tokens = std::move(highlighted.tokens);
      right_tokens.clear();
      compare_tab.right_tokens_alias_left_by_row[index] = 1;
      ++compare_tab.syntax_rows_tokenized;
      continue;
    }
    if (has_alias_flags) {
      compare_tab.right_tokens_alias_left_by_row[index] = 0;
    }

    if (compare_row.left_line > 0) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.left_text, compare_tab.path, compare_tab.left_current_syntax_state);
      compare_tab.left_current_syntax_state = highlighted.end_state;
      left_tokens = std::move(highlighted.tokens);
    } else {
      left_tokens.clear();
    }

    if (compare_row.right_line > 0) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.right_text, compare_tab.path, compare_tab.right_current_syntax_state);
      compare_tab.right_current_syntax_state = highlighted.end_state;
      right_tokens = std::move(highlighted.tokens);
    } else {
      right_tokens.clear();
    }

    ++compare_tab.syntax_rows_tokenized;
  }
}

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer,
                                          const SDL_FRect& rect,
                                          CompareTabState& compare_tab_state,
                                          const std::filesystem::path& project_root,
                                          bool draw_compare_caret,
                                          const editor::DiagnosticsStore& diagnostics_store) {
  CompareTabState* compare_tab = &compare_tab_state;
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::RenderCompareSurface");
  ++render_compare_surface_invocation_count_;
  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;
  static const editor::DecoratedTextGridRenderer kDecoratedRowRenderer;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const CompareSurfaceLayout surface = ComputeCompareSurfaceLayout(rect, *compare_tab);
  ClampCompareScrollRow(*compare_tab, surface.visible_rows);
  ClampCompareHorizontalScroll(*compare_tab, surface.visible_columns);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, compare_tab->scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  // The compare view collapses large unchanged runs, so a visible presentation row
  // can map to a MODEL row far below its presentation index. The cumulative
  // tokenizer indexes by model row, so derive its window in MODEL space from the
  // presentation rows actually on screen (a CollapsedContext row spans through the
  // end of its run so rows below inherit the correct syntax state). Feeding the
  // presentation scroll position directly capped tokenization far below the visible
  // model rows, leaving hunks deep in a collapsed diff unhighlighted.
  const std::size_t presentation_row_count = compare_tab->presentation.rows.size();
  const auto model_row_for_visual = [&](std::size_t visual_index) -> std::size_t {
    if (presentation_row_count == 0) {
      return compare_tab->model.rows.size();
    }
    const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(
        *compare_tab,
        std::min(CompareVisualRowToPresentationRow(*compare_tab, visual_index),
                 presentation_row_count - 1));
    if (row == nullptr) {
      return compare_tab->model.rows.size();
    }
    if (row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
      return row->collapsed_run_start_model_row + row->collapsed_run_length;
    }
    return row->model_row_index;
  };
  const std::size_t syntax_model_start = model_row_for_visual(visible_start_row);
  const std::size_t syntax_model_end =
      std::min(compare_tab->model.rows.size(), model_row_for_visual(visible_end_row) + 1);
  PopulateCompareSyntaxTokensForWindow(*compare_tab, syntax_model_start, syntax_model_end);
  // The tokenizer is cumulative and capped per frame; if it has not yet reached the
  // deepest visible model row (e.g. after scrolling past a giant collapsed run),
  // request another frame so it progressively catches up instead of leaving those
  // rows permanently unhighlighted. Self-terminates once caught up (no idle redraw).
  if (compare_tab->syntax_rows_tokenized < syntax_model_end) {
    RequestRedrawRect(rect);
  }
  PrepareCompareVisibleLayoutsForWindow(
      *compare_tab, visible_start_row,
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)),
      surface.left_visible_columns, surface.right_visible_columns);
  const TextGridInteractionLayout right_interaction =
      BuildCompareRightInteractionLayout(surface, *compare_tab);
  const std::optional<editor::SelectionRange> right_selection =
      compare_tab->right_view_active ? compare_tab->right_viewport.selection_range() : std::nullopt;
  const std::optional<editor::EditorBlameOverlay> blame_overlay =
      compare_tab->right_editable && compare_tab->right_view_active
          ? editor_blame_overlay_service_.BuildCompareOverlay(
                project_root, text_renderer_, git_blame_service_, *compare_tab,
                EditorBlameOverlayService::CompareOverlayLayout{
                    .pane_rect = MakeRect(
                        surface.right_x, rect.y, surface.gutter_width + surface.right_width,
                        std::max(0.0f, rect.h - (surface.show_horizontal ? 12.0f : 0.0f))),
                    .right_x = surface.right_x,
                    .gutter_width = surface.gutter_width,
                    .right_width = surface.right_width,
                    .rows_y = surface.rows_y,
                    .line_height = surface.line_height,
                    .visible_rows = surface.visible_rows,
                    .visible_columns = surface.visible_columns,
                },
                [compare_tab](std::size_t line_index, std::size_t line_visual_columns)
                    -> std::optional<EditorBlameOverlayService::CompareBlameAnchor> {
                  const CompareRightLineAnchor anchor = CompareRightLineBlameAnchor(
                      *compare_tab, line_index, line_visual_columns);
                  if (!anchor.valid) {
                    return std::nullopt;
                  }
                  return EditorBlameOverlayService::CompareBlameAnchor{
                      .visual_row = anchor.visual_row,
                      .end_column = anchor.end_column,
                  };
                })
          : std::nullopt;
  const auto* right_diagnostics =
      compare_tab->right_editable && !compare_tab->right_viewport.path().empty() &&
              !compare_tab->right_viewport.dirty()
          ? diagnostics_store.FindByPathKey(compare_tab->right_viewport.path_key())
          : nullptr;
  editor_blame_overlay_service_.SetVisibleOverlay(blame_overlay);
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const float divider_x = surface.center_x + std::min(1.0f, std::max(0.0f, surface.divider_width - 1.0f));
  std::size_t blame_index = 0;

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f), theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x, rect.y, 1.0f, content_height),
                 theme_.border);
  DrawFilledRect(renderer, MakeRect(surface.right_x - 1.0f, rect.y, 1.0f, content_height),
                 theme_.border);

  if (!compare_tab->review_header.summary_line.empty()) {
    constexpr std::string_view kShortcutHint = "Ctrl+E  keyboard-shortcuts";
    const float hint_width = text_renderer_.MeasureWidth(kShortcutHint);
    const float hint_x = rect.x + std::max(8.0f, content_width - hint_width - 10.0f);
    const float summary_width =
        std::max(0.0f, (hint_x - 14.0f) - (rect.x + 8.0f));
    text_renderer_.DrawString(renderer, rect.x + 8.0f, surface.review_summary_y,
                              theme_.text_primary,
                              TruncateLabelView(compare_tab->review_header.summary_line,
                                                summary_width));
    text_renderer_.DrawString(renderer, hint_x, surface.review_summary_y,
                              theme_.text_muted, kShortcutHint);
  }
  text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(compare_tab->left_label, surface.left_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(compare_tab->right_label, surface.right_width - 8.0f));

  {
  // The compare surface paints two side panes here; the row loop was the whole of
  // RenderCompareSurface's 74 us-per-frame self time and had no scope of its own,
  // exactly as the merge surface's did.
  util::PerformanceTrace::Scope side_rows_scope("WorkspaceShell::RenderCompareSurface::SideRows");
  const bool wrapped = compare_tab->wrap_layout.active();
  const float char_width_px = text_renderer_.CharWidth();
  for (int row = 0; row < surface.visible_rows; ++row) {
    const int visual_index = compare_tab->scroll_row + row;
    if (visual_index < 0 ||
        static_cast<std::size_t>(visual_index) >= CompareTabVisualRowCount(*compare_tab)) {
      break;
    }
    const DiffWrapRow wrap_row =
        compare_tab->wrap_layout.RowAt(static_cast<std::size_t>(visual_index));
    const std::size_t presentation_index = wrap_row.unit;

    const compare::ComparePresentationRow* presentation_row =
        CompareTabPresentationRowAt(*compare_tab, presentation_index);
    if (presentation_row == nullptr) {
      break;
    }

    const float y = surface.rows_y + static_cast<float>(row) * surface.line_height;
    // The whole wrapped block of the selected row highlights, not just its first
    // segment -- selection is a presentation-row concept.
    const bool selected = presentation_index == compare_tab->selected_row;
    if (presentation_row->kind != compare::ComparePresentationRowKind::Model) {
      const SDL_Color summary_background = selected
                                               ? theme_.row_highlight
                                               : render::BlendColors(theme_.editor_background,
                                                                     theme_.surface_background, 0.24f);
      const SDL_FRect row_rect =
          MakeRect(rect.x, y - 1.0f, content_width, surface.line_height);
      DrawFilledRect(renderer, row_rect, summary_background);
      const auto draw_context_button = [&](const SDL_FRect& button_rect,
                                           std::string_view label,
                                           bool hovered) {
        const bool emphasized = selected || hovered;
        const SDL_Color fill = emphasized
                                   ? theme_.chrome_active
                                   : render::BlendColors(theme_.surface_raised,
                                                         theme_.editor_background, 0.36f);
        DrawFilledRect(renderer, button_rect, fill);
        DrawRect(renderer, button_rect, emphasized ? theme_.text_secondary : theme_.border);
        const float text_x =
            button_rect.x +
            std::max(0.0f, (button_rect.w - text_renderer_.MeasureWidth(label)) * 0.5f);
        const float text_y =
            button_rect.y +
            std::max(0.0f, (button_rect.h - text_renderer_.LineHeight()) * 0.5f);
        text_renderer_.DrawStringOn(renderer, text_x, text_y,
                                    emphasized ? theme_.text_primary : theme_.text_muted,
                                    fill, label);
      };
      const std::string& summary = presentation_row->display_summary_text;
      float summary_width = content_width - 16.0f;
      if (presentation_row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
        const SDL_FRect block_rect = CompareCollapsedContextBlockRect(
            rect, surface.rows_y, surface.line_height, surface.show_vertical, row);
        DrawFilledRect(renderer, block_rect,
                       selected ? theme_.chrome_active
                                : render::BlendColors(theme_.surface_raised, theme_.editor_background,
                                                      0.30f));
        DrawRect(renderer, block_rect, selected ? theme_.accent : theme_.border);
        const auto action_rects = BuildCollapsedContextActionRects(
            text_renderer_, block_rect, presentation_row->previous_hunk_index >= 0,
            presentation_row->next_hunk_index >= 0);
        const auto hovered_action = [&compare_tab, presentation_index,
                                     presentation_row](CompareHoverKind kind) {
          return compare_tab->hover_state.has_value() &&
                 compare_tab->hover_state->kind == kind &&
                 compare_tab->hover_state->presentation_row == presentation_index &&
                 compare_tab->hover_state->collapsed_run_start_model_row ==
                     presentation_row->collapsed_run_start_model_row &&
                 compare_tab->hover_state->collapsed_run_length ==
                     presentation_row->collapsed_run_length;
        };
        const float summary_x = block_rect.x + 8.0f;
        summary_width = std::max(0.0f, action_rects.text_right_edge - summary_x);
        if (action_rects.previous_rect.has_value()) {
          draw_context_button(*action_rects.previous_rect, "Show previous 20",
                              hovered_action(CompareHoverKind::CollapsedContextPreviousAction));
        }
        draw_context_button(action_rects.all_rect, "Show all",
                            hovered_action(CompareHoverKind::CollapsedContextAllAction));
        if (action_rects.next_rect.has_value()) {
          draw_context_button(*action_rects.next_rect, "Show next 20",
                              hovered_action(CompareHoverKind::CollapsedContextNextAction));
        }
        text_renderer_.DrawString(renderer, summary_x, y,
                                  selected ? theme_.text_primary : theme_.text_secondary,
                                  TruncateLabelView(summary, summary_width));
      } else {
        text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, y,
                                  selected ? theme_.text_primary : theme_.text_muted,
                                  TruncateLabelView(summary, summary_width));
      }
      continue;
    }

    const std::size_t model_index = presentation_row->model_row_index;
    if (model_index >= compare_tab->model.rows.size()) {
      break;
    }

    const auto& compare_row = compare_tab->model.rows[model_index];
    if (selected) {
      DrawFilledRect(renderer, MakeRect(rect.x + 1.0f, y - 1.0f, 2.0f, surface.line_height),
                     theme_.accent);
      DrawFilledRect(renderer,
                     MakeRect(rect.x + content_width - 3.0f, y - 1.0f, 2.0f, surface.line_height),
                     theme_.accent);
    }

    const auto draw_text = [&](float x, float width, SDL_Color color, std::string_view text) {
      const std::string_view display_text = TruncateLabelView(text, width);
      if (display_text.empty()) {
        return;
      }
      text_renderer_.DrawString(renderer, x, y, color, display_text);
    };
    const SDL_Color neutral_text_color = selected ? theme_.text_primary : theme_.text_secondary;
    SDL_Color left_color = neutral_text_color;
    SDL_Color right_color = neutral_text_color;
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        marker_color = theme_.diff_added;
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        marker_color = theme_.diff_deleted;
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        marker_color = theme_.diff_modified;
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }
    const SDL_Color left_row_background = [&]() {
      const SDL_Color base = selected ? theme_.row_highlight : theme_.editor_background;
      switch (compare_row.kind) {
        case compare::CompareRowKind::Deleted:
          return render::BlendColors(base, theme_.diff_deleted,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Modified:
          return render::BlendColors(base, theme_.diff_modified,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Added:
        case compare::CompareRowKind::Unchanged:
        default:
          return base;
      }
    }();
    const SDL_Color right_row_background = [&]() {
      const SDL_Color base = selected ? theme_.row_highlight : theme_.editor_background;
      switch (compare_row.kind) {
        case compare::CompareRowKind::Added:
          return render::BlendColors(base, theme_.diff_added,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Modified:
          return render::BlendColors(base, theme_.diff_modified,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Deleted:
        case compare::CompareRowKind::Unchanged:
        default:
          return base;
      }
    }();

    const std::size_t left_row_start = wrapped ? wrap_row.left_start : compare_tab->horizontal_scroll;
    const std::size_t left_row_end =
        wrapped ? wrap_row.left_end : compare_tab->horizontal_scroll + surface.left_visible_columns;
    const std::size_t right_row_start =
        wrapped ? wrap_row.right_start : compare_tab->horizontal_scroll;
    const std::size_t right_row_end = wrapped
                                          ? wrap_row.right_end
                                          : compare_tab->horizontal_scroll +
                                                surface.right_visible_columns;
    if (compare_row.left_line > 0 && wrap_row.left_present) {
      std::array<char, 20> line_number_buf;
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->left_tokens_by_row.size()
              ? &compare_tab->left_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      const std::vector<compare::CompareTextSpan>& left_changed_spans =
          compare::CompareInlineLeftSpans(compare_tab->presentation, compare_tab->model, model_index);
      const bool left_diff_edge = compare_row.kind == compare::CompareRowKind::Deleted ||
                                  compare_row.kind == compare::CompareRowKind::Modified;
      editor::RowDecorationInput left_input;
      // Continuation rows render under the line's own indent (hanging indent), so
      // the glyph origin shifts right by it while the visual span stays absolute.
      left_input.text_x = surface.left_x + surface.gutter_width +
                          static_cast<float>(wrap_row.left_indent) * char_width_px;
      left_input.y = y;
      left_input.char_width = char_width_px;
      left_input.line_height = surface.line_height;
      left_input.row_visual_start = left_row_start;
      left_input.row_visual_end = left_row_end;
      left_input.text = compare_row.left_text;
      left_input.line_length = left_input.text.size();
      left_input.tokens = cached_tokens;
      left_input.plain_color = selected ? theme_.text_primary : left_color;
      left_input.has_background_fill = true;
      left_input.background_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.left_x, y - 1.0f,
                           surface.gutter_width + surface.left_width, surface.line_height),
          .color = left_row_background,
      };
      left_input.has_edge_stripe = left_diff_edge;
      if (left_diff_edge) {
        left_input.edge_stripe_fill = editor::DecoratedTextFill{
            .rect = MakeRect(surface.left_x, y - 1.0f, kDiffEdgeStripeWidth, surface.line_height),
            .color = compare_row.kind == compare::CompareRowKind::Deleted ? theme_.diff_deleted
                                                                          : theme_.diff_modified,
        };
      }
      left_input.changed_spans =
          std::span<const compare::CompareTextSpan>(left_changed_spans);
      left_input.changed_span_color = compare_row.kind == compare::CompareRowKind::Deleted
                                          ? theme_.diff_deleted
                                          : theme_.diff_modified;
      left_input.layout = CompareVisibleLayoutForRow(
          *compare_tab, model_index, false, left_row_start,
          wrapped ? left_row_end - left_row_start : surface.left_visible_columns);
      left_input.text_renderer = &text_renderer_;
      left_input.theme = &theme_;
      editor::DecoratedTextRow& left_row = compare_left_scratch_row_;
      editor::BuildDecoratedRow(left_row, left_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, left_row);
      if (wrap_row.first) {
        draw_text(surface.left_x + kDiffGutterNumberInset,
                  surface.gutter_width - 4.0f - kDiffGutterNumberInset,
                  selected ? theme_.current_line_number : theme_.line_number,
                  FormatLineNumber(static_cast<std::size_t>(compare_row.left_line),
                                   line_number_buf));
      }
    } else if (wrapped && compare_row.left_line > 0) {
      // Padding row: this side ran out of segments before the other did. Paint the
      // row band so the pane stays a continuous column of the diff tint.
      DrawFilledRect(renderer,
                     MakeRect(surface.left_x, y - 1.0f,
                              surface.gutter_width + surface.left_width, surface.line_height),
                     left_row_background);
    }
    if (compare_row.right_line > 0 && wrap_row.right_present) {
      std::array<char, 20> line_number_buf;
      const std::size_t right_line_index = static_cast<std::size_t>(compare_row.right_line - 1);
      // The same right_text is queried for the selection fill and (potentially) the caret
      // visual column below. Build the boundary→visual column table once and reuse it instead
      // of re-walking the line per query.
      // A whole-line boundary->visual map is 16 bytes/char. On a pathological
      // megaline (a minified bundle, or a single-line multi-hundred-MB right
      // side) building it per visible row would be an O(line) walk and an
      // ~8 GB allocation. Past the cap, skip the map: selection fills and the
      // caret fall back to the identity column mapping (as the left side already
      // does when it passes no map), keeping compare rendering bounded.
      constexpr std::size_t kMaxCompareVisualMapBytes = 1u << 20;  // 1 MiB
      // A line with no tab and no multi-byte code point maps byte offset to visual
      // column exactly, which is what the null-map fallback already computes — so
      // building the table for it is two heap vectors and an O(line) fill per
      // visible row for an answer the fallback gets for free. Most source lines are
      // that shape, and this was 87 % of a compare scroll-with-selection frame's
      // allocations (TD-2026-08-06-159).
      const bool right_map_allowed =
          compare_row.right_text.size() <= kMaxCompareVisualMapBytes &&
          !editor::TextLayout::VisualColumnsAreIdentity(compare_row.right_text);
      std::optional<editor::TextLayout::LineVisualColumnMap> right_visual_map;
      auto ensure_right_visual_map = [&]() -> const editor::TextLayout::LineVisualColumnMap* {
        if (!right_map_allowed) {
          return nullptr;
        }
        if (!right_visual_map.has_value()) {
          right_visual_map.emplace(compare_row.right_text,
                                   compare_tab->right_viewport.tab_size());
        }
        return &*right_visual_map;
      };
      std::array<editor::RowFillSpan, 1> right_column_fills;
      std::size_t right_column_fill_count = 0;
      if (right_selection.has_value()) {
        // Copy out of the optional so GCC's optimizer sees a definitely-initialized
        // SelectionRange instead of complaining about `*right_selection` storage
        // bytes through the inlined `std::optional` access path.
        const editor::SelectionRange sel = *right_selection;
        if (right_line_index >= sel.start.line && right_line_index <= sel.end.line) {
          const std::size_t line_start =
              right_line_index == sel.start.line ? sel.start.column : 0;
          const std::size_t line_end =
              right_line_index == sel.end.line ? sel.end.column
                                               : compare_row.right_text.size();
          // Resolution happens in the builder against this map; build it now so the
          // caret below reuses the same table.
          ensure_right_visual_map();
          right_column_fills[right_column_fill_count++] = editor::RowFillSpan{
              .start_column = line_start,
              .end_column = line_end,
              .color = theme_.selection_fill,
              .geometry = editor::RowFillSpan::Geometry::kRange,
          };
        }
      }
      // An unchanged row whose two sides are byte-identical stores its tokens once,
      // in the left cache; the flag says which rows those are.
      const std::size_t right_token_row = static_cast<std::size_t>(model_index);
      const bool right_aliases_left =
          right_token_row < compare_tab->right_tokens_alias_left_by_row.size() &&
          compare_tab->right_tokens_alias_left_by_row[right_token_row] != 0 &&
          right_token_row < compare_tab->left_tokens_by_row.size();
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          right_aliases_left ? &compare_tab->left_tokens_by_row[right_token_row]
          : right_token_row < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[right_token_row]
              : &kEmptyTokens;
      const std::vector<compare::CompareTextSpan>& right_changed_spans =
          compare::CompareInlineRightSpans(compare_tab->presentation, compare_tab->model,
                                           model_index);
      const bool right_diff_edge = compare_row.kind == compare::CompareRowKind::Added ||
                                   compare_row.kind == compare::CompareRowKind::Modified;
      editor::RowDecorationInput right_input;
      right_input.text_x = right_interaction.text_x +
                           static_cast<float>(wrap_row.right_indent) * char_width_px;
      right_input.y = y;
      right_input.char_width = right_interaction.char_width;
      right_input.line_height = surface.line_height;
      right_input.row_visual_start = right_row_start;
      right_input.row_visual_end = right_row_end;
      right_input.text = compare_row.right_text;
      right_input.line_length = right_input.text.size();
      right_input.tokens = cached_tokens;
      right_input.plain_color = selected ? theme_.text_primary : right_color;
      right_input.visual_map = right_visual_map.has_value() ? &*right_visual_map : nullptr;
      right_input.has_background_fill = true;
      right_input.background_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.right_x, y - 1.0f,
                           surface.gutter_width + surface.right_width, surface.line_height),
          .color = right_row_background,
      };
      right_input.has_edge_stripe = right_diff_edge;
      if (right_diff_edge) {
        right_input.edge_stripe_fill = editor::DecoratedTextFill{
            .rect = MakeRect(surface.right_x, y - 1.0f, kDiffEdgeStripeWidth, surface.line_height),
            .color = compare_row.kind == compare::CompareRowKind::Added ? theme_.diff_added
                                                                        : theme_.diff_modified,
        };
      }
      right_input.column_fills =
          std::span<const editor::RowFillSpan>(right_column_fills.data(), right_column_fill_count);
      right_input.changed_spans =
          std::span<const compare::CompareTextSpan>(right_changed_spans);
      right_input.changed_span_color = compare_row.kind == compare::CompareRowKind::Added
                                           ? theme_.diff_added
                                           : theme_.diff_modified;
      right_input.layout = CompareVisibleLayoutForRow(
          *compare_tab, model_index, true, right_row_start,
          wrapped ? right_row_end - right_row_start : surface.right_visible_columns);
      if (right_diagnostics != nullptr) {
        right_input.diagnostics =
            std::span<const editor::PublishedDiagnostic>(*right_diagnostics);
        right_input.diagnostic_line_index = right_line_index;
        right_input.diagnostic_horizontal_scroll = right_row_start;
        right_input.diagnostic_visible_columns =
            wrapped ? right_row_end - right_row_start : surface.right_visible_columns;
        right_input.tab_size = compare_tab->right_viewport.tab_size();
      }
      right_input.text_renderer = &text_renderer_;
      right_input.theme = &theme_;
      editor::DecoratedTextRow& right_row = compare_right_scratch_row_;
      editor::BuildDecoratedRow(right_row, right_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, right_row);
      if (right_diagnostics != nullptr && wrap_row.first) {
        if (const auto severity = editor::HighestDiagnosticSeverityForLine(
                std::span<const editor::PublishedDiagnostic>(*right_diagnostics), right_line_index);
            severity.has_value()) {
          editor::DrawDiagnosticGutterMarker(renderer, theme_, surface.right_x, y,
                                             surface.gutter_width, surface.line_height,
                                             *severity);
        }
      }
      if (wrap_row.first) {
        draw_text(surface.right_x + kDiffGutterNumberInset,
                  surface.gutter_width - 4.0f - kDiffGutterNumberInset,
                  selected ? theme_.current_line_number : theme_.line_number,
                  FormatLineNumber(static_cast<std::size_t>(compare_row.right_line),
                                   line_number_buf));
      }
      if (draw_compare_caret && right_line_index == compare_tab->right_viewport.cursor_line()) {
        const editor::TextLayout::LineVisualColumnMap* caret_map = ensure_right_visual_map();
        const std::size_t caret_visual =
            caret_map != nullptr
                ? caret_map->VisualColumnFor(compare_tab->right_viewport.cursor_column())
                : std::min(compare_tab->right_viewport.cursor_column(),
                           compare_row.right_text.size());
        // Under wrap the caret belongs to exactly one segment, and its on-screen
        // cell is the hanging indent plus the offset into that segment.
        const bool caret_on_this_row =
            wrapped ? compare_tab->wrap_layout.RowForUnitColumn(
                          presentation_index, caret_visual, /*right_side=*/true) ==
                          static_cast<std::size_t>(visual_index)
                    : caret_visual >= right_row_start && caret_visual <= right_row_end;
        if (caret_on_this_row) {
          const std::size_t caret_cell =
              wrapped ? wrap_row.right_indent + (caret_visual - right_row_start) : caret_visual;
          DrawFilledRect(
              renderer, MakeRect(TextGridCursorX(right_interaction, caret_cell), y - 1.0f, 1.5f,
                                 surface.line_height),
              theme_.cursor);
        }
      }
      // The annotation is anchored past the END of the line, i.e. on its last
      // wrapped row; the rect is absolute, so painting it once there is enough.
      const bool blame_row =
          !wrapped || compare_tab->wrap_layout.RowForUnitColumn(
                          presentation_index, std::numeric_limits<std::size_t>::max(),
                          /*right_side=*/true) == static_cast<std::size_t>(visual_index);
      if (blame_row && blame_overlay.has_value() && blame_overlay->visible) {
        while (blame_index < blame_overlay->lines.size() &&
               blame_overlay->lines[blame_index].line_index < right_line_index) {
          ++blame_index;
        }
        if (blame_index < blame_overlay->lines.size() &&
            blame_overlay->lines[blame_index].line_index == right_line_index) {
          text_renderer_.DrawStringOn(renderer, blame_overlay->lines[blame_index].rect.x,
                                     blame_overlay->lines[blame_index].rect.y,
                                     theme_.text_disabled, right_row_background,
                                     blame_overlay->lines[blame_index].text);
        }
      }
    } else if (wrapped && compare_row.right_line > 0) {
      DrawFilledRect(renderer,
                     MakeRect(surface.right_x, y - 1.0f,
                              surface.gutter_width + surface.right_width, surface.line_height),
                     right_row_background);
    }
    if (wrap_row.first) {
      draw_text(divider_x, surface.divider_width, marker_color, std::string_view(&marker, 1));
    }
  }
  }
}

void WorkspaceShell::RenderCompareScrollbars(SDL_Renderer* renderer,
                                             const SDL_FRect& editor_surface,
                                             CompareTabState& compare_tab_state) {
  CompareTabState* compare_tab = &compare_tab_state;
  if (renderer == nullptr) {
    return;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(editor_surface, *compare_tab);
  const auto scroll_layout =
      ComputeCompareScrollLayout(editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;

  if (scroll_layout.vertical_scrollbar.has_value()) {
    const SDL_FRect track = scroll_layout.vertical_scrollbar->track;
    const SDL_FRect lane = overview::LaneRect(track, editor_surface.x);
    const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
    EnsureCompareOverviewMarkers(theme_, inner_lane, overview::ThemeMarkerToken(theme_),
                                 *compare_tab);
    overview::DrawLane(renderer, theme_, lane, compare_tab->scrollbar_marker_cache);
    detail::DrawScrollbarTrack(renderer, theme_, track);
    detail::DrawScrollbarThumb(renderer, theme_, scroll_layout.vertical_scrollbar->thumb,
                       context_.interaction_state.drag_target == DragTarget::CompareVerticalScrollbar);
  }

  if (scroll_layout.horizontal_scrollbar.has_value()) {
    detail::DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::CompareHorizontalScrollbar);
  }
}

}  // namespace microide::workspace
