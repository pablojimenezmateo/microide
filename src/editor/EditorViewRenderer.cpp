#include "editor/EditorViewRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/BreakpointRender.h"
#include "editor/DiagnosticsRender.h"
#include "editor/ExecutionLineRender.h"
#include "editor/GutterMetrics.h"
#include "editor/WelcomeView.h"
#include "render/SurfacePrimitives.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::editor {

namespace {

const DecoratedTextGridRenderer kDecoratedRowRenderer;

// A caret is a 1px-wide vertical bar spanning the row, drawn in theme.cursor.
// The primary caret and every secondary caret share this exact geometry.
void DrawCaret(SDL_Renderer* renderer, float caret_x, float y, float line_height,
               SDL_Color cursor) {
  const SDL_FRect caret = SDL_FRect{std::round(caret_x), y - 1.0f, 1.0f, line_height};
  SDL_SetRenderDrawColor(renderer, cursor.r, cursor.g, cursor.b, cursor.a);
  SDL_RenderFillRect(renderer, &caret);
}

// Full-width row background highlight (execution line / selected row), inset 1px
// from the viewport edges so it never paints over the border.
SDL_FRect MakeRowBackgroundRect(const SDL_FRect& rect, float y, float line_height) {
  return SDL_FRect{rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, line_height};
}

// True when a visual row is the first (or only) visual row of its logical line,
// i.e. the row that owns the gutter elements (line number, fold/breakpoint/plugin
// marks). With soft-wrap off every row is a head; with it on only visual_start==0.
bool IsLogicalLineHead(bool soft_wrap, std::size_t visual_start) {
  return !soft_wrap || visual_start == 0;
}

// Append the render-whitespace marker for one cell: a 2x2 dot centered in the
// cell for a space, or a thin horizontal bar spanning the tab's cells. Both the
// view-model glyph-run path and the text-iteration fallback build byte-identical
// rects, so they share this one constructor. `cell_span_width` is the tab's full
// pixel width (cell count * char_width); it is unused for spaces.
void PushWhitespaceMarker(std::vector<DecoratedTextFill>& scratch, bool is_tab, float cell_x,
                          float char_width, float cell_span_width, float y, float line_height,
                          SDL_Color color) {
  if (is_tab) {
    scratch.push_back(DecoratedTextFill{
        .rect = SDL_FRect{cell_x + 2.0f, y + line_height * 0.5f, cell_span_width - 4.0f, 1.0f},
        .color = color,
    });
  } else {
    scratch.push_back(DecoratedTextFill{
        .rect = SDL_FRect{cell_x + char_width * 0.5f - 1.0f, y + line_height * 0.5f - 1.0f, 2.0f,
                          2.0f},
        .color = color,
    });
  }
}

float ComputeGutterWidth(const render::TextRenderer& text_renderer, std::size_t line_count,
                         bool show_line_numbers) {
  if (!show_line_numbers) {
    // No digits: reserve only the marker strip (diagnostic bar, breakpoint / execution
    // marker) plus the dedicated fold column, so disabling line
    // numbers actually reclaims the digit column instead of leaving it blank.
    return kGutterLineNumberInset + kGutterFoldColumnWidth;
  }
  char buf[20];
  const auto [end, _] = std::to_chars(buf, buf + sizeof(buf), std::max<std::size_t>(1, line_count));
  // Digits begin after the reserved marker strip (kGutterLineNumberInset), so the
  // gutter must be wide enough for both the markers and the widest line number.
  const float digits_width = text_renderer.MeasureWidth(std::string_view{buf, end});
  return std::max(56.0f,
                  kGutterLineNumberInset + digits_width + kGutterFoldGap +
                      kGutterFoldColumnWidth);
}

// Draws the single fold control: a small square button with a `+` glyph when
// the region is collapsed (click to expand) and a `−` glyph when expanded
// (click to collapse). A filled background plus a 1px border make it read as a
// clickable button, and the 2px-thick glyph keeps it legible at gutter scale.
// This is the only fold-marker draw path; both the scrolled rows and the
// sticky header route through here so the control looks identical everywhere.
void DrawFoldGutterMarker(SDL_Renderer* renderer,
                          SDL_Color glyph,
                          SDL_Color background,
                          SDL_Color border,
                          const SDL_FRect& rect,
                          bool collapsed) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
  SDL_RenderFillRect(renderer, &rect);
  SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
  SDL_RenderRect(renderer, &rect);

  SDL_SetRenderDrawColor(renderer, glyph.r, glyph.g, glyph.b, glyph.a);
  const float thickness = std::max(2.0f, std::floor(rect.h * 0.18f));
  const float mid_y = std::floor(rect.y + (rect.h - thickness) * 0.5f);
  const SDL_FRect horizontal =
      SDL_FRect{rect.x + 2.0f, mid_y, std::max(1.0f, rect.w - 4.0f), thickness};
  SDL_RenderFillRect(renderer, &horizontal);
  if (!collapsed) {
    return;
  }

  const float mid_x = std::floor(rect.x + (rect.w - thickness) * 0.5f);
  const SDL_FRect vertical =
      SDL_FRect{mid_x, rect.y + 2.0f, thickness, std::max(1.0f, rect.h - 4.0f)};
  SDL_RenderFillRect(renderer, &vertical);
}

