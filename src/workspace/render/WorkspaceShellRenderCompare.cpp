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
#include "editor/GutterIconRegistry.h"
#include "editor/PluginDecorationStore.h"
#include "editor/IndentGuides.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/Theme.h"
#include "util/PerformanceTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/DiffOverviewMarkers.h"
#include "workspace/render/CompareRowUnits.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/render/CompareVisibleLayoutCache.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

namespace {

constexpr float kDiffRowTint = 0.17f;
constexpr float kDiffRowTintSelected = 0.26f;
constexpr float kDiffTokenTint = 0.42f;
constexpr float kDiffEdgeStripeWidth = 3.0f;
constexpr float kDiffGutterNumberInset = kDiffEdgeStripeWidth + 3.0f;

struct DiffSideStyle {
  SDL_Color background{};
  bool stripe = false;
  SDL_Color stripe_color{};
  SDL_Color token_fill{};
};

DiffSideStyle StyleDiffSide(const render::Theme& theme, compare::CompareSideChange change,
                            SDL_Color side_color, bool selected) {
  const SDL_Color base = selected ? theme.row_highlight : theme.editor_background;
  DiffSideStyle style{.background = base};
  switch (change) {
    case compare::CompareSideChange::Moved:
      side_color = theme.diff_moved;
      [[fallthrough]];
    case compare::CompareSideChange::Novel:
      style.background =
          render::BlendColors(base, side_color, selected ? kDiffRowTintSelected : kDiffRowTint);
      style.stripe = true;
      style.stripe_color = side_color;
      style.token_fill = render::BlendColors(base, side_color, kDiffTokenTint);
      break;
    case compare::CompareSideChange::LayoutOnly:
      style.stripe = true;
      style.stripe_color = theme.text_disabled;
      break;
    case compare::CompareSideChange::None:
      break;
  }
  return style;
}

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

void ComputeCompareIndentGuides(const CompareTabState* compare_tab, const CompareSurfaceLayout& surface,
                                std::vector<editor::IndentGuideRun>& compare_guide_left_runs_,
                                std::vector<editor::IndentGuideRun>& compare_guide_right_runs_) {
  thread_local std::vector<std::string_view> compare_guide_left_texts_;
  thread_local std::vector<std::string_view> compare_guide_right_texts_;
  thread_local std::vector<std::size_t> compare_guide_row_indices_;
  compare_guide_left_texts_.clear();
  compare_guide_right_texts_.clear();
  compare_guide_left_texts_.reserve(static_cast<std::size_t>(surface.visible_rows));
  compare_guide_right_texts_.reserve(static_cast<std::size_t>(surface.visible_rows));
  for (int row = 0; row < surface.visible_rows; ++row) {
    const int visual_index = compare_tab->scroll_row + row;
    if (visual_index < 0 || static_cast<std::size_t>(visual_index) >= CompareTabVisualRowCount(*compare_tab)) {
      break;
    }
    const DiffWrapRow guide_wrap_row = compare_tab->wrap_layout.RowAt(static_cast<std::size_t>(visual_index));
    const compare::ComparePresentationRow* guide_row = CompareTabPresentationRowAt(*compare_tab, guide_wrap_row.unit);
    const compare::CompareRow* model_row = guide_row != nullptr &&
                                                   guide_row->kind == compare::ComparePresentationRowKind::Model &&
                                                   guide_row->model_row_index < compare_tab->model.rows.size()
                                               ? &compare_tab->model.rows[guide_row->model_row_index]
                                               : nullptr;
    compare_guide_left_texts_.push_back(model_row != nullptr && guide_wrap_row.left_present
                                            ? std::string_view(model_row->left_text)
                                            : std::string_view{});
    compare_guide_right_texts_.push_back(model_row != nullptr && guide_wrap_row.right_present
                                             ? std::string_view(model_row->right_text)
                                             : std::string_view{});
  }
  compare_guide_row_indices_.resize(compare_guide_left_texts_.size());
  for (std::size_t i = 0; i < compare_guide_row_indices_.size(); ++i) {
    compare_guide_row_indices_[i] = i;
  }
  const std::size_t tab_size = compare_tab->right_viewport.tab_size();
  const std::size_t indent_width =
      compare_tab->right_viewport.indent_width() == 0 ? 1 : compare_tab->right_viewport.indent_width();
  editor::ComputeIndentGuides(editor::LineSpan(compare_guide_left_texts_), compare_guide_row_indices_, tab_size,
                              indent_width,
                              SIZE_MAX, 0, &compare_guide_left_runs_);
  editor::ComputeIndentGuides(editor::LineSpan(compare_guide_right_texts_), compare_guide_row_indices_, tab_size,
                              indent_width,
                              SIZE_MAX, 0, &compare_guide_right_runs_);
}

struct CompareControlPainter {
  SDL_Renderer* renderer = nullptr;
  const render::Theme& theme;
  render::TextRenderer& text;
  const CompareTabState& compare_tab;