void DrawPlaceholderView(SDL_Renderer* renderer,
                         const render::TextRenderer& text_renderer,
                         const render::Theme& theme,
                         const SDL_FRect& rect,
                         const WelcomeViewModel& model) {
  const float line_height = text_renderer.LineHeight();
  const WelcomeLayout layout = ComputeWelcomeLayout(rect, model, line_height);
  const SDL_FRect& card = layout.card;

  render::DrawTitledCardFrame(renderer, theme, card, layout.header.h, render::CardStyle::Raised);

  // The welcome surface is a cold (non-hot) path, so it draws with DrawString (no opaque
  // background box) rather than DrawStringOn. Glyphs alpha-blend over whatever backdrop is
  // already painted — card body (surface_raised), header band (chrome_background), and the
  // semi-transparent action-button highlight — so no run shows a mismatched background box.
  const float inset_x = card.x + 20.0f;
  text_renderer.DrawString(renderer, inset_x, card.y + 8.0f, theme.chrome_text,
                           text_renderer.TruncateToWidth(model.title, card.w - 40.0f));
  text_renderer.DrawString(renderer, inset_x, card.y + layout.header.h - line_height - 4.0f,
                           theme.text_secondary,
                           text_renderer.TruncateToWidth(model.subtitle, card.w - 40.0f));

  // A primary-action button: opaque selection fill plus a 3px accent bar on the left edge —
  // the same selection-bar language used by the overlay list — instead of a muddy box.
  const auto draw_button = [&](const SDL_FRect& button, std::string_view label) {
    render::FillRect(renderer, button, theme.selection_strong);
    render::FillRect(renderer, SDL_FRect{button.x, button.y + 2.0f, 3.0f, button.h - 4.0f},
                     theme.accent);
    const float text_y = button.y + std::floor(std::max(0.0f, button.h - line_height) * 0.5f);
    text_renderer.DrawStringOn(renderer, button.x + 12.0f, text_y, theme.surface_text,
                               theme.selection_strong,
                               text_renderer.TruncateToWidth(label, button.w - 20.0f));
  };

  // A recent-entry row: folder/file name (accent) + muted path tail.
  const auto draw_recent_row = [&](const WelcomeHitRegion& region, const WelcomeRecent& recent) {
    const std::string name = text_renderer.TruncateToWidth(recent.name, region.rect.w * 0.5f);
    const float name_w = text_renderer.MeasureWidth(name);
    text_renderer.DrawString(renderer, region.rect.x + 4.0f, region.rect.y + 2.0f, theme.accent,
                             name);
    text_renderer.DrawString(
        renderer, region.rect.x + 14.0f + name_w, region.rect.y + 2.0f, theme.text_muted,
        text_renderer.TruncateToWidth(recent.path_display, region.rect.w - name_w - 26.0f));
  };

  const float caption_x = layout.recents_panel.x;
  const float recents_caption_y = layout.recents_rows_top - line_height - 10.0f;

  if (model.kind == WelcomeKind::ProjectHome) {
    // Left column flows top-down: an "Actions" caption, the three action buttons (keyed off
    // the shared hit regions), a "Recent files" caption, then the recent-file rows.
    text_renderer.DrawString(renderer, caption_x, layout.recents_panel.y, theme.text_muted,
                             model.actions_heading);
    bool drew_file = false;
    for (const WelcomeHitRegion& region : layout.hit_regions) {
      switch (region.kind) {
        case WelcomeHitRegion::Kind::NewFile:
          draw_button(region.rect, model.new_file_label);
          break;
        case WelcomeHitRegion::Kind::OpenFile:
          draw_button(region.rect, model.open_file_label);
          break;
        case WelcomeHitRegion::Kind::FindInProject:
          draw_button(region.rect, model.find_in_project_label);
          break;
        case WelcomeHitRegion::Kind::RecentFile:
          drew_file = true;
          draw_recent_row(region, model.recent_files[region.recent_index]);
          break;
        default:
          break;
      }
    }
    text_renderer.DrawString(renderer, caption_x, recents_caption_y, theme.text_muted,
                             model.recent_files_heading);
    if (!drew_file) {
      text_renderer.DrawString(
          renderer, caption_x + 4.0f, layout.recents_rows_top, theme.text_muted,
          text_renderer.TruncateToWidth(model.empty_recent_files_label,
                                        layout.recents_panel.w - 16.0f));
    }
  } else {
    // NoProject: a "Start" caption, the open-folder button, a "Recent" caption, recent rows.
    text_renderer.DrawString(renderer, caption_x, layout.recents_panel.y, theme.text_muted,
                             model.start_heading);
    draw_button(layout.open_folder_rect, model.open_folder_label);
    text_renderer.DrawString(renderer, caption_x, recents_caption_y, theme.text_muted,
                             model.recents_heading);
    bool drew_recent = false;
    for (const WelcomeHitRegion& region : layout.hit_regions) {
      if (region.kind != WelcomeHitRegion::Kind::RecentProject) {
        continue;
      }
      drew_recent = true;
      draw_recent_row(region, model.recent_projects[region.recent_index]);
    }
    if (!drew_recent) {
      text_renderer.DrawString(
          renderer, caption_x + 4.0f, layout.recents_rows_top, theme.text_muted,
          text_renderer.TruncateToWidth(model.empty_recents_label,
                                        layout.recents_panel.w - 16.0f));
    }
  }

  // Shortcuts panel: curated, registry-sourced key chords (never drifts). Shared by both
  // variants, so the palette is advertised exactly once (here) — no separate footer hint.
  text_renderer.DrawString(renderer, layout.shortcuts_panel.x, layout.shortcuts_panel.y,
                           theme.text_muted, model.shortcuts_heading);
  const float keys_col = std::min(150.0f, layout.shortcuts_panel.w * 0.45f);
  const float sc_row_step = line_height + 6.0f;
  float sc_y = layout.shortcuts_panel.y + line_height + 14.0f;
  for (const WelcomeShortcut& shortcut : model.shortcuts) {
    if (sc_y + line_height > layout.shortcuts_panel.y + layout.shortcuts_panel.h) {
      break;
    }
    text_renderer.DrawString(renderer, layout.shortcuts_panel.x, sc_y, theme.surface_text,
                             text_renderer.TruncateToWidth(shortcut.keys, keys_col - 8.0f));
    text_renderer.DrawString(
        renderer, layout.shortcuts_panel.x + keys_col, sc_y, theme.text_secondary,
        text_renderer.TruncateToWidth(shortcut.label, layout.shortcuts_panel.w - keys_col - 8.0f));
    sc_y += sc_row_step;
  }
}

}  // namespace

SDL_FRect FoldGutterMarkerRect(float gutter_x,
                               float gutter_width,
                               float row_y,
                               float line_height) {
  return SDL_FRect{gutter_x + gutter_width - kGutterFoldRightPad - kGutterFoldMarkerSize,
                   row_y +
                       std::max(1.0f,
                                std::floor((line_height - kGutterFoldMarkerSize) * 0.5f)),
                   kGutterFoldMarkerSize,
                   kGutterFoldMarkerSize};
}

EditorViewMetrics EditorViewRenderer::ComputeMetrics(const render::TextRenderer& text_renderer,
                                                     const TextViewport& viewport,
                                                     const SDL_FRect& rect,
                                                     std::size_t sticky_scroll_rows,
                                                     bool show_line_numbers) {
  EditorViewMetrics metrics;
  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  metrics.gutter_width = ComputeGutterWidth(text_renderer, viewport.line_count(), show_line_numbers);
  metrics.text_x = rect.x + metrics.gutter_width + 12.0f;
  metrics.line_height = text_renderer.LineHeight();
  metrics.sticky_band_top_y = rect.y + 8.0f;
  const int max_total_rows =
      std::max(1, static_cast<int>(std::floor((rect.h - 12.0f) / metrics.line_height)));
  const std::size_t sticky_budget = static_cast<std::size_t>(std::max(0, max_total_rows - 1));
  const std::size_t sticky_clamped = std::min(sticky_scroll_rows, sticky_budget);
  metrics.sticky_scroll_rows = sticky_clamped;
  metrics.first_line_y =
      metrics.sticky_band_top_y + static_cast<float>(sticky_clamped) * metrics.line_height;
  metrics.visible_rows =
      static_cast<std::size_t>(std::max(1, max_total_rows - static_cast<int>(sticky_clamped)));
  // Right reserve = 12px inset past the gutter (mirrors text_x) + a 21px right gutter that
  // covers the vertical scrollbar (kScrollbarThickness 10 + inset 2) AND the overview-ruler
  // lane painted just left of it (workspace/OverviewRuler.h: kLaneGap 3 + kLaneWidth 6), so
  // a long line's trailing glyphs never lay out under the lane. Kept unconditional (not
  // gated on the overview-ruler setting) so the text width is identical across the render
  // path and every hit-test/scroll ComputeMetrics caller — a gated reserve would desync
  // click-to-caret mapping near the right edge from what was drawn.
  constexpr float kTextRightReserve = 33.0f;
  metrics.visible_columns = static_cast<std::size_t>(
      std::max(8.0f, (rect.w - metrics.gutter_width - kTextRightReserve) / char_width));
  return metrics;
}