  void DrawLabelBox(const SDL_FRect& box, std::string_view label, SDL_Color fill, SDL_Color border,
                    SDL_Color text_color) const {
    render::FillRect(renderer, box, fill);
    render::OutlineRect(renderer, box, border);
    text.DrawStringOn(renderer, box.x + std::max(0.0f, (box.w - text.MeasureWidth(label)) * 0.5f),
                      box.y + std::max(0.0f, (box.h - text.LineHeight()) * 0.5f), text_color, fill, label);
  }
  void DrawButton(const SDL_FRect& button_rect, std::string_view label, bool hovered, bool emphasized = false) const {
    const bool strong = hovered || emphasized;
    DrawLabelBox(
        button_rect, label,
        strong ? theme.chrome_active : render::BlendColors(theme.surface_raised, theme.editor_background, 0.36f),
        strong ? theme.text_secondary : theme.border, strong ? theme.text_primary : theme.text_muted);
  }
  bool Hovered(CompareHoverKind kind, std::size_t presentation_row, int hunk) const {
    return compare_tab.hover_state.has_value() && compare_tab.hover_state->kind == kind &&
           compare_tab.hover_state->presentation_row == presentation_row && compare_tab.hover_state->hunk == hunk;
  }
};

void PaintCompareSummaryRow(const CompareControlPainter& control_painter, render::TextRenderer& ui_text,
                            const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                            const CompareControlGeometry& control_geometry, float content_width,
                            const compare::ComparePresentationRow* presentation_row, std::size_t presentation_index,
                            int row, float y, bool selected) {
  SDL_Renderer* const renderer = control_painter.renderer;
  const render::Theme& theme = control_painter.theme;
  render::TextRenderer& text = control_painter.text;
  const CompareTabState* const compare_tab = &control_painter.compare_tab;
  const SDL_Color summary_background =
      selected ? theme.row_highlight : render::BlendColors(theme.editor_background, theme.surface_background, 0.24f);
  const SDL_FRect row_rect = MakeRect(rect.x, y - 1.0f, content_width, surface.line_height);
  render::FillRect(renderer, row_rect, summary_background);
  const std::string& summary = presentation_row->display_summary_text;
  float summary_width = content_width - 16.0f;
  if (presentation_row->kind == compare::ComparePresentationRowKind::ReviewNote) {
    const CompareNoteActionRects note = BuildCompareNoteActionRects(text, control_geometry, row);
    const SDL_Color block_fill = render::BlendColors(theme.editor_background, theme.accent, 0.16f);
    render::FillRect(renderer, note.block, block_fill);
    render::OutlineRect(renderer, note.block, render::BlendColors(theme.border, theme.accent, 0.5f));
    render::FillRect(renderer, MakeRect(note.block.x, note.block.y, 3.0f, note.block.h), theme.accent);
    const std::string_view caption = CompareNoteCaption(*compare_tab, presentation_row->hunk_index);
    const float caption_x = note.text_x;
    const float caption_width = text.MeasureWidth(caption);
    const float text_limit = note.text_right_edge;
    if (caption_x + caption_width < text_limit) {
      text.DrawStringOn(renderer, caption_x, y, theme.accent, block_fill, caption);
      text.DrawStringOn(
          renderer, caption_x + caption_width, y, theme.text_primary, block_fill,
          ui_text.TruncateToWidthEphemeralView(presentation_row->summary_text, text_limit - caption_x - caption_width));
    }
    control_painter.DrawButton(
        note.edit, kCompareNoteEditLabel,
        control_painter.Hovered(CompareHoverKind::NoteEdit, presentation_index, presentation_row->hunk_index));
    control_painter.DrawButton(
        note.remove, kCompareNoteDeleteLabel,
        control_painter.Hovered(CompareHoverKind::NoteDelete, presentation_index, presentation_row->hunk_index));
    return;
  }
  if (presentation_row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
    const SDL_FRect block_rect =
        CompareCollapsedContextBlockRect(rect, surface.rows_y, surface.line_height, surface.show_vertical, row);
    render::FillRect(
        renderer, block_rect,
        selected ? theme.chrome_active : render::BlendColors(theme.surface_raised, theme.editor_background, 0.30f));
    render::OutlineRect(renderer, block_rect, selected ? theme.accent : theme.border);
    const auto action_rects = BuildCollapsedContextActionRects(
        text, block_rect, presentation_row->previous_hunk_index >= 0, presentation_row->next_hunk_index >= 0);
    const auto hovered_action = [&compare_tab, presentation_index, presentation_row](CompareHoverKind kind) {
      return compare_tab->hover_state.has_value() && compare_tab->hover_state->kind == kind &&
             compare_tab->hover_state->presentation_row == presentation_index &&
             compare_tab->hover_state->collapsed_run_start_model_row ==
                 presentation_row->collapsed_run_start_model_row &&
             compare_tab->hover_state->collapsed_run_length == presentation_row->collapsed_run_length;
    };
    const float summary_x = block_rect.x + 8.0f;
    summary_width = std::max(0.0f, action_rects.text_right_edge - summary_x);
    if (action_rects.previous_rect.has_value()) {
      control_painter.DrawButton(*action_rects.previous_rect, "Show previous 20",
                                 hovered_action(CompareHoverKind::CollapsedContextPreviousAction), selected);
    }
    control_painter.DrawButton(action_rects.all_rect, "Show all",
                               hovered_action(CompareHoverKind::CollapsedContextAllAction), selected);
    if (action_rects.next_rect.has_value()) {
      control_painter.DrawButton(*action_rects.next_rect, "Show next 20",
                                 hovered_action(CompareHoverKind::CollapsedContextNextAction), selected);
    }
    text.DrawString(renderer, summary_x, y, selected ? theme.text_primary : theme.text_secondary,
                    ui_text.TruncateToWidthEphemeralView(summary, summary_width));
  } else {
    text.DrawString(renderer, surface.left_x + surface.gutter_width, y,
                    selected ? theme.text_primary : theme.text_muted,
                    ui_text.TruncateToWidthEphemeralView(summary, summary_width));
  }
}

void PaintCompareHeader(const CompareControlPainter& control_painter, render::TextRenderer& ui_text,
                        const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                        const CompareControlGeometry& control_geometry) {
  SDL_Renderer* const renderer = control_painter.renderer;
  const render::Theme& theme = control_painter.theme;
  render::TextRenderer& text = control_painter.text;
  const CompareTabState* const compare_tab = &control_painter.compare_tab;
  const CompareHeaderButtonRects header = BuildCompareHeaderButtonRects(text, control_geometry, *compare_tab);
  constexpr std::string_view kShortcutHint = "Ctrl+E  keyboard-shortcuts";
  const float summary_x = rect.x + 8.0f;
  const float summary_full = text.MeasureWidth(compare_tab->review_header.summary_line);
  const float hint_width = text.MeasureWidth(kShortcutHint);
  const float hint_x = header.text_right_edge - hint_width;
  const bool show_hint = hint_x - 14.0f >= summary_x + summary_full;
  const float summary_width = std::max(0.0f, (show_hint ? hint_x - 14.0f : header.text_right_edge) - summary_x);
  if (!compare_tab->review_header.summary_line.empty()) {
    text.DrawString(renderer, summary_x, surface.review_summary_y, theme.text_primary,
                    ui_text.TruncateToWidthEphemeralView(compare_tab->review_header.summary_line, summary_width));
  }
  if (show_hint) {
    text.DrawString(renderer, hint_x, surface.review_summary_y, theme.text_muted, kShortcutHint);
  }
  if (header.previous.has_value()) {
    control_painter.DrawButton(*header.previous, kComparePreviousChangeLabel,
                               control_painter.Hovered(CompareHoverKind::HeaderPreviousChange, 0, -1));
  }
  if (header.next.has_value()) {
    control_painter.DrawButton(*header.next, kCompareNextChangeLabel,
                               control_painter.Hovered(CompareHoverKind::HeaderNextChange, 0, -1));
  }
  if (header.file_reviewed.has_value()) {
    const bool file_reviewed = CompareFileFullyReviewed(*compare_tab);
    control_painter.DrawButton(*header.file_reviewed, CompareFileReviewedLabel(file_reviewed),
                               control_painter.Hovered(CompareHoverKind::HeaderToggleFileReviewed, 0, -1),
                               file_reviewed);
  }
}

}

void EnsureCompareOverviewMarkers(const render::Theme& theme,
                                  const SDL_FRect& inner_lane,
                                  std::uint64_t theme_token,
                                  CompareTabState& compare_tab) {
  const std::size_t marker_rows = CompareTabVisualRowCount(compare_tab);
  if (compare_tab.scrollbar_marker_cache_valid &&
      compare_tab.scrollbar_marker_cache_revision == compare_tab.presentation_revision &&
      compare_tab.scrollbar_marker_cache_rows == marker_rows &&
      compare_tab.scrollbar_marker_cache_theme_token == theme_token &&
      RectsEqual(compare_tab.scrollbar_marker_cache_track, inner_lane)) {
    return;
  }

  BuildCompareScrollbarRunsInto(compare_tab.presentation, compare_tab.model,
                                compare_tab.scrollbar_run_scratch);
  const std::vector<CompareScrollbarRun>& runs = compare_tab.scrollbar_run_scratch;
  std::vector<overview::MarkerInput>& inputs = compare_tab.scrollbar_marker_input_scratch;
  inputs.clear();
  inputs.reserve(runs.size());
  for (const CompareScrollbarRun& run : runs) {
    inputs.push_back(overview::MarkerInput{
        .start_row = static_cast<int>(ComparePresentationRowToVisualRow(
            compare_tab, static_cast<std::size_t>(std::max(0, run.start_row)))),
        .end_row = static_cast<int>(ComparePresentationRowToVisualRow(
            compare_tab, static_cast<std::size_t>(std::max(0, run.end_row)))),
        .color = run.layout_only ? theme.text_disabled
                 : run.moved     ? theme.diff_moved
                                 : CompareMarkerColor(theme, run.kind),
        .priority = 0});
  }
  overview::BuildMarkersInto(inner_lane, marker_rows, inputs,
                             compare_tab.scrollbar_marker_cache);
  compare_tab.scrollbar_marker_cache_track = inner_lane;
  compare_tab.scrollbar_marker_cache_revision = compare_tab.presentation_revision;
  compare_tab.scrollbar_marker_cache_rows = marker_rows;
  compare_tab.scrollbar_marker_cache_theme_token = theme_token;
  compare_tab.scrollbar_marker_cache_valid = true;
}

struct WorkspaceShell::CompareRowsPass {
  SDL_Renderer* renderer = nullptr;
  CompareTabState* compare_tab = nullptr;
  const CompareSurfaceLayout* surface = nullptr;
  const editor::DecoratedTextGridRenderer* row_renderer = nullptr;
  bool wrapped = false;
  bool render_whitespace_enabled = false;
  bool draw_compare_caret = false;
  float char_width_px = 0.0f;
  std::span<const editor::PublishedDiagnostic> right_diagnostics;
  const editor::FileDecorations* right_plugin_decorations = nullptr;
  const std::optional<editor::EditorBlameOverlay>* blame_overlay = nullptr;
  std::size_t* blame_index = nullptr;
  std::optional<editor::BracketMatchPair> bracket_match_pair;
  const TextGridInteractionLayout* right_interaction = nullptr;
  std::optional<editor::SelectionRange> right_selection;
  std::span<const editor::TextViewportUndoHistory::SecondaryCaret> right_secondaries;
  bool right_secondaries_span_lines = false;
  const std::vector<editor::IndentGuideRun>* left_guides = nullptr;
  const std::vector<editor::IndentGuideRun>* right_guides = nullptr;
  editor::WhitespaceWalkCursor* left_whitespace_cursor = nullptr;
  editor::WhitespaceWalkCursor* right_whitespace_cursor = nullptr;

  std::span<const editor::TextViewportUndoHistory::SecondaryCaret> RightSecondariesOnLine(std::size_t line) const {
    if (right_secondaries_span_lines) {
      return right_secondaries;
    }
    const auto lower =
        std::lower_bound(right_secondaries.begin(), right_secondaries.end(), line,
                         [](const auto& caret, std::size_t value) { return caret.position.line < value; });
    const auto upper = std::upper_bound(lower, right_secondaries.end(), line, [](std::size_t value, const auto& caret) {
      return value < caret.position.line;
    });
    return right_secondaries.subspan(static_cast<std::size_t>(lower - right_secondaries.begin()),
                                     static_cast<std::size_t>(upper - lower));
  }