void EditorViewRenderer::Render(SDL_Renderer* renderer,
                                const render::TextRenderer& text_renderer,
                                const render::Theme& theme,
                                TextViewport& viewport,
                                const SDL_FRect& rect,
                                bool draw_caret,
                                std::string_view search_query,
                                const std::optional<SelectionRange>& active_search_match,
                                const std::optional<EditorBlameOverlay>& blame_overlay,
                                std::span<const PublishedDiagnostic> diagnostics,
                                const EditorViewModel* view_model,
                                bool bracket_match_highlight_enabled,
                                bool indent_guides_enabled,
                                bool render_whitespace_enabled,
                                const FoldingModel* folding_model,
                                const WelcomeViewModel* welcome_view,
                                const FileDecorations* plugin_decorations,
                                bool show_line_numbers) const {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("EditorViewRenderer::Render");
  SDL_SetRenderDrawColor(renderer, theme.editor_background.r, theme.editor_background.g,
                         theme.editor_background.b, theme.editor_background.a);
  SDL_RenderFillRect(renderer, &rect);

  if (viewport.is_placeholder()) {
    if (welcome_view != nullptr) {
      DrawPlaceholderView(renderer, text_renderer, theme, rect, *welcome_view);
    }
    viewport.SetViewportSize(1, 1);
    return;
  }

  std::optional<BracketMatchPair> bracket_match_pair;
  if (bracket_match_highlight_enabled) {
    const std::size_t caret_line = viewport.cursor_line();
    const std::size_t caret_column = viewport.cursor_column();
    const std::uint64_t content_revision = viewport.content_revision();
    auto cache_it = std::find_if(
        bracket_match_cache_entries_.begin(), bracket_match_cache_entries_.end(),
        [&](const BracketMatchCacheEntry& entry) {
          return entry.viewport == &viewport && entry.content_revision == content_revision &&
                 entry.caret_line == caret_line && entry.caret_column == caret_column;
        });
    if (cache_it != bracket_match_cache_entries_.end()) {
      bracket_match_pair = cache_it->pair;
      last_bracket_match_pair_ = bracket_match_pair;
      ++bracket_match_cache_hits_;
      if (cache_it != bracket_match_cache_entries_.begin()) {
        std::iter_swap(bracket_match_cache_entries_.begin(), cache_it);
      }
    } else {
      bracket_match_pair = FindBracketMatch(viewport, caret_line, caret_column);
      last_bracket_match_pair_ = bracket_match_pair;
      auto same_viewport_it = std::find_if(
          bracket_match_cache_entries_.begin(), bracket_match_cache_entries_.end(),
          [&](const BracketMatchCacheEntry& entry) { return entry.viewport == &viewport; });
      BracketMatchCacheEntry updated_entry{
          .viewport = &viewport,
          .content_revision = content_revision,
          .caret_line = caret_line,
          .caret_column = caret_column,
          .pair = bracket_match_pair,
      };
      if (same_viewport_it != bracket_match_cache_entries_.end()) {
        *same_viewport_it = std::move(updated_entry);
        if (same_viewport_it != bracket_match_cache_entries_.begin()) {
          std::iter_swap(bracket_match_cache_entries_.begin(), same_viewport_it);
        }
      } else if (bracket_match_cache_entries_.size() < kBracketMatchCacheLimit) {
        bracket_match_cache_entries_.push_back(std::move(updated_entry));
        if (bracket_match_cache_entries_.size() > 1) {
          std::iter_swap(bracket_match_cache_entries_.begin(),
                         bracket_match_cache_entries_.end() - 1);
        }
      } else {
        bracket_match_cache_entries_.back() = std::move(updated_entry);
        std::iter_swap(bracket_match_cache_entries_.begin(),
                       bracket_match_cache_entries_.end() - 1);
      }
      ++bracket_match_cache_misses_;
    }
  } else {
    last_bracket_match_pair_.reset();
  }

  const std::size_t sticky_row_count =
      view_model != nullptr ? view_model->sticky_lines.size() : 0;
  const EditorViewMetrics metrics =
      ComputeMetrics(text_renderer, viewport, rect, sticky_row_count, show_line_numbers);
  const SDL_FRect gutter = SDL_FRect{rect.x, rect.y, metrics.gutter_width, rect.h};
  SDL_SetRenderDrawColor(renderer, theme.gutter_background.r, theme.gutter_background.g,
                         theme.gutter_background.b, theme.gutter_background.a);
  SDL_RenderFillRect(renderer, &gutter);
  const SDL_FRect gutter_divider =
      SDL_FRect{gutter.x + gutter.w - 1.0f, gutter.y, 1.0f, gutter.h};
  SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
  SDL_RenderFillRect(renderer, &gutter_divider);

  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const auto& lines = viewport.lines();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t cursor_line = viewport.cursor_line();
  const bool soft_wrap = viewport.soft_wrap();
  // 2026-05-15 perf deep-dive round 2 Finding 10: span accessor avoids the per-frame vector
  // allocation that secondary_carets() performed when the caret set is stable.
  const std::span<const TextPosition> secondary_carets = viewport.secondary_caret_positions();
  const auto selection = viewport.selection_range();
  char line_number_buf[20];
  // Skip the ToLower allocation entirely on the common no-search frame; reuse
  // the scratch buffer when a query is active so the std::string capacity
  // persists across frames.
  if (search_query.empty()) {
    lowered_search_query_scratch_.clear();
  } else {
    util::ToLowerAsciiInto(search_query, lowered_search_query_scratch_);
  }
  const std::string& lowered_search_query = lowered_search_query_scratch_;
  std::size_t blame_index = 0;
  std::size_t secondary_caret_index = 0;
  std::string& lowered_line_scratch = lowered_line_scratch_;
  last_fold_gutter_marks_.clear();
  if (view_model != nullptr) {
    last_fold_gutter_marks_.insert(last_fold_gutter_marks_.end(),
                                   view_model->fold_gutter_marks.begin(),
                                   view_model->fold_gutter_marks.end());
  }
  const std::vector<FoldGutterMark>* fold_gutter_marks =
      view_model != nullptr ? &view_model->fold_gutter_marks : nullptr;
  std::size_t fold_gutter_mark_index = 0;
  const std::vector<BreakpointGutterMark>* breakpoint_gutter_marks =
      view_model != nullptr ? &view_model->breakpoint_gutter_marks : nullptr;
  std::size_t breakpoint_gutter_mark_index = 0;

  // Build the visible-row→buffer-line map once so the indent-guides compute
  // and the per-row paint loop can both consume it. The visible-rows count is
  // also part of the indent-guides cache key. The scratch member preserves
  // capacity across frames so this loop does not reallocate every render.
  std::vector<std::size_t>& visible_rows_for_guides = visible_rows_for_guides_scratch_;
  visible_rows_for_guides.clear();
  if (indent_guides_enabled) {
    visible_rows_for_guides.reserve(metrics.visible_rows);
    for (std::size_t row = 0; row < metrics.visible_rows; ++row) {
      const std::size_t visual_row_index = scroll_line + row;
      if (visual_row_index >= viewport.visual_line_count()) break;
      const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
      visible_rows_for_guides.push_back(row_meta.line_index);
    }
  }

  const std::vector<IndentGuideRun>* indent_guides_to_paint = nullptr;
  if (indent_guides_enabled) {
    const std::size_t indent_width =
        viewport.indent_width() == 0 ? 1 : viewport.indent_width();
    const std::size_t fold_emphasis_revision =
        folding_model != nullptr ? folding_model->revision() : std::size_t{0};
    auto cache_it = std::find_if(
        indent_guides_cache_entries_.begin(), indent_guides_cache_entries_.end(),
        [&](const IndentGuidesCacheEntry& entry) {
          return entry.viewport == &viewport &&
                 entry.content_revision == viewport.content_revision() &&
                 entry.layout_shape_revision == viewport.layout_shape_revision() &&
                 entry.fold_revision == viewport.folding_revision() &&
                 entry.fold_emphasis_revision == fold_emphasis_revision &&
                 entry.scroll_line == scroll_line &&
                 entry.visible_rows_count == visible_rows_for_guides.size() &&
                 entry.indent_width == indent_width && entry.caret_line == cursor_line;
        });
    if (cache_it != indent_guides_cache_entries_.end()) {
      indent_guides_to_paint = &cache_it->runs;
      indent_guides_cache_last_runs_ = indent_guides_to_paint;
      ++indent_guides_cache_hits_;
      if (cache_it != indent_guides_cache_entries_.begin()) {
        std::iter_swap(indent_guides_cache_entries_.begin(), cache_it);
        indent_guides_to_paint = &indent_guides_cache_entries_.front().runs;
        indent_guides_cache_last_runs_ = indent_guides_to_paint;
      }
    } else {
      const std::size_t caret_indent =
          cursor_line < lines.size()
              ? LeadingVisualIndent(lines[cursor_line], viewport.tab_size())
              : 0;
      auto same_viewport_it = std::find_if(
          indent_guides_cache_entries_.begin(), indent_guides_cache_entries_.end(),
          [&](const IndentGuidesCacheEntry& entry) { return entry.viewport == &viewport; });
      IndentGuidesCacheEntry* target_entry = nullptr;
      if (same_viewport_it != indent_guides_cache_entries_.end()) {
        target_entry = &*same_viewport_it;
      } else if (indent_guides_cache_entries_.size() < kIndentGuidesCacheLimit) {
        indent_guides_cache_entries_.push_back(IndentGuidesCacheEntry{});
        target_entry = &indent_guides_cache_entries_.back();
      } else {
        target_entry = &indent_guides_cache_entries_.back();
      }
      target_entry->viewport = &viewport;
      target_entry->content_revision = viewport.content_revision();
      target_entry->layout_shape_revision = viewport.layout_shape_revision();
      target_entry->fold_revision = viewport.folding_revision();
      target_entry->fold_emphasis_revision = fold_emphasis_revision;
      target_entry->scroll_line = scroll_line;
      target_entry->visible_rows_count = visible_rows_for_guides.size();
      target_entry->indent_width = indent_width;
      target_entry->caret_line = cursor_line;
      ComputeIndentGuides(lines, visible_rows_for_guides, viewport.tab_size(),
                          indent_width, cursor_line, caret_indent, &target_entry->runs,
                          folding_model);
      indent_guides_to_paint = &target_entry->runs;
      indent_guides_cache_last_runs_ = indent_guides_to_paint;
      if (target_entry != &indent_guides_cache_entries_.front()) {
        std::iter_swap(indent_guides_cache_entries_.begin(),
                       indent_guides_cache_entries_.begin() +
                           static_cast<std::ptrdiff_t>(target_entry -
                                                       indent_guides_cache_entries_.data()));
        indent_guides_to_paint = &indent_guides_cache_entries_.front().runs;
        indent_guides_cache_last_runs_ = indent_guides_to_paint;
      }
      ++indent_guides_cache_misses_;
    }
  } else {
    indent_guides_cache_last_runs_ = nullptr;
  }

  if (metrics.sticky_scroll_rows > 0 && view_model != nullptr &&
      !view_model->sticky_lines.empty()) {
    for (std::size_t si = 0; si < view_model->sticky_lines.size(); ++si) {
      const std::size_t line_index = view_model->sticky_lines[si];
      if (line_index >= lines.size()) {
        continue;
      }
      const float y =
          metrics.sticky_band_top_y + static_cast<float>(si) * metrics.line_height;
      const bool selected = line_index == cursor_line;
      const SDL_Color row_background = selected ? theme.row_highlight : theme.editor_background;
      const std::size_t opener_visual = viewport.VisualRowForLine(line_index);
      if (opener_visual >= viewport.visual_line_count()) {
        continue;
      }
      const auto row_meta = viewport.WrappedVisualRowLayout(opener_visual);
      const auto row_layout = soft_wrap ? viewport.VisibleWrappedRowLayout(opener_visual)
                                        : viewport.VisibleLineLayout(line_index);

      DecoratedTextRow& sticky_row = sticky_scratch_row_;
      const std::vector<SyntaxTokenKind>* sticky_tokens = nullptr;
      {
        util::PerformanceTrace::Scope token_scope("EditorViewRenderer::Render::StickyHighlightedLineTokens");
        sticky_tokens = &viewport.HighlightedLineTokens(line_index);
      }
      RowDecorationInput sticky_input;
      sticky_input.text_x = metrics.text_x;
      sticky_input.y = y;
      sticky_input.char_width = text_renderer.CharWidth();
      sticky_input.line_height = metrics.line_height;
      sticky_input.row_visual_start = row_meta.visual_start;
      sticky_input.row_visual_end = row_meta.visual_end;
      sticky_input.text = &lines[line_index];
      sticky_input.tokens = sticky_tokens;
      sticky_input.plain_color = selected ? theme.text_primary : theme.text_secondary;
      sticky_input.layout = &row_layout;
      sticky_input.has_background_fill = true;
      sticky_input.background_fill = DecoratedTextFill{
          .rect = MakeRowBackgroundRect(rect, y, metrics.line_height),
          .color = row_background,
      };
      sticky_input.diagnostics = diagnostics;
      sticky_input.diagnostic_line_index = line_index;
      sticky_input.diagnostic_horizontal_scroll = row_meta.visual_start;
      sticky_input.diagnostic_visible_columns = row_meta.visual_end - row_meta.visual_start;
      sticky_input.tab_size = viewport.tab_size();
      if (plugin_decorations != nullptr) {
        sticky_input.text_styles =
            plugin_decorations->TextStylesForLine(static_cast<std::uint32_t>(line_index));
      }
      sticky_input.text_renderer = &text_renderer;
      sticky_input.theme = &theme;
      BuildDecoratedRow(sticky_row, sticky_input);
      {
        util::PerformanceTrace::Scope row_render_scope("EditorViewRenderer::Render::StickyDecoratedRow");
        kDecoratedRowRenderer.RenderRow(renderer, text_renderer, sticky_row);
      }
      if (const auto severity = HighestDiagnosticSeverityForLine(diagnostics, line_index);
          severity.has_value()) {
        DrawDiagnosticGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height,
                                   *severity);
      }
      if (show_line_numbers && IsLogicalLineHead(soft_wrap, row_meta.visual_start)) {
        const auto [end_sticky, _] =
            std::to_chars(line_number_buf, line_number_buf + sizeof(line_number_buf),
                          line_index + 1);
        text_renderer.DrawStringOn(
            renderer, gutter.x + kGutterLineNumberInset, y,
            selected ? theme.current_line_number : theme.line_number,
            selected ? theme.row_highlight : theme.gutter_background,
            std::string_view{line_number_buf, end_sticky});
      }
      if (folding_model != nullptr && IsLogicalLineHead(soft_wrap, row_meta.visual_start) &&
          folding_model->FoldStartingAt(line_index).has_value()) {
        DrawFoldGutterMarker(renderer,
                             selected ? theme.current_line_number : theme.line_number,
                             theme.row_highlight, theme.border,
                             FoldGutterMarkerRect(gutter.x, gutter.w, y, metrics.line_height),
                             folding_model->IsCollapsedAtOpener(line_index));
      }
    }
    SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    const SDL_FRect sticky_divider =
        SDL_FRect{rect.x, metrics.first_line_y - 1.0f, rect.w, 1.0f};
    SDL_RenderFillRect(renderer, &sticky_divider);
  }

  util::PerformanceTrace::Scope rows_scope("EditorViewRenderer::Render::Rows");
  // The single row -> y mapping. With no inline insets (the common case) row_gaps
  // is empty and RowTop(row) == first_line_y + row*line_height exactly.
  const EditorRowYLayout row_y_layout(
      metrics.first_line_y, metrics.line_height, static_cast<std::uint32_t>(scroll_line),
      view_model != nullptr ? view_model->row_gaps : std::span<const RowGap>{});
  // Only collect gutter numbers for a batched flush when the backend actually
  // batches (GPU atlas); otherwise draw them inline to avoid any collection cost
  // on the software/debug path (keeps it byte-for-byte the prior behaviour).
  const bool batch_gutter_numbers = text_renderer.BatchesRuns();
  gutter_number_scratch_.clear();
  // Owned layout only for the soft-wrap branch (which builds a per-row slice the
  // cache cannot serve by reference); reused across rows so the wrap path does
  // not re-allocate its string/vectors each iteration.
  LayoutLine wrapped_layout_scratch;
  // Resolve the caret's visual row once per frame; the soft-wrap layout only
  // needs it to flag the caret row, and recomputing it per visible row is an
  // O(visible_rows * caret_column) redundancy.
  const std::size_t caret_visual_row = soft_wrap ? viewport.cursor_visual_row() : 0;
  // CharWidth() is fixed for the whole frame (font size does not change mid-render),
  // so resolve the backend's advance once instead of per row / per glyph-cell.
  const float char_width_px = text_renderer.CharWidth();
  for (std::size_t row = 0; row < metrics.visible_rows; ++row) {
    const std::size_t visual_row_index = scroll_line + row;
    if (visual_row_index >= viewport.visual_line_count()) {
      break;
    }
    const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
    const std::size_t line_index = row_meta.line_index;
    // Bind the row layout by reference: the soft-wrap branch fills a reusable
    // owned scratch, the common branch hands back the cache entry in place. This
    // avoids copying the LayoutLine (string + 2 vectors) per visible row.
    const LayoutLine& row_layout =
        soft_wrap ? (wrapped_layout_scratch =
                         viewport.VisibleWrappedRowLayout(visual_row_index, caret_visual_row))
                  : viewport.VisibleLineLayoutRef(line_index);
    // Caret is per-call (not baked into the cached layout): the wrap branch
    // already resolved it onto the scratch; the common branch resolves it here.
    const TextViewport::LineCaret row_caret =
        soft_wrap ? TextViewport::LineCaret{row_layout.caret_visible, row_layout.caret_column}
                  : viewport.CaretForLine(line_index);
    if (line_index >= lines.size()) {
      break;
    }

    const float y = row_y_layout.RowTop(row);
    const std::size_t row_visual_origin =
        soft_wrap ? row_meta.visual_start : viewport.horizontal_scroll();
    // Hanging indent: continuation rows shift their whole content (text, carets,
    // highlights, guides, whitespace markers) right by `indent` cells. Zero for
    // first rows and non-wrapped rows, so this is a no-op there.
    const float row_text_x =
        metrics.text_x + static_cast<float>(row_meta.indent) * char_width_px;
    // Mid-line inlay hints for this row. Suppressed on soft-wrapped lines in v1
    // (cross-wrap-row displacement is out of scope); those lines still render,
    // just without their hints. Built once here and reused by the whitespace,
    // caret, decorated-row and end-of-line passes so they all stay grid-aligned
    // with the shifted glyphs. Empty (the common case) => identity, ~zero cost.
    std::span<const InlineTextDecoration> row_inline_texts;
    InlayRowDisplacement row_inlay;
    std::size_t row_inlay_total_cells = 0;
    if (plugin_decorations != nullptr && !soft_wrap) {
      row_inline_texts =
          plugin_decorations->InlineTextsForLine(static_cast<std::uint32_t>(line_index));
      BuildInlayRowSpans(row_inline_texts, &row_layout, nullptr, row_visual_origin,
                         row_meta.visual_end, text_renderer, char_width_px, inlay_span_scratch_,
                         &row_inlay_total_cells);
      row_inlay = InlayRowDisplacement(inlay_span_scratch_);
    }
    const auto inlay_shift_px = [&](std::size_t absolute_visual) -> float {
      if (row_inlay.empty() || absolute_visual < row_visual_origin) {
        return 0.0f;
      }
      return static_cast<float>(
                 row_inlay.CellsInsertedBefore(absolute_visual - row_visual_origin)) *
             char_width_px;
    };
    const bool selected = line_index == cursor_line;
    const bool is_execution_line = view_model != nullptr &&
                                   view_model->execution_line_index.has_value() &&
                                   *view_model->execution_line_index == line_index;
    const bool active_search_line =
        active_search_match.has_value() &&
        line_index >= active_search_match->start.line &&
        line_index <= active_search_match->end.line;
    const SDL_Color row_background = selected ? theme.row_highlight : theme.editor_background;
    // Marshal this row's source-column fills and pre-positioned fills, then hand
    // them to the unified BuildDecoratedRow. The builder resolves source->visual
    // columns and owns the assembly order (background -> column fills ->
    // pre-positioned -> syntax -> diagnostics).
    column_fill_scratch_.clear();
    prepositioned_fill_scratch_.clear();

    if (!lowered_search_query.empty()) {
      // Probe with a borrowed view so a cache hit allocates nothing (the query
      // string is identical for every row this frame).
      const SearchMatchCacheKeyView cache_key_view{
          .viewport = &viewport,
          .content_revision = viewport.content_revision(),
          .line_index = line_index,
          .query = lowered_search_query,
      };
      auto cache_it = search_match_cache_.find(cache_key_view);
      if (cache_it == search_match_cache_.end()) {
        const std::string& src = lines[line_index];
        util::ToLowerAsciiInto(src, lowered_line_scratch);
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        std::size_t match_offset = lowered_line_scratch.find(lowered_search_query);
        while (match_offset != std::string::npos) {
          matches.emplace_back(match_offset, match_offset + lowered_search_query.size());
          match_offset = lowered_line_scratch.find(lowered_search_query, match_offset + 1);
        }
        if (search_match_cache_.size() >= kSearchMatchCacheLimit) {
          search_match_cache_.erase(search_match_cache_order_.front());
          search_match_cache_order_.pop_front();
        }
        // Materialize the owning key only now, on insert.
        SearchMatchCacheKey owning_key{
            .viewport = &viewport,
            .content_revision = viewport.content_revision(),
            .line_index = line_index,
            .query = std::string(lowered_search_query),
        };
        search_match_cache_order_.push_back(owning_key);
        auto [inserted_it, _] = search_match_cache_.emplace(std::move(owning_key), std::move(matches));
        cache_it = inserted_it;
      }

      // Source-column spans; the builder resolves them against `row_layout`.
      for (const auto& [match_start, match_end] : cache_it->second) {
        const bool is_active_match =
            active_search_line &&
            match_start == active_search_match->start.column &&
            line_index == active_search_match->start.line;
        column_fill_scratch_.push_back(RowFillSpan{
            .start_column = match_start,
            .end_column = match_end,
            .color = is_active_match ? theme.search_match_active : theme.search_match,
            .geometry = RowFillSpan::Geometry::kRange,
        });
      }
    }

    if (view_model != nullptr && !view_model->occurrence_ranges.empty()) {
      // Round-2 Finding 2: occurrence_ranges is sorted by line_index (see
      // RefillOccurrenceScanCache). Binary-search for this row's slice instead of scanning the
      // full vector per visible row.
      const auto occ_data = view_model->occurrence_ranges.data();
      const auto occ_end_ptr = occ_data + view_model->occurrence_ranges.size();
      const auto occ_lo = std::lower_bound(
          occ_data, occ_end_ptr, line_index,
          [](const OccurrenceRange& r, std::size_t l) { return r.line_index < l; });
      const auto occ_hi = std::upper_bound(
          occ_lo, occ_end_ptr, line_index,
          [](std::size_t l, const OccurrenceRange& r) { return l < r.line_index; });
      for (auto occ_it = occ_lo; occ_it < occ_hi; ++occ_it) {
        const OccurrenceRange& occ = *occ_it;
        if (occ.start_column >= occ.end_column) {
          continue;
        }
        column_fill_scratch_.push_back(RowFillSpan{
            .start_column = occ.start_column,
            .end_column = occ.end_column,
            .color = occ.is_primary_seed ? theme.search_match_active : theme.search_match,
            .geometry = RowFillSpan::Geometry::kRange,
        });
      }
    }

    if (selection.has_value() &&
        line_index >= selection->start.line &&
        line_index <= selection->end.line) {
      const std::size_t line_start =
          line_index == selection->start.line ? selection->start.column : 0;
      const std::size_t line_end =
          line_index == selection->end.line ? selection->end.column : lines[line_index].size();
      column_fill_scratch_.push_back(RowFillSpan{
          .start_column = line_start,
          .end_column = line_end,
          .color = theme.selection_fill,
          .geometry = RowFillSpan::Geometry::kRange,
      });
    }

    if (bracket_match_pair.has_value()) {
      const auto append_bracket_cell = [&](std::size_t bracket_line, std::size_t bracket_column) {
        if (bracket_line != line_index) return;
        if (bracket_column >= lines[line_index].size()) return;
        column_fill_scratch_.push_back(RowFillSpan{
            .start_column = bracket_column,
            .end_column = bracket_column + 1,
            .color = theme.bracket_match_background,
            .geometry = RowFillSpan::Geometry::kSingleCell,
        });
      };
      append_bracket_cell(bracket_match_pair->open_line, bracket_match_pair->open_column);
      append_bracket_cell(bracket_match_pair->close_line, bracket_match_pair->close_column);
    }

    if (indent_guides_to_paint != nullptr) {
      const std::size_t row_start_visual = row_meta.visual_start;
      const std::size_t row_end_visual = row_meta.visual_end;
      // Guides are now stored as vertical runs `[start_row, end_row]`; honor
      // both ends so a multi-row guide draws on every row it covers.
      for (const auto& guide : *indent_guides_to_paint) {
        if (row < guide.start_row || row > guide.end_row) continue;
        if (guide.column < row_start_visual || guide.column >= row_end_visual) continue;
        const SDL_Color color = guide.active ? theme.text_muted : theme.border;
        prepositioned_fill_scratch_.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    row_text_x +
                        static_cast<float>(guide.column - row_start_visual) * char_width_px,
                    y - 1.0f,
                    1.0f,
                    metrics.line_height,
                },
            .color = color,
        });
      }
    }

    if (render_whitespace_enabled) {
      const std::size_t row_start_visual = row_meta.visual_start;
      const std::size_t row_end_visual = row_meta.visual_end;
      const float char_width = char_width_px;
      // Round-2 Finding 2: use the CSR-style row-offset table so we iterate only this row's runs
      // instead of scanning the flat whitespace_glyph_runs vector.
      const bool use_vm_whitespace =
          view_model != nullptr && !view_model->whitespace_glyph_runs.empty() &&
          row + 1 < view_model->whitespace_row_offsets.size();
      if (use_vm_whitespace) {
        const std::size_t run_begin = view_model->whitespace_row_offsets[row];
        const std::size_t run_end = view_model->whitespace_row_offsets[row + 1];
        for (std::size_t gi = run_begin; gi < run_end; ++gi) {
          const WhitespaceGlyphRun& glyph = view_model->whitespace_glyph_runs[gi];
          // The CSR row-offset table (whitespace_row_offsets) already partitions
          // runs strictly by row, so every glyph in [run_begin, run_end) belongs
          // to this row by construction; no per-glyph row-bounds filter needed.
          const float cell_x =
              row_text_x + static_cast<float>(glyph.cell_visual_start - row_start_visual) *
                               char_width +
              inlay_shift_px(glyph.cell_visual_start);
          const float cell_w = static_cast<float>(glyph.cell_visual_extent) * char_width;
          PushWhitespaceMarker(prepositioned_fill_scratch_, glyph.is_tab_rule, cell_x, char_width,
                               cell_w, y, metrics.line_height, theme.text_disabled);
        }
      } else {
        const std::string& line_text = lines[line_index];
        const std::size_t tab_size = viewport.tab_size();
        std::size_t visual_col = 0;
        for (char c : line_text) {
          std::size_t cell_width = 1;
          if (c == '\t') {
            const std::size_t step = tab_size == 0 ? 1 : tab_size;
            cell_width = step - (visual_col % step);
          }
          const std::size_t cell_start = visual_col;
          visual_col += cell_width;
          if (cell_start >= row_end_visual) break;
          if (cell_start < row_start_visual) continue;
          if (visual_col > row_end_visual) continue;
          const float cell_x =
              row_text_x +
              static_cast<float>(cell_start - row_start_visual) * char_width +
              inlay_shift_px(cell_start);
          if (c == ' ' || c == '\t') {
            const float cell_w = static_cast<float>(cell_width) * char_width;
            PushWhitespaceMarker(prepositioned_fill_scratch_, c == '\t', cell_x, char_width,
                                 cell_w, y, metrics.line_height, theme.text_disabled);
          }
        }
      }
    }

    const std::vector<SyntaxTokenKind>* token_kinds = nullptr;
    {
      util::PerformanceTrace::Scope token_scope("EditorViewRenderer::Render::HighlightedLineTokens");
      token_kinds = &viewport.HighlightedLineTokens(line_index);
    }
    DecoratedTextRow& row_desc = scratch_row_;
    RowDecorationInput row_input;
    row_input.text_x = row_text_x;
    row_input.y = y;
    row_input.char_width = char_width_px;
    row_input.line_height = metrics.line_height;
    row_input.row_visual_start = row_visual_origin;
    row_input.row_visual_end = row_meta.visual_end;
    row_input.text = &lines[line_index];
    row_input.tokens = token_kinds;
    row_input.plain_color = selected ? theme.text_primary : theme.text_secondary;
    row_input.layout = &row_layout;
    // The execution line outranks the selected-row highlight so a paused frame
    // stays visible even while it is the caret line.
    row_input.has_background_fill = selected || is_execution_line;
    if (is_execution_line) {
      row_input.background_fill = DecoratedTextFill{
          .rect = MakeRowBackgroundRect(rect, y, metrics.line_height),
          .color = theme.debug_execution_line,
      };
    } else if (selected) {
      row_input.background_fill = DecoratedTextFill{
          .rect = MakeRowBackgroundRect(rect, y, metrics.line_height),
          .color = theme.row_highlight,
      };
    }
    row_input.column_fills = std::span<const RowFillSpan>(column_fill_scratch_);
    row_input.prepositioned_fills = std::span<const DecoratedTextFill>(prepositioned_fill_scratch_);
    if (plugin_decorations != nullptr) {
      row_input.text_styles =
          plugin_decorations->TextStylesForLine(static_cast<std::uint32_t>(line_index));
    }
    row_input.inlay = row_inlay;
    row_input.inlay_inline_texts = row_inline_texts;
    row_input.inlay_color = theme.text_disabled;
    row_input.diagnostics = diagnostics;
    row_input.diagnostic_line_index = line_index;
    row_input.diagnostic_horizontal_scroll = row_meta.visual_start;
    row_input.diagnostic_visible_columns = row_meta.visual_end - row_meta.visual_start;
    row_input.tab_size = viewport.tab_size();
    row_input.text_renderer = &text_renderer;
    row_input.theme = &theme;
    BuildDecoratedRow(row_desc, row_input);
    {
      util::PerformanceTrace::Scope row_render_scope("EditorViewRenderer::Render::DecoratedRow");
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer, row_desc);
    }
    if (const auto severity = HighestDiagnosticSeverityForLine(diagnostics, line_index);
        severity.has_value()) {
      DrawDiagnosticGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height,
                                 *severity);
    }
    // Plugin gutter marks share the breakpoint/diagnostic marker slot. Draw the
    // highest-priority mark (first after the line-sorted, priority-desc order)
    // once per logical line, before the breakpoint dot / execution arrow so the
    // debugger's own glyphs win when they coincide.
    if (plugin_decorations != nullptr && IsLogicalLineHead(soft_wrap, row_meta.visual_start)) {
      const std::span<const GutterMarkDecoration> marks =
          plugin_decorations->GutterMarksForLine(static_cast<std::uint32_t>(line_index));
      if (!marks.empty()) {
        GutterIconRegistry::Draw(renderer, marks.front().shape, marks.front().color, gutter.x, y,
                                 gutter.w, metrics.line_height);
      }
    }
    if (breakpoint_gutter_marks != nullptr &&
        breakpoint_gutter_mark_index < breakpoint_gutter_marks->size() &&
        (*breakpoint_gutter_marks)[breakpoint_gutter_mark_index].visual_row_index ==
            visual_row_index) {
      const BreakpointGutterMark& bp_mark =
          (*breakpoint_gutter_marks)[breakpoint_gutter_mark_index];
      const BreakpointGutterKind bp_kind = bp_mark.is_logpoint ? BreakpointGutterKind::Logpoint
                                           : bp_mark.has_condition
                                               ? BreakpointGutterKind::Conditional
                                               : BreakpointGutterKind::Plain;
      DrawBreakpointGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height,
                                 bp_mark.verified, bp_kind, bp_mark.enabled);
      ++breakpoint_gutter_mark_index;
    }
    if (is_execution_line) {
      // Drawn after the breakpoint dot so the arrow overlays it when a session
      // stops on a breakpoint line.
      DrawExecutionLineGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height);
    }
    if (fold_gutter_marks != nullptr && fold_gutter_mark_index < fold_gutter_marks->size() &&
        (*fold_gutter_marks)[fold_gutter_mark_index].visual_row_index == visual_row_index) {
      DrawFoldGutterMarker(renderer,
                           selected ? theme.current_line_number : theme.line_number,
                           theme.row_highlight, theme.border,
                           FoldGutterMarkerRect(gutter.x, gutter.w, y, metrics.line_height),
                           (*fold_gutter_marks)[fold_gutter_mark_index].collapsed);
      ++fold_gutter_mark_index;
    }
    if (IsLogicalLineHead(soft_wrap, row_meta.visual_start)) {
      const auto [end, _] = std::to_chars(line_number_buf, line_number_buf + sizeof(line_number_buf),
                                          line_index + 1);
      const float number_x = gutter.x + kGutterLineNumberInset;
      const SDL_Color number_color = selected ? theme.current_line_number : theme.line_number;
      if (batch_gutter_numbers) {
        // Defer to a single batched DrawRuns after the loop (see flush below). The
        // gutter background is a separate fill, and the atlas backend's DrawStringOn
        // ignores its background arg, so deferring the foreground digits is exact.
        GutterNumber& number = gutter_number_scratch_.emplace_back();
        number.x = number_x;
        number.y = y;
        number.color = number_color;
        number.text.assign(line_number_buf, end);
      } else {
        // Non-batching backend: draw inline exactly as before, no collection cost.
        text_renderer.DrawStringOn(renderer, number_x, y, number_color,
                                   selected ? theme.row_highlight : theme.gutter_background,
                                   std::string_view{line_number_buf, end});
      }
    }

    if (draw_caret && selected && row_caret.visible) {
      const float caret_x = row_text_x + static_cast<float>(row_caret.column) * char_width_px +
                            inlay_shift_px(row_visual_origin + row_caret.column);
      DrawCaret(renderer, caret_x, y, metrics.line_height, theme.cursor);
      // Ghost-text tail: the suggestion's first line, drawn dimmed starting at the
      // caret. Below-caret rows (if any) render as a Below inset gap by the shell.
      if (view_model != nullptr && view_model->ghost_text_tail.has_value() &&
          view_model->ghost_text_tail->visual_row == visual_row_index) {
        text_renderer.DrawString(renderer, caret_x, y, theme.text_muted,
                                 view_model->ghost_text_tail->text);
      }
    }

    if (draw_caret) {
      while (secondary_caret_index < secondary_carets.size() &&
             secondary_carets[secondary_caret_index].line < line_index) {
        ++secondary_caret_index;
      }
      for (std::size_t idx = secondary_caret_index;
           idx < secondary_carets.size() && secondary_carets[idx].line == line_index; ++idx) {
        const std::size_t row_start_visual_sc = row_visual_origin;
        const std::size_t row_end_visual_sc = row_meta.visual_end;
        const std::size_t visual_column = TextLayout::VisualColumnFromLayoutClipped(
            row_layout, row_start_visual_sc, row_end_visual_sc, secondary_carets[idx].column);
        const bool caret_hits_last_column = visual_column == row_end_visual_sc &&
                                            row_end_visual_sc == row_layout.visual_columns;
        if (visual_column < row_start_visual_sc ||
            (visual_column >= row_end_visual_sc && !caret_hits_last_column)) {
          continue;
        }
        const float caret_x =
            row_text_x + static_cast<float>(visual_column - row_start_visual_sc) * char_width_px +
            inlay_shift_px(visual_column);
        DrawCaret(renderer, caret_x, y, metrics.line_height, theme.cursor);
      }
    }

    // Plugin-published end-of-line inline text (Error Lens, GitLens blame) and
    // code lenses, drawn once per logical line on its first visual row, past the
    // line's last glyph. The segment geometry mirrors WorkspaceShell's click
    // hit-test (shared BuildEolDecorationSegments) so a click lands exactly where
    // the code-lens affordance was painted.
    if (plugin_decorations != nullptr && IsLogicalLineHead(soft_wrap, row_meta.visual_start)) {
      // Non-wrapped rows already resolved this exact line slice for the mid-line
      // inlay pass above (row_inline_texts); reuse it instead of a second
      // binary search. Only the soft-wrap head (where row_inline_texts is left
      // empty) needs a fresh lookup.
      const std::span<const InlineTextDecoration> inline_texts =
          soft_wrap ? plugin_decorations->InlineTextsForLine(static_cast<std::uint32_t>(line_index))
                    : row_inline_texts;
      // Phase E2: when above-line code lenses are active they render as inset
      // strips, so suppress the end-of-line affordance for the same lenses.
      const std::span<const CodeLensDecoration> code_lenses =
          (view_model != nullptr && view_model->code_lens_above)
              ? std::span<const CodeLensDecoration>{}
              : plugin_decorations->CodeLensesForLine(static_cast<std::uint32_t>(line_index));
      if (!inline_texts.empty() || !code_lenses.empty()) {
        // Non-wrap rows already have the full-line layout in `row_layout`; only
        // the wrap branch (where row_layout is a single wrapped slice) needs the
        // full-line width, served by reference from the cache.
        const std::size_t full_line_visual_columns =
            soft_wrap ? viewport.VisibleLineLayoutRef(line_index).visual_columns
                      : row_layout.visual_columns;
        // Push the anchor past the line's mid-line inlay hints so end-of-line
        // decorations still sit beyond the visually-last glyph.
        const float anchor_x =
            metrics.text_x +
            static_cast<float>(full_line_visual_columns + row_inlay_total_cells) * char_width_px;
        const float right_limit = rect.x + rect.w - 12.0f;
        BuildEolDecorationSegments(text_renderer, inline_texts, code_lenses, anchor_x, y,
                                   metrics.line_height, right_limit, eol_decoration_scratch_);
        for (const EolDecorationSegment& seg : eol_decoration_scratch_) {
          if (seg.kind == EolDecorationSegment::Kind::CodeLens) {
            text_renderer.DrawString(renderer, seg.rect.x, seg.rect.y, theme.accent,
                                     code_lenses[seg.index].text);
          } else {
            const InlineTextDecoration& inl = inline_texts[seg.index];
            const SDL_Color fg = inl.color.a != 0 ? inl.color : theme.text_disabled;
            if (inl.background.a != 0) {
              text_renderer.DrawStringOn(renderer, seg.rect.x, seg.rect.y, fg, inl.background,
                                         inl.text);
            } else {
              text_renderer.DrawString(renderer, seg.rect.x, seg.rect.y, fg, inl.text);
            }
          }
        }
      }
    }

    if (blame_overlay.has_value() && blame_overlay->visible) {
      while (blame_index < blame_overlay->lines.size() &&
             blame_overlay->lines[blame_index].line_index < line_index) {
        ++blame_index;
      }
      if (blame_index < blame_overlay->lines.size() &&
          blame_overlay->lines[blame_index].line_index == line_index) {
        text_renderer.DrawStringOn(renderer, blame_overlay->lines[blame_index].rect.x,
                                   blame_overlay->lines[blame_index].rect.y, theme.text_disabled,
                                   row_background,
                                   blame_overlay->lines[blame_index].text);
      }
    }
  }

  // Flush the deferred gutter line numbers in one batched DrawRuns. On the GPU
  // atlas backend this collapses every visible line number into a single submit
  // (digits come from the shared glyph atlas, so scrolling no longer rebuilds and
  // uploads a composite texture per line number); on other backends it is one
  // DrawString per number, identical to the prior inline draw.
  if (!gutter_number_scratch_.empty()) {
    gutter_number_run_scratch_.clear();
    gutter_number_run_scratch_.reserve(gutter_number_scratch_.size());
    for (const GutterNumber& number : gutter_number_scratch_) {
      gutter_number_run_scratch_.push_back(render::TextRun{
          .x = number.x,
          .y = number.y,
          .color = number.color,
          .text = number.text,
      });
    }
    text_renderer.DrawRuns(renderer, gutter_number_run_scratch_.data(),
                           gutter_number_run_scratch_.size());
  }
}

}  // namespace microide::editor