  void AppendGuides(std::vector<editor::DecoratedTextFill>& fills, const std::vector<editor::IndentGuideRun>& runs,
                    std::size_t visible_row, std::size_t row_start, std::size_t row_end, float text_x, float y,
                    SDL_Color color) const {
    for (const editor::IndentGuideRun& guide : runs) {
      if (visible_row < guide.start_row || visible_row > guide.end_row) {
        continue;
      }
      if (guide.column < row_start || guide.column >= row_end) {
        continue;
      }
      fills.push_back(editor::DecoratedTextFill{
          .rect = MakeRect(text_x + static_cast<float>(guide.column - row_start) * char_width_px, y - 1.0f, 1.0f,
                           surface->line_height),
          .color = color,
      });
    }
  }
};

struct WorkspaceShell::CompareSidePaint {
  int row = 0;
  float y = 0.0f;
  bool selected = false;
  std::size_t model_index = 0;
  int visual_index = 0;
  std::size_t presentation_index = 0;
  DiffWrapRow wrap_row{};
  const compare::CompareRow* compare_row = nullptr;
  SDL_Color color{};
  SDL_Color row_background{};
  std::size_t row_start = 0;
  std::size_t row_end = 0;
  DiffSideStyle style{};
};

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer,
                                          const SDL_FRect& rect,
                                          CompareTabState& compare_tab_state,
                                          bool draw_compare_caret,
                                          const CompareRenderProjectInputs& project_inputs) {
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
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, compare_tab->scroll_row));
  {
    util::PerformanceTrace::Scope layouts_scope(
        "WorkspaceShell::RenderCompareSurface::VisibleLayouts");
    PrepareCompareVisibleLayoutsForWindow(
        *compare_tab, visible_start_row,
        visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)),
        surface.left_visible_columns, surface.right_visible_columns);
  }
  const TextGridInteractionLayout right_interaction =
      BuildCompareRightInteractionLayout(surface, *compare_tab);
  const std::optional<editor::SelectionRange> right_selection =
      compare_tab->right_view_active ? compare_tab->right_viewport.selection_range() : std::nullopt;
  const std::span<const editor::TextViewportUndoHistory::SecondaryCaret> right_secondaries =
      compare_tab->right_view_active
          ? compare_tab->right_viewport.secondary_caret_range_view()
          : std::span<const editor::TextViewportUndoHistory::SecondaryCaret>{};
  const bool right_secondaries_span_lines =
      std::any_of(right_secondaries.begin(), right_secondaries.end(), [](const auto& caret) {
        return caret.selection_anchor.has_value() &&
               caret.selection_anchor->line != caret.position.line;
      });
  const DiffSurfacePaint& paint = *project_inputs.paint;
  const std::optional<editor::EditorBlameOverlay>& blame_overlay = paint.blame;
  const std::span<const editor::PublishedDiagnostic> right_diagnostics = paint.diagnostics;
  const editor::FileDecorations* const right_plugin_decorations = paint.decorations;
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const float divider_x = surface.center_x + std::min(1.0f, std::max(0.0f, surface.divider_width - 1.0f));
  std::size_t blame_index = 0;

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f), theme_.border);
  const float divider_top = std::max(rect.y, surface.header_y - 1.0f);
  const float divider_height = std::max(0.0f, content_height - (divider_top - rect.y));
  DrawFilledRect(renderer, MakeRect(surface.center_x, divider_top, 1.0f, divider_height), theme_.border);
  DrawFilledRect(renderer, MakeRect(surface.right_x - 1.0f, divider_top, 1.0f, divider_height), theme_.border);

  const CompareControlGeometry control_geometry = MakeCompareControlGeometry(rect, surface);
  const CompareControlPainter control_painter{
      .renderer = renderer, .theme = theme_, .text = editor_text_renderer_, .compare_tab = *compare_tab};
  PaintCompareHeader(control_painter, text_renderer_, rect, surface, control_geometry);
  editor_text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(compare_tab->left_label, surface.left_width - 8.0f));
  editor_text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(compare_tab->right_label, surface.right_width - 8.0f));

  {
  util::PerformanceTrace::Scope side_rows_scope("WorkspaceShell::RenderCompareSurface::SideRows");
  const bool wrapped = compare_tab->wrap_layout.active();
  const float char_width_px = editor_text_renderer_.CharWidth();
  const bool render_whitespace_enabled = paint.render_whitespace;
  const bool indent_guides_enabled = paint.indent_guides;

  const std::optional<editor::BracketMatchPair> bracket_match_pair = CompareBracketMatch(*compare_tab, paint.bracket_match);

  thread_local std::vector<editor::IndentGuideRun> compare_guide_left_runs_;
  thread_local std::vector<editor::IndentGuideRun> compare_guide_right_runs_;
  compare_guide_left_runs_.clear();
  compare_guide_right_runs_.clear();
  if (indent_guides_enabled) {
    ComputeCompareIndentGuides(compare_tab, surface, compare_guide_left_runs_, compare_guide_right_runs_);
  }

  editor::WhitespaceWalkCursor left_whitespace_cursor;
  editor::WhitespaceWalkCursor right_whitespace_cursor;
  const CompareRowsPass rows{
      .renderer = renderer,
      .compare_tab = compare_tab,
      .surface = &surface,
      .row_renderer = &kDecoratedRowRenderer,
      .wrapped = wrapped,
      .render_whitespace_enabled = render_whitespace_enabled,
      .draw_compare_caret = draw_compare_caret,
      .char_width_px = char_width_px,
      .right_diagnostics = right_diagnostics,
      .right_plugin_decorations = right_plugin_decorations,
      .blame_overlay = &blame_overlay,
      .blame_index = &blame_index,
      .bracket_match_pair = bracket_match_pair,
      .right_interaction = &right_interaction,
      .right_selection = right_selection,
      .right_secondaries = right_secondaries,
      .right_secondaries_span_lines = right_secondaries_span_lines,
      .left_guides = &compare_guide_left_runs_,
      .right_guides = &compare_guide_right_runs_,
      .left_whitespace_cursor = &left_whitespace_cursor,
      .right_whitespace_cursor = &right_whitespace_cursor,
  };
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
    const bool selected = presentation_index == compare_tab->selected_row;
    if (presentation_row->kind != compare::ComparePresentationRowKind::Model) {
      PaintCompareSummaryRow(control_painter, text_renderer_, rect, surface, control_geometry, content_width,
                             presentation_row, presentation_index, row, y, selected);
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
      editor_text_renderer_.DrawString(renderer, x, y, color, display_text);
    };
    const SDL_Color neutral_text_color = selected ? theme_.text_primary : theme_.text_secondary;
    SDL_Color left_color = neutral_text_color;
    SDL_Color right_color = neutral_text_color;
    const compare::CompareSideChange left_change = compare::LeftSideChange(compare_row);
    const compare::CompareSideChange right_change = compare::RightSideChange(compare_row);
    const DiffSideStyle left_style =
        StyleDiffSide(theme_, left_change, theme_.diff_deleted, selected);
    const DiffSideStyle right_style =
        StyleDiffSide(theme_, right_change, theme_.diff_added, selected);
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }
    if (compare_row.kind != compare::CompareRowKind::Unchanged &&
        !compare::CompareRowIsLayoutOnly(compare_row)) {
      marker_color = CompareMarkerColor(theme_, compare_row.kind);
    }
    const SDL_Color left_row_background = left_style.background;
    const SDL_Color right_row_background = right_style.background;

    const std::size_t left_row_start = wrapped ? wrap_row.left_start : compare_tab->horizontal_scroll;
    const std::size_t left_row_end =
        wrapped ? wrap_row.left_end : compare_tab->horizontal_scroll + surface.left_visible_columns;
    const std::size_t right_row_start =
        wrapped ? wrap_row.right_start : compare_tab->horizontal_scroll;
    const std::size_t right_row_end = wrapped
                                          ? wrap_row.right_end
                                          : compare_tab->horizontal_scroll +
                                                surface.right_visible_columns;
    PaintCompareLeftSide(rows, CompareSidePaint{.row = row,
                                                .y = y,
                                                .selected = selected,
                                                .model_index = model_index,
                                                .visual_index = visual_index,
                                                .presentation_index = presentation_index,
                                                .wrap_row = wrap_row,
                                                .compare_row = &compare_row,
                                                .color = left_color,
                                                .row_background = left_row_background,
                                                .row_start = left_row_start,
                                                .row_end = left_row_end,
                                                .style = left_style});
    PaintCompareRightSide(rows, CompareSidePaint{.row = row,
                                                 .y = y,
                                                 .selected = selected,
                                                 .model_index = model_index,
                                                 .visual_index = visual_index,
                                                 .presentation_index = presentation_index,
                                                 .wrap_row = wrap_row,
                                                 .compare_row = &compare_row,
                                                 .color = right_color,
                                                 .row_background = right_row_background,
                                                 .row_start = right_row_start,
                                                 .row_end = right_row_end,
                                                 .style = right_style});
    if (wrap_row.first) {
      draw_text(divider_x, surface.divider_width, marker_color, std::string_view(&marker, 1));
    }
    {
      if (const int hunk = CompareHunkControlsOnRow(*compare_tab, wrap_row, row); hunk >= 0) {
        const CompareHunkReviewRects controls =
            BuildCompareHunkReviewRects(editor_text_renderer_, control_geometry, *compare_tab, hunk, row);
        for (const std::optional<SDL_FRect>& leftmost : {controls.badge, controls.toggle, controls.comment}) {
          if (leftmost.has_value()) {
            const float right_end = surface.right_x + surface.gutter_width + surface.right_width;
            DrawFilledRect(renderer, MakeRect(leftmost->x - 4.0f, y - 1.0f, right_end - leftmost->x + 4.0f,
                                              surface.line_height),
                           right_row_background);
            break;
          }
        }
        if (controls.badge.has_value()) {
          const compare::BranchReviewMarkerStatus status = CompareHunkReviewStatus(*compare_tab, hunk);
          const SDL_Color tone =
              status == compare::BranchReviewMarkerStatus::Reviewed ? theme_.diff_added : theme_.diff_modified;
          const SDL_Color fill = render::BlendColors(theme_.editor_background, tone, 0.22f);
          control_painter.DrawLabelBox(*controls.badge, CompareHunkBadgeLabel(status), fill, fill,
                                       theme_.text_secondary);
        }
        if (controls.toggle.has_value()) {
          control_painter.DrawButton(
              *controls.toggle, CompareHunkToggleLabel(CompareHunkReviewStatus(*compare_tab, hunk)),
              control_painter.Hovered(CompareHoverKind::HunkToggleReviewed, presentation_index, hunk));
        }
        if (controls.comment.has_value()) {
          control_painter.DrawButton(*controls.comment, kCompareHunkCommentLabel,
                                     control_painter.Hovered(CompareHoverKind::HunkComment, presentation_index, hunk));
        }
      }
    }
  }
  }
}

void WorkspaceShell::PaintCompareLeftSide(const CompareRowsPass& rows, const CompareSidePaint& side) {
  const SDL_Color left_color = side.color;
  const SDL_Color left_row_background = side.row_background;
  const std::size_t left_row_start = side.row_start;
  const std::size_t left_row_end = side.row_end;
  const DiffSideStyle& left_style = side.style;
  SDL_Renderer* const renderer = rows.renderer;
  CompareTabState* const compare_tab = rows.compare_tab;
  const CompareSurfaceLayout& surface = *rows.surface;
  const editor::DecoratedTextGridRenderer& kDecoratedRowRenderer = *rows.row_renderer;
  const bool wrapped = rows.wrapped;
  const bool render_whitespace_enabled = rows.render_whitespace_enabled;
  const float char_width_px = rows.char_width_px;
  const std::vector<editor::IndentGuideRun>& compare_guide_left_runs_ = *rows.left_guides;
  editor::WhitespaceWalkCursor& left_whitespace_cursor = *rows.left_whitespace_cursor;
  const int row = side.row;
  const float y = side.y;
  const bool selected = side.selected;
  const std::size_t model_index = side.model_index;
  const DiffWrapRow& wrap_row = side.wrap_row;
  const compare::CompareRow& compare_row = *side.compare_row;
  const auto append_guides = [&](std::vector<editor::DecoratedTextFill>& fills,
                                 const std::vector<editor::IndentGuideRun>& runs, std::size_t visible_row,
                                 std::size_t row_start, std::size_t row_end, float text_x, float row_y) {
    rows.AppendGuides(fills, runs, visible_row, row_start, row_end, text_x, row_y, theme_.border);
  };
  const auto draw_text = [&](float x, float width, SDL_Color color, std::string_view text) {
    const std::string_view display_text = TruncateLabelView(text, width);
    if (display_text.empty()) {
      return;
    }
    editor_text_renderer_.DrawString(renderer, x, y, color, display_text);
  };
  if (compare_row.left_line > 0 && wrap_row.left_present) {
    std::array<char, 20> line_number_buf;
    const std::vector<editor::SyntaxTokenKind>* cached_tokens = &CompareRowLeftTokens(*compare_tab, compare_row);
    const std::vector<compare::CompareTextSpan>& left_changed_spans = compare_row.left_changed_spans;
    editor::RowDecorationInput left_input;
    left_input.text_x =
        surface.left_x + surface.gutter_width + static_cast<float>(wrap_row.left_indent) * char_width_px;
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
        .rect = MakeRect(surface.left_x, y - 1.0f, surface.gutter_width + surface.left_width, surface.line_height),
        .color = left_row_background,
    };
    left_input.has_edge_stripe = left_style.stripe;
    if (left_style.stripe) {
      left_input.edge_stripe_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.left_x, y - 1.0f, kDiffEdgeStripeWidth, surface.line_height),
          .color = left_style.stripe_color,
      };
    }
    left_input.changed_spans = std::span<const compare::CompareTextSpan>(left_changed_spans);
    left_input.changed_span_fill = left_style.token_fill;
    left_input.layout =
        CompareVisibleLayoutForRow(*compare_tab, model_index, false, left_row_start,
                                   wrapped ? left_row_end - left_row_start : surface.left_visible_columns);
    compare_whitespace_scratch_.clear();
    if (render_whitespace_enabled) {
      editor::AppendWhitespaceMarkers(compare_whitespace_scratch_, compare_row.left_text,
                                      compare_tab->right_viewport.tab_size(), left_row_start, left_row_end,
                                      left_input.text_x, char_width_px, y, surface.line_height, theme_.text_disabled,
                                      editor::WhitespaceRowResume{&left_whitespace_cursor, model_index});
    }
    append_guides(compare_whitespace_scratch_, compare_guide_left_runs_, static_cast<std::size_t>(row), left_row_start,
                  left_row_end, left_input.text_x, y);
    left_input.prepositioned_fills = std::span<const editor::DecoratedTextFill>(compare_whitespace_scratch_);
    left_input.text_renderer = &editor_text_renderer_;
    left_input.theme = &theme_;
    editor::DecoratedTextRow& left_row = compare_left_scratch_row_;
    editor::BuildDecoratedRow(left_row, left_input);
    kDecoratedRowRenderer.RenderRow(renderer, editor_text_renderer_, left_row);
    if (wrap_row.first) {
      draw_text(surface.left_x + kDiffGutterNumberInset, surface.gutter_width - 4.0f - kDiffGutterNumberInset,
                selected ? theme_.current_line_number : theme_.line_number,
                FormatLineNumber(static_cast<std::size_t>(compare_row.left_line), line_number_buf));
    }
  } else if (wrapped && compare_row.left_line > 0) {
    DrawFilledRect(renderer,
                   MakeRect(surface.left_x, y - 1.0f, surface.gutter_width + surface.left_width, surface.line_height),
                   left_row_background);
  }
}

void WorkspaceShell::PaintCompareRightSide(const CompareRowsPass& rows, const CompareSidePaint& side) {
  const SDL_Color right_color = side.color;
  const SDL_Color right_row_background = side.row_background;
  const std::size_t right_row_start = side.row_start;
  const std::size_t right_row_end = side.row_end;
  const DiffSideStyle& right_style = side.style;
  SDL_Renderer* const renderer = rows.renderer;
  CompareTabState* const compare_tab = rows.compare_tab;
  const CompareSurfaceLayout& surface = *rows.surface;
  const editor::DecoratedTextGridRenderer& kDecoratedRowRenderer = *rows.row_renderer;
  const bool wrapped = rows.wrapped;
  const bool render_whitespace_enabled = rows.render_whitespace_enabled;
  const bool draw_compare_caret = rows.draw_compare_caret;
  const float char_width_px = rows.char_width_px;
  const std::span<const editor::PublishedDiagnostic> right_diagnostics = rows.right_diagnostics;
  const editor::FileDecorations* const right_plugin_decorations = rows.right_plugin_decorations;
  const std::optional<editor::EditorBlameOverlay>& blame_overlay = *rows.blame_overlay;
  std::size_t& blame_index = *rows.blame_index;
  const std::optional<editor::BracketMatchPair>& bracket_match_pair = rows.bracket_match_pair;
  const TextGridInteractionLayout& right_interaction = *rows.right_interaction;
  const std::optional<editor::SelectionRange>& right_selection = rows.right_selection;
  const std::vector<editor::IndentGuideRun>& compare_guide_right_runs_ = *rows.right_guides;
  editor::WhitespaceWalkCursor& right_whitespace_cursor = *rows.right_whitespace_cursor;
  const int row = side.row;
  const float y = side.y;
  const bool selected = side.selected;
  const std::size_t model_index = side.model_index;
  const int visual_index = side.visual_index;
  const std::size_t presentation_index = side.presentation_index;
  const DiffWrapRow& wrap_row = side.wrap_row;
  const compare::CompareRow& compare_row = *side.compare_row;
  const auto right_secondaries_on_line = [&rows](std::size_t line) { return rows.RightSecondariesOnLine(line); };
  const auto append_guides = [&](std::vector<editor::DecoratedTextFill>& fills,
                                 const std::vector<editor::IndentGuideRun>& runs, std::size_t visible_row,
                                 std::size_t row_start, std::size_t row_end, float text_x, float row_y) {
    rows.AppendGuides(fills, runs, visible_row, row_start, row_end, text_x, row_y, theme_.border);
  };
  const auto draw_text = [&](float x, float width, SDL_Color color, std::string_view text) {
    const std::string_view display_text = TruncateLabelView(text, width);
    if (display_text.empty()) {
      return;
    }
    editor_text_renderer_.DrawString(renderer, x, y, color, display_text);
  };
  if (compare_row.right_line > 0 && wrap_row.right_present) {
    std::array<char, 20> line_number_buf;
    const std::size_t right_line_index = static_cast<std::size_t>(compare_row.right_line - 1);
    constexpr std::size_t kMaxCompareVisualMapBytes = 1u << 20;
    const bool right_map_allowed = compare_row.right_text.size() <= kMaxCompareVisualMapBytes &&
                                   !editor::TextLayout::VisualColumnsAreIdentity(compare_row.right_text);
    std::optional<editor::TextLayout::LineVisualColumnMap> right_visual_map;
    auto ensure_right_visual_map = [&]() -> const editor::TextLayout::LineVisualColumnMap* {
      if (!right_map_allowed) {
        return nullptr;
      }
      if (!right_visual_map.has_value()) {
        right_visual_map.emplace(compare_row.right_text, compare_tab->right_viewport.tab_size());
      }
      return &*right_visual_map;
    };
    std::vector<editor::RowFillSpan>& right_column_fills = compare_right_fill_scratch_;
    right_column_fills.clear();
    const auto append_selection = [&](const editor::SelectionRange& sel) {
      if (right_line_index < sel.start.line || right_line_index > sel.end.line) {
        return;
      }
      ensure_right_visual_map();
      right_column_fills.push_back(editor::RowFillSpan{
          .start_column = right_line_index == sel.start.line ? sel.start.column : 0,
          .end_column = right_line_index == sel.end.line ? sel.end.column : compare_row.right_text.size(),
          .color = theme_.selection_fill,
          .geometry = editor::RowFillSpan::Geometry::kRange,
      });
    };
    if (right_selection.has_value()) {
      const editor::SelectionRange sel = *right_selection;
      append_selection(sel);
    }
    const auto row_secondaries = right_secondaries_on_line(right_line_index);
    for (const auto& caret : row_secondaries) {
      if (!caret.selection_anchor.has_value())
        continue;
      editor::SelectionRange sel{*caret.selection_anchor, caret.position};
      if (sel.end.line < sel.start.line || (sel.end.line == sel.start.line && sel.end.column < sel.start.column)) {
        std::swap(sel.start, sel.end);
      }
      append_selection(sel);
    }
    if (bracket_match_pair.has_value()) {
      const auto append_bracket_cell = [&](std::size_t bracket_line, std::size_t bracket_column) {
        if (bracket_line != right_line_index || bracket_column >= compare_row.right_text.size()) {
          return;
        }
        ensure_right_visual_map();
        right_column_fills.push_back(editor::RowFillSpan{
            .start_column = bracket_column,
            .end_column = bracket_column + 1,
            .color = theme_.bracket_match_background,
            .geometry = editor::RowFillSpan::Geometry::kSingleCell,
        });
      };
      append_bracket_cell(bracket_match_pair->open_line, bracket_match_pair->open_column);
      append_bracket_cell(bracket_match_pair->close_line, bracket_match_pair->close_column);
    }
    const std::vector<editor::SyntaxTokenKind>* cached_tokens =
        CompareRowRightTokensAliasLeft(*compare_tab, static_cast<std::size_t>(model_index))
            ? &CompareRowLeftTokens(*compare_tab, compare_row)
            : &CompareRowRightOwnTokens(*compare_tab, compare_row);
    const std::vector<compare::CompareTextSpan>& right_changed_spans = compare_row.right_changed_spans;
    editor::RowDecorationInput right_input;
    right_input.text_x = right_interaction.text_x + static_cast<float>(wrap_row.right_indent) * char_width_px;
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
        .rect = MakeRect(surface.right_x, y - 1.0f, surface.gutter_width + surface.right_width, surface.line_height),
        .color = right_row_background,
    };
    right_input.has_edge_stripe = right_style.stripe;
    if (right_style.stripe) {
      right_input.edge_stripe_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.right_x, y - 1.0f, kDiffEdgeStripeWidth, surface.line_height),
          .color = right_style.stripe_color,
      };
    }
    right_input.column_fills = std::span<const editor::RowFillSpan>(right_column_fills);
    right_input.changed_spans = std::span<const compare::CompareTextSpan>(right_changed_spans);
    right_input.changed_span_fill = right_style.token_fill;
    right_input.layout =
        CompareVisibleLayoutForRow(*compare_tab, model_index, true, right_row_start,
                                   wrapped ? right_row_end - right_row_start : surface.right_visible_columns);
    compare_whitespace_scratch_.clear();
    if (render_whitespace_enabled) {
      editor::AppendWhitespaceMarkers(compare_whitespace_scratch_, compare_row.right_text,
                                      compare_tab->right_viewport.tab_size(), right_row_start, right_row_end,
                                      right_input.text_x, char_width_px, y, surface.line_height, theme_.text_disabled,
                                      editor::WhitespaceRowResume{&right_whitespace_cursor, model_index});
    }
    append_guides(compare_whitespace_scratch_, compare_guide_right_runs_, static_cast<std::size_t>(row),
                  right_row_start, right_row_end, right_input.text_x, y);
    right_input.prepositioned_fills = std::span<const editor::DecoratedTextFill>(compare_whitespace_scratch_);
    if (right_plugin_decorations != nullptr) {
      right_input.text_styles =
          right_plugin_decorations->TextStylesForLine(static_cast<std::uint32_t>(right_line_index));
    }
    if (!right_diagnostics.empty()) {
      right_input.diagnostics = right_diagnostics;
      right_input.diagnostic_line_index = right_line_index;
      right_input.diagnostic_horizontal_scroll = right_row_start;
      right_input.diagnostic_visible_columns =
          wrapped ? right_row_end - right_row_start : surface.right_visible_columns;
      right_input.tab_size = compare_tab->right_viewport.tab_size();
    }
    right_input.text_renderer = &editor_text_renderer_;
    right_input.theme = &theme_;
    editor::DecoratedTextRow& right_row = compare_right_scratch_row_;
    editor::BuildDecoratedRow(right_row, right_input);
    kDecoratedRowRenderer.RenderRow(renderer, editor_text_renderer_, right_row);
    if (!right_diagnostics.empty() && wrap_row.first) {
      if (const auto severity = editor::HighestDiagnosticSeverityForLine(right_diagnostics, right_line_index);
          severity.has_value()) {
        editor::DrawDiagnosticGutterMarker(renderer, theme_, surface.right_x, y, surface.gutter_width,
                                           surface.line_height, *severity);
      }
    }
    if (right_plugin_decorations != nullptr && wrap_row.first) {
      const std::span<const editor::GutterMarkDecoration> marks =
          right_plugin_decorations->GutterMarksForLine(static_cast<std::uint32_t>(right_line_index));
      if (!marks.empty()) {
        editor::GutterIconRegistry::Draw(renderer, marks.front().shape, marks.front().color, surface.right_x, y,
                                         surface.gutter_width, surface.line_height);
      }
    }
    if (wrap_row.first) {
      draw_text(surface.right_x + kDiffGutterNumberInset, surface.gutter_width - 4.0f - kDiffGutterNumberInset,
                selected ? theme_.current_line_number : theme_.line_number,
                FormatLineNumber(static_cast<std::size_t>(compare_row.right_line), line_number_buf));
    }
    const auto draw_caret = [&](std::size_t column) {
      const editor::TextLayout::LineVisualColumnMap* caret_map = ensure_right_visual_map();
      const std::size_t caret_visual =
          caret_map != nullptr ? caret_map->VisualColumnFor(column) : std::min(column, compare_row.right_text.size());
      const bool on_this_row =
          wrapped ? compare_tab->wrap_layout.RowForUnitColumn(presentation_index, caret_visual, true) ==
                        static_cast<std::size_t>(visual_index)
                  : caret_visual >= right_row_start && caret_visual <= right_row_end;
      if (!on_this_row) {
        return;
      }
      const std::size_t caret_cell = wrapped ? wrap_row.right_indent + (caret_visual - right_row_start) : caret_visual;
      DrawFilledRect(renderer,
                     MakeRect(TextGridCursorX(right_interaction, caret_cell), y - 1.0f, 1.5f, surface.line_height),
                     theme_.cursor);
    };
    if (draw_compare_caret && right_line_index == compare_tab->right_viewport.cursor_line()) {
      draw_caret(compare_tab->right_viewport.cursor_column());
    }
    if (draw_compare_caret) {
      for (const auto& caret : row_secondaries) {
        if (caret.position.line == right_line_index) {
          draw_caret(caret.position.column);
        }
      }
    }
    const bool blame_row = !wrapped || compare_tab->wrap_layout.RowForUnitColumn(
                                           presentation_index, std::numeric_limits<std::size_t>::max(),
                                           true) == static_cast<std::size_t>(visual_index);
    if (blame_row && blame_overlay.has_value() && blame_overlay->visible) {
      while (blame_index < blame_overlay->lines.size() &&
             blame_overlay->lines[blame_index].line_index < right_line_index) {
        ++blame_index;
      }
      if (blame_index < blame_overlay->lines.size() &&
          blame_overlay->lines[blame_index].line_index == right_line_index) {
        editor_text_renderer_.DrawStringOn(renderer, blame_overlay->lines[blame_index].rect.x,
                                           blame_overlay->lines[blame_index].rect.y, theme_.text_disabled,
                                           right_row_background, blame_overlay->lines[blame_index].text);
      }
    }
  } else if (wrapped && compare_row.right_line > 0) {
    DrawFilledRect(renderer,
                   MakeRect(surface.right_x, y - 1.0f, surface.gutter_width + surface.right_width, surface.line_height),
                   right_row_background);
  }
}

void WorkspaceShell::RenderCompareScrollbars(SDL_Renderer* renderer,
                                             const SDL_FRect& editor_surface,
                                             CompareTabState& compare_tab_state,
                                             const DiffSurfacePaint& paint) {
  CompareTabState* compare_tab = &compare_tab_state;
  if (renderer == nullptr) {
    return;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(editor_surface, *compare_tab);
  const auto scroll_layout =
      ComputeCompareScrollLayout(editor_surface, surface_layout, *compare_tab);

  if (scroll_layout.vertical_scrollbar.has_value()) {
    const SDL_FRect track = scroll_layout.vertical_scrollbar->track;
    const SDL_FRect lane = overview::LaneRect(track, editor_surface.x);
    const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
    EnsureCompareOverviewMarkers(theme_, inner_lane, overview::ThemeMarkerToken(theme_),
                                 *compare_tab);
    overview::DrawLane(renderer, theme_, lane, compare_tab->scrollbar_marker_cache);
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

}
