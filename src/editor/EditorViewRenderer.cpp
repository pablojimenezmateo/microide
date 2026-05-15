#include "editor/EditorViewRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsRender.h"
#include "render/SurfacePrimitives.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::editor {

namespace {

const DecoratedTextGridRenderer kDecoratedRowRenderer;

float ComputeGutterWidth(const render::TextRenderer& text_renderer, std::size_t line_count) {
  char buf[20];
  const auto [end, _] = std::to_chars(buf, buf + sizeof(buf), std::max<std::size_t>(1, line_count));
  return std::max(48.0f, text_renderer.MeasureWidth(std::string_view{buf, end}) + 18.0f);
}

void DrawFoldGutterMarker(SDL_Renderer* renderer,
                          SDL_Color color,
                          const SDL_FRect& rect,
                          bool collapsed) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);

  const float mid_y = std::floor(rect.y + rect.h * 0.5f);
  const SDL_FRect horizontal =
      SDL_FRect{rect.x + 2.0f, mid_y, std::max(1.0f, rect.w - 4.0f), 1.0f};
  SDL_RenderFillRect(renderer, &horizontal);
  if (!collapsed) {
    return;
  }

  const float mid_x = std::floor(rect.x + rect.w * 0.5f);
  const SDL_FRect vertical =
      SDL_FRect{mid_x, rect.y + 2.0f, 1.0f, std::max(1.0f, rect.h - 4.0f)};
  SDL_RenderFillRect(renderer, &vertical);
}

void DrawPlaceholderView(SDL_Renderer* renderer,
                         const render::TextRenderer& text_renderer,
                         const render::Theme& theme,
                         const SDL_FRect& rect) {
  struct CheatRow {
    std::string_view key;
    std::string_view label;
  };

  const auto draw_panel = [&](const SDL_FRect& panel,
                              std::string_view title,
                              auto&& rows,
                              float key_width) {
    SDL_SetRenderDrawColor(renderer, theme.surface_background.r, theme.surface_background.g,
                           theme.surface_background.b, theme.surface_background.a);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    SDL_RenderRect(renderer, &panel);

    const float inset_x = panel.x + 12.0f;
    const float title_y = panel.y + 10.0f;
    const float row_y_start = title_y + text_renderer.LineHeight() + 8.0f;
    const float row_step = text_renderer.LineHeight() + 1.0f;

    text_renderer.DrawStringOn(renderer, inset_x, title_y, theme.surface_text,
                               theme.surface_background, title);

    float row_y = row_y_start;
    for (const auto& row : rows) {
      if (row_y + text_renderer.LineHeight() > panel.y + panel.h - 10.0f) {
        break;
      }

      if (!row.label.empty() && key_width > 0.0f) {
        text_renderer.DrawStringOn(
            renderer, inset_x, row_y, theme.surface_text, theme.surface_background,
            text_renderer.TruncateToWidth(row.key, key_width - 8.0f));
        text_renderer.DrawStringOn(
            renderer, inset_x + key_width, row_y, theme.text_secondary,
            theme.surface_background,
            text_renderer.TruncateToWidth(row.label, panel.w - key_width - 24.0f));
      } else {
        text_renderer.DrawStringOn(
            renderer, inset_x, row_y, theme.text_secondary, theme.surface_background,
            text_renderer.TruncateToWidth(row.key, panel.w - 24.0f));
      }
      row_y += row_step;
    }
  };

  static constexpr std::array<CheatRow, 19> kCoreShortcuts = {{
      {"Ctrl+E", "command palette"},
      {"F6", "file finder overlay"},
      {"F8", "toggle sidebar"},
      {"Ctrl+0 / - / =", "reset / shrink / grow UI"},
      {"Ctrl+Tab", "switch editor/sidebar/panel focus"},
      {"Ctrl+S", "save active file"},
      {"Ctrl+W", "close active tab"},
      {"Ctrl+Shift+F", "project search sidebar"},
      {"Ctrl+F", "search in buffer"},
      {"Ctrl+H", "replace in buffer"},
      {"Ctrl+A", "select all"},
      {"Ctrl+C / X / V", "copy / cut / paste"},
      {"Ctrl+Z", "undo"},
      {"Ctrl+Shift+Z / Ctrl+Y", "redo"},
      {"Arrows + Shift", "move / extend selection"},
      {"Home / End", "line start / line end"},
      {"Ctrl+Home / End", "file start / file end"},
      {"PgUp / PgDn / Esc", "page move / close overlay"},
  }};

  static constexpr std::array<CheatRow, 40> kToolShortcuts = {{
      {"Tree Up / Down", "move tree selection"},
      {"Tree Left / Right", "collapse / expand"},
      {"Tree Enter", "open file or toggle dir"},
      {"Tree R", "refresh tree"},
      {"Tree D", "compare selected file"},
      {"Finder type", "filter project files"},
      {"Finder Backspace", "delete query char"},
      {"Finder Up / Down / Pg", "move result selection"},
      {"Finder Enter", "open selected file"},
      {"Search Up / Down / Home / End", "move result selection"},
      {"Search PgUp / PgDn", "page through results"},
      {"Search Enter / Right", "open selected result"},
      {"Search /", "edit query"},
      {"Search =", "edit replace text"},
      {"Search buttons", "toggle Lit/Rx, case mode, and hidden files"},
      {"Search r", "rerun current query"},
      {"Search R", "replace all literal matches"},
      {"Search Esc", "close temporary search"},
      {"Buffer search type", "filter buffer matches"},
      {"Buffer search Backspace", "delete query char"},
      {"Buffer search Up / Down / Pg", "move match selection"},
      {"Buffer search Enter", "jump to match and close"},
      {"Buffer replace Tab", "switch query / replace field"},
      {"Buffer replace Enter", "replace current match"},
      {"Buffer replace Ctrl+Enter", "replace all matches"},
      {"Compare j / k or arrows", "move compare rows"},
      {"Compare [ / ]", "jump previous / next hunk"},
      {"Compare Enter / o", "open working file"},
      {"Compare Esc", "close compare tab"},
      {"Merge Alt+[ / Alt+]", "move previous / next conflict"},
      {"Merge Alt+I / B / C / M", "take incoming / base / current / both"},
      {"Merge type / edit", "edit the result pane directly"},
      {"Merge click / drag", "select text or resize merge panes"},
      {"Merge Alt+O / Esc", "open result / close merge tab"},
      {"Split click", "focus the hovered editor split"},
      {"Split divider drag", "resize editor split"},
      {"Terminal click", "focus the terminal panel"},
      {"Terminal type / Enter", "send text / run command"},
      {"Terminal arrows / Pg / Home", "send terminal navigation keys"},
      {"Dirty prompt Tab / Enter / Esc", "pick / confirm / cancel"},
  }};

  static constexpr std::array<CheatRow, 38> kCommands = {{
      {"colorscheme [name]", ""},
      {"ui-scale [n|up|down|reset]", ""},
      {"help", ""},
      {"open <path>", ""},
      {"tab [path]", ""},
      {"tabswitch <tab>", ""},
      {"tabmove <n>", ""},
      {"compare [path] [commit]", ""},
      {"merge <base> <incoming> <current> [output]", ""},
      {"tab-size [n]", ""},
      {"indent-width [n]", ""},
      {"soft-tabs [on|off]", ""},
      {"save", ""},
      {"quit", ""},
      {"jump <line[:col]>", ""},
      {"vsplit [path]", ""},
      {"unsplit", ""},
      {"split-next", ""},
      {"split-prev", ""},
      {"split-first", ""},
      {"split-last", ""},
      {"term [command]", ""},
      {"find <query>", ""},
      {"files [root]", ""},
      {"tree [root]", ""},
      {"search <query>", ""},
      {"project-search [query]", ""},
      {"goto <line[:col]>", ""},
      {"tree-refresh", ""},
      {"sidebar-toggle [tool]", ""},
      {"sidebar-show [tool]", ""},
      {"sidebar-hide", ""},
      {"sidebar-close", ""},
      {"sidebar-width <n>", ""},
      {"focus <editor|sidebar|panel>", ""},
  }};

  const float card_width = std::min(rect.w - 48.0f, 1180.0f);
  const float card_height = std::min(rect.h - 48.0f, 620.0f);
  const float card_x = rect.x + std::max(24.0f, (rect.w - card_width) * 0.5f);
  const float card_y = rect.y + std::max(24.0f, rect.h * 0.05f);
  const SDL_FRect card = SDL_FRect{card_x, card_y, std::max(320.0f, card_width), card_height};
  const SDL_FRect header =
      render::DrawTitledCardFrame(renderer, theme, card, 32.0f, render::CardStyle::Raised);

  const float inset_x = card.x + 18.0f;
  text_renderer.DrawStringOn(renderer, inset_x, header.y + 8.0f, theme.chrome_text,
                             theme.chrome_background, "Workspace Ready");
  text_renderer.DrawStringOn(renderer, inset_x, card.y + 46.0f, theme.text_secondary,
                             theme.surface_raised,
                             "Open a file from the tree or use this reference while the editor is empty.");
  text_renderer.DrawStringOn(renderer, inset_x, card.y + 64.0f, theme.text_muted,
                             theme.surface_raised,
                             microide::workspace::JoinHintSegments(
                                 {"Commands on the right", "Shortcuts on the left"}));

  const float panels_y = card.y + 98.0f;
  const float panels_h = card.h - 132.0f;
  const float panel_gap = 12.0f;
  const float panel_w = (card.w - 36.0f - panel_gap * 2.0f) / 3.0f;
  const SDL_FRect core_panel = SDL_FRect{card.x + 12.0f, panels_y, panel_w, panels_h};
  const SDL_FRect tool_panel =
      SDL_FRect{core_panel.x + core_panel.w + panel_gap, panels_y, panel_w, panels_h};
  const SDL_FRect command_panel =
      SDL_FRect{tool_panel.x + tool_panel.w + panel_gap, panels_y, panel_w, panels_h};

  draw_panel(core_panel, "Core Shortcuts", kCoreShortcuts, 128.0f);
  draw_panel(tool_panel, "Tool Shortcuts", kToolShortcuts, 148.0f);
  draw_panel(command_panel, "Command Palette", kCommands, 0.0f);

  // Backend label is rebuilt only when the placeholder view is drawn (i.e. there is no open file).
  // Still cheap, but reuse a thread_local so the placeholder paint stays allocation-free per
  // frame.
  thread_local std::string backend_label_scratch;
  backend_label_scratch.clear();
  backend_label_scratch.reserve(20 + text_renderer.BackendName().size());
  backend_label_scratch.append("text renderer: ");
  backend_label_scratch.append(text_renderer.BackendName());
  const float backend_width = text_renderer.MeasureWidth(backend_label_scratch);
  text_renderer.DrawStringOn(renderer, card.x + card.w - backend_width - 18.0f,
                             card.y + card.h - 24.0f, theme.text_disabled,
                             theme.surface_raised, backend_label_scratch);
}

}  // namespace

SDL_FRect FoldGutterMarkerRect(float gutter_x,
                               float gutter_width,
                               float row_y,
                               float line_height) {
  constexpr float kMarkerSize = 8.0f;
  return SDL_FRect{gutter_x + gutter_width - 14.0f,
                   row_y + std::max(1.0f, std::floor((line_height - kMarkerSize) * 0.5f)),
                   kMarkerSize,
                   kMarkerSize};
}

EditorViewMetrics EditorViewRenderer::ComputeMetrics(const render::TextRenderer& text_renderer,
                                                     const TextViewport& viewport,
                                                     const SDL_FRect& rect,
                                                     std::size_t sticky_scroll_rows) {
  EditorViewMetrics metrics;
  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  metrics.gutter_width = ComputeGutterWidth(text_renderer, viewport.line_count());
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
  metrics.visible_columns = static_cast<std::size_t>(
      std::max(8.0f, (rect.w - metrics.gutter_width - 28.0f) / char_width));
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
                                const FoldingModel* folding_model) const {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("EditorViewRenderer::Render");
  SDL_SetRenderDrawColor(renderer, theme.editor_background.r, theme.editor_background.g,
                         theme.editor_background.b, theme.editor_background.a);
  SDL_RenderFillRect(renderer, &rect);

  if (viewport.is_placeholder()) {
    DrawPlaceholderView(renderer, text_renderer, theme, rect);
    viewport.SetViewportSize(1, 1);
    return;
  }

  std::optional<BracketMatchPair> bracket_match_pair;
  if (bracket_match_highlight_enabled) {
    const std::size_t caret_line = viewport.cursor_line();
    const std::size_t caret_column = viewport.cursor_column();
    const std::size_t layout_revision = viewport.layout_revision();
    if (bracket_match_cache_.valid && bracket_match_cache_.viewport == &viewport &&
        bracket_match_cache_.layout_revision == layout_revision &&
        bracket_match_cache_.caret_line == caret_line &&
        bracket_match_cache_.caret_column == caret_column) {
      bracket_match_pair = bracket_match_cache_.pair;
      ++bracket_match_cache_.hits;
    } else {
      bracket_match_pair = FindBracketMatch(viewport, caret_line, caret_column);
      bracket_match_cache_.viewport = &viewport;
      bracket_match_cache_.layout_revision = layout_revision;
      bracket_match_cache_.caret_line = caret_line;
      bracket_match_cache_.caret_column = caret_column;
      bracket_match_cache_.pair = bracket_match_pair;
      bracket_match_cache_.valid = true;
      ++bracket_match_cache_.misses;
    }
  } else if (bracket_match_cache_.valid) {
    bracket_match_cache_.viewport = nullptr;
    bracket_match_cache_.layout_revision = 0;
    bracket_match_cache_.caret_line = 0;
    bracket_match_cache_.caret_column = 0;
    bracket_match_cache_.pair.reset();
    bracket_match_cache_.valid = false;
  }

  const std::size_t sticky_row_count =
      view_model != nullptr ? view_model->sticky_lines.size() : 0;
  const EditorViewMetrics metrics =
      ComputeMetrics(text_renderer, viewport, rect, sticky_row_count);
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
  const auto& secondary_carets = viewport.secondary_carets();
  const auto selection = viewport.selection_range();
  char line_number_buf[20];
  // Skip the ToLower allocation entirely on the common no-search frame; reuse
  // the scratch buffer when a query is active so the std::string capacity
  // persists across frames.
  if (search_query.empty()) {
    lowered_search_query_scratch_.clear();
  } else {
    lowered_search_query_scratch_.resize(search_query.size());
    std::transform(search_query.begin(), search_query.end(),
                   lowered_search_query_scratch_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
    if (indent_guides_cache_.valid && indent_guides_cache_.viewport == &viewport &&
        indent_guides_cache_.layout_revision == viewport.layout_revision() &&
        indent_guides_cache_.fold_revision == viewport.folding_revision() &&
        indent_guides_cache_.fold_emphasis_revision == fold_emphasis_revision &&
        indent_guides_cache_.scroll_line == scroll_line &&
        indent_guides_cache_.visible_rows_count == visible_rows_for_guides.size() &&
        indent_guides_cache_.indent_width == indent_width &&
        indent_guides_cache_.caret_line == cursor_line) {
      ++indent_guides_cache_.hits;
    } else {
      const std::size_t caret_indent =
          cursor_line < lines.size()
              ? LeadingVisualIndent(lines[cursor_line], viewport.tab_size())
              : 0;
      ComputeIndentGuides(lines, visible_rows_for_guides, viewport.tab_size(),
                          indent_width, cursor_line, caret_indent,
                          &indent_guides_cache_.runs, folding_model);
      indent_guides_cache_.viewport = &viewport;
      indent_guides_cache_.layout_revision = viewport.layout_revision();
      indent_guides_cache_.fold_revision = viewport.folding_revision();
      indent_guides_cache_.fold_emphasis_revision = fold_emphasis_revision;
      indent_guides_cache_.scroll_line = scroll_line;
      indent_guides_cache_.visible_rows_count = visible_rows_for_guides.size();
      indent_guides_cache_.indent_width = indent_width;
      indent_guides_cache_.caret_line = cursor_line;
      indent_guides_cache_.valid = true;
      ++indent_guides_cache_.misses;
    }
    indent_guides_to_paint = &indent_guides_cache_.runs;
  } else if (indent_guides_cache_.valid) {
    indent_guides_cache_.viewport = nullptr;
    indent_guides_cache_.runs.clear();
    indent_guides_cache_.valid = false;
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
      sticky_row.fills.clear();
      sticky_row.runs.clear();
      sticky_row.underlines.clear();
      sticky_row.fills.push_back(DecoratedTextFill{
          .rect = SDL_FRect{rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, metrics.line_height},
          .color = row_background,
      });
      const auto layout = row_layout;
      {
        util::PerformanceTrace::Scope token_scope("EditorViewRenderer::Render::StickyHighlightedLineTokens");
        const std::vector<SyntaxTokenKind>& token_kinds = viewport.HighlightedLineTokens(line_index);
        AppendLayoutSyntaxTextRuns(sticky_row, text_renderer, theme, metrics.text_x, y, layout,
                                   selected ? theme.text_primary : theme.text_secondary,
                                   token_kinds);
      }
      AppendDiagnosticUnderlines(sticky_row, text_renderer, theme, metrics.text_x, y,
                                 metrics.line_height, lines[line_index], line_index,
                                 row_meta.visual_start, row_meta.visual_end - row_meta.visual_start,
                                 viewport.tab_size(), diagnostics);
      {
        util::PerformanceTrace::Scope row_render_scope("EditorViewRenderer::Render::StickyDecoratedRow");
        kDecoratedRowRenderer.RenderRow(renderer, text_renderer, sticky_row);
      }
      if (const auto severity = HighestDiagnosticSeverityForLine(diagnostics, line_index);
          severity.has_value()) {
        DrawDiagnosticGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height,
                                   *severity);
      }
      if (!soft_wrap || row_meta.visual_start == 0) {
        const auto [end_sticky, _] =
            std::to_chars(line_number_buf, line_number_buf + sizeof(line_number_buf),
                          line_index + 1);
        text_renderer.DrawStringOn(
            renderer, gutter.x + 10.0f, y,
            selected ? theme.current_line_number : theme.line_number,
            selected ? theme.row_highlight : theme.gutter_background,
            std::string_view{line_number_buf, end_sticky});
      }
      if (folding_model != nullptr && (!soft_wrap || row_meta.visual_start == 0) &&
          folding_model->FoldStartingAt(line_index).has_value()) {
        const bool collapsed = folding_model->IsCollapsedAtOpener(line_index);
        const float mark_size = std::max(4.0f, std::min(8.0f, metrics.line_height - 6.0f));
        const float mark_x = gutter.x + gutter.w - 4.0f - mark_size;
        const float mark_y = y + (metrics.line_height - mark_size) * 0.5f;
        const SDL_FRect mark_rect = SDL_FRect{mark_x, mark_y, mark_size, mark_size};
        const SDL_Color color = selected ? theme.text_primary : theme.text_secondary;
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        if (collapsed) {
          SDL_RenderFillRect(renderer, &mark_rect);
        } else {
          SDL_FRect top = SDL_FRect{mark_x, mark_y, mark_size, 1.0f};
          SDL_FRect bottom = SDL_FRect{mark_x, mark_y + mark_size - 1.0f, mark_size, 1.0f};
          SDL_FRect left = SDL_FRect{mark_x, mark_y, 1.0f, mark_size};
          SDL_FRect right = SDL_FRect{mark_x + mark_size - 1.0f, mark_y, 1.0f, mark_size};
          SDL_RenderFillRect(renderer, &top);
          SDL_RenderFillRect(renderer, &bottom);
          SDL_RenderFillRect(renderer, &left);
          SDL_RenderFillRect(renderer, &right);
        }
      }
    }
    SDL_SetRenderDrawColor(renderer, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    const SDL_FRect sticky_divider =
        SDL_FRect{rect.x, metrics.first_line_y - 1.0f, rect.w, 1.0f};
    SDL_RenderFillRect(renderer, &sticky_divider);
  }

  util::PerformanceTrace::Scope rows_scope("EditorViewRenderer::Render::Rows");
  for (std::size_t row = 0; row < metrics.visible_rows; ++row) {
    const std::size_t visual_row_index = scroll_line + row;
    if (visual_row_index >= viewport.visual_line_count()) {
      break;
    }
    const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
    const std::size_t line_index = row_meta.line_index;
    const auto row_layout = soft_wrap ? viewport.VisibleWrappedRowLayout(visual_row_index)
                                      : viewport.VisibleLineLayout(line_index);
    if (line_index >= lines.size()) {
      break;
    }

    const float y = metrics.first_line_y + static_cast<float>(row) * metrics.line_height;
    const std::size_t row_visual_origin =
        soft_wrap ? row_meta.visual_start : viewport.horizontal_scroll();
    const bool selected = line_index == cursor_line;
    const bool active_search_line =
        active_search_match.has_value() &&
        line_index >= active_search_match->start.line &&
        line_index <= active_search_match->end.line;
    const SDL_Color row_background = selected ? theme.row_highlight : theme.editor_background;
    DecoratedTextRow& row_desc = scratch_row_;
    row_desc.fills.clear();
    row_desc.runs.clear();
    row_desc.underlines.clear();
    if (selected) {
      row_desc.fills.push_back(DecoratedTextFill{
          .rect = SDL_FRect{rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, metrics.line_height},
          .color = theme.row_highlight,
      });
    }

    if (!lowered_search_query.empty()) {
      const SearchMatchCacheKey cache_key{
          .viewport = &viewport,
          .layout_revision = viewport.layout_revision(),
          .line_index = line_index,
          .query = lowered_search_query,
      };
      auto cache_it = search_match_cache_.find(cache_key);
      if (cache_it == search_match_cache_.end()) {
        const std::string& src = lines[line_index];
        lowered_line_scratch.resize(src.size());
        std::transform(src.begin(), src.end(), lowered_line_scratch.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
        auto [inserted_it, _] = search_match_cache_.emplace(cache_key, std::move(matches));
        search_match_cache_order_.push_back(cache_key);
        cache_it = inserted_it;
      }

      for (const auto& [match_start, match_end] : cache_it->second) {
        const std::size_t start_visual =
            TextLayout::VisualColumnForTextColumn(lines[line_index], match_start, viewport.tab_size());
        const std::size_t end_visual = TextLayout::VisualColumnForTextColumn(
            lines[line_index], match_end, viewport.tab_size());
        const std::size_t row_start_visual = row_visual_origin;
        const std::size_t row_end_visual = row_meta.visual_end;
        const std::size_t visible_start = std::max(start_visual, row_start_visual);
        const std::size_t visible_end = std::min(end_visual, row_end_visual);
        if (visible_end > visible_start) {
          const bool is_active_match =
              active_search_line &&
              match_start == active_search_match->start.column &&
              line_index == active_search_match->start.line;
          row_desc.fills.push_back(DecoratedTextFill{
              .rect =
                  SDL_FRect{
                      metrics.text_x +
                          static_cast<float>(visible_start - row_start_visual) *
                              text_renderer.CharWidth(),
                      y - 1.0f,
                      static_cast<float>(visible_end - visible_start) * text_renderer.CharWidth(),
                      metrics.line_height,
                  },
              .color = is_active_match ? theme.search_match_active : theme.search_match,
          });
        }
      }
    }

    if (view_model != nullptr && !view_model->occurrence_ranges.empty()) {
      const std::size_t row_start_visual_occ = row_visual_origin;
      const std::size_t row_end_visual_occ = row_meta.visual_end;
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
        const std::size_t match_start = occ.start_column;
        const std::size_t match_end = occ.end_column;
        const std::size_t start_visual =
            TextLayout::VisualColumnForTextColumn(lines[line_index], match_start, viewport.tab_size());
        const std::size_t end_visual = TextLayout::VisualColumnForTextColumn(
            lines[line_index], match_end, viewport.tab_size());
        const std::size_t visible_start = std::max(start_visual, row_start_visual_occ);
        const std::size_t visible_end = std::min(end_visual, row_end_visual_occ);
        if (visible_end <= visible_start) {
          continue;
        }
        row_desc.fills.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    metrics.text_x +
                        static_cast<float>(visible_start - row_start_visual_occ) *
                            text_renderer.CharWidth(),
                    y - 1.0f,
                    static_cast<float>(visible_end - visible_start) * text_renderer.CharWidth(),
                    metrics.line_height,
                },
            .color =
                occ.is_primary_seed ? theme.search_match_active : theme.search_match,
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
      const std::size_t start_visual =
          TextLayout::VisualColumnForTextColumn(lines[line_index], line_start, viewport.tab_size());
      const std::size_t end_visual =
          TextLayout::VisualColumnForTextColumn(lines[line_index], line_end, viewport.tab_size());
      const std::size_t row_start_visual = row_visual_origin;
      const std::size_t row_end_visual = row_meta.visual_end;
      const std::size_t visible_start = std::max(start_visual, row_start_visual);
      const std::size_t visible_end = std::min(end_visual, row_end_visual);
      if (visible_end > visible_start) {
        row_desc.fills.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    metrics.text_x +
                        static_cast<float>(visible_start - row_start_visual) *
                            text_renderer.CharWidth(),
                    y - 1.0f,
                    static_cast<float>(visible_end - visible_start) * text_renderer.CharWidth(),
                    metrics.line_height,
                },
            .color = theme.selection_fill,
        });
      }
    }

    if (bracket_match_pair.has_value()) {
      const auto append_bracket_cell = [&](std::size_t bracket_line, std::size_t bracket_column) {
        if (bracket_line != line_index) return;
        if (bracket_column >= lines[line_index].size()) return;
        const std::size_t cell_visual = TextLayout::VisualColumnForTextColumn(
            lines[line_index], bracket_column, viewport.tab_size());
        const std::size_t row_start_visual = row_visual_origin;
        const std::size_t row_end_visual = row_meta.visual_end;
        if (cell_visual < row_start_visual || cell_visual >= row_end_visual) return;
        row_desc.fills.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    metrics.text_x +
                        static_cast<float>(cell_visual - row_start_visual) *
                            text_renderer.CharWidth(),
                    y - 1.0f,
                    text_renderer.CharWidth(),
                    metrics.line_height,
                },
            .color = theme.bracket_match_background,
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
        row_desc.fills.push_back(DecoratedTextFill{
            .rect =
                SDL_FRect{
                    metrics.text_x +
                        static_cast<float>(guide.column - row_start_visual) *
                            text_renderer.CharWidth(),
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
      const float char_width = text_renderer.CharWidth();
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
          if (glyph.row_visual_start != row_start_visual ||
              glyph.row_visual_end != row_end_visual) {
            continue;
          }
          const float cell_x =
              metrics.text_x + static_cast<float>(glyph.cell_visual_start - row_start_visual) *
                                   char_width;
          if (!glyph.is_tab_rule) {
            row_desc.fills.push_back(DecoratedTextFill{
                .rect =
                    SDL_FRect{
                        cell_x + char_width * 0.5f - 1.0f,
                        y + metrics.line_height * 0.5f - 1.0f,
                        2.0f,
                        2.0f,
                    },
                .color = theme.text_disabled,
            });
          } else {
            const float cell_w = static_cast<float>(glyph.cell_visual_extent) * char_width;
            row_desc.fills.push_back(DecoratedTextFill{
                .rect =
                    SDL_FRect{
                        cell_x + 2.0f,
                        y + metrics.line_height * 0.5f,
                        cell_w - 4.0f,
                        1.0f,
                    },
                .color = theme.text_disabled,
            });
          }
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
              metrics.text_x +
              static_cast<float>(cell_start - row_start_visual) * char_width;
          if (c == ' ') {
            row_desc.fills.push_back(DecoratedTextFill{
                .rect =
                    SDL_FRect{
                        cell_x + char_width * 0.5f - 1.0f,
                        y + metrics.line_height * 0.5f - 1.0f,
                        2.0f,
                        2.0f,
                    },
                .color = theme.text_disabled,
            });
          } else if (c == '\t') {
            const float cell_w = static_cast<float>(cell_width) * char_width;
            row_desc.fills.push_back(DecoratedTextFill{
                .rect =
                    SDL_FRect{
                        cell_x + 2.0f,
                        y + metrics.line_height * 0.5f,
                        cell_w - 4.0f,
                        1.0f,
                    },
                .color = theme.text_disabled,
            });
          }
        }
      }
    }

    const auto layout = row_layout;
    const std::vector<SyntaxTokenKind>* token_kinds = nullptr;
    {
      util::PerformanceTrace::Scope token_scope("EditorViewRenderer::Render::HighlightedLineTokens");
      token_kinds = &viewport.HighlightedLineTokens(line_index);
    }
    AppendLayoutSyntaxTextRuns(row_desc, text_renderer, theme, metrics.text_x, y, layout,
                               selected ? theme.text_primary : theme.text_secondary,
                               *token_kinds);
    AppendDiagnosticUnderlines(row_desc, text_renderer, theme, metrics.text_x, y,
                               metrics.line_height, lines[line_index], line_index,
                               row_meta.visual_start, row_meta.visual_end - row_meta.visual_start,
                               viewport.tab_size(), diagnostics);
    {
      util::PerformanceTrace::Scope row_render_scope("EditorViewRenderer::Render::DecoratedRow");
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer, row_desc);
    }
    if (const auto severity = HighestDiagnosticSeverityForLine(diagnostics, line_index);
        severity.has_value()) {
      DrawDiagnosticGutterMarker(renderer, theme, gutter.x, y, gutter.w, metrics.line_height,
                                 *severity);
    }
    if (fold_gutter_marks != nullptr && fold_gutter_mark_index < fold_gutter_marks->size() &&
        (*fold_gutter_marks)[fold_gutter_mark_index].visual_row_index == visual_row_index) {
      const SDL_Color marker_color =
          selected ? theme.current_line_number : theme.line_number;
      DrawFoldGutterMarker(renderer, marker_color,
                           FoldGutterMarkerRect(gutter.x, gutter.w, y, metrics.line_height),
                           (*fold_gutter_marks)[fold_gutter_mark_index].collapsed);
      ++fold_gutter_mark_index;
    }
    if (!soft_wrap || row_meta.visual_start == 0) {
      const auto [end, _] = std::to_chars(line_number_buf, line_number_buf + sizeof(line_number_buf),
                                          line_index + 1);
      text_renderer.DrawStringOn(renderer, gutter.x + 10.0f, y,
                                 selected ? theme.current_line_number : theme.line_number,
                                 selected ? theme.row_highlight : theme.gutter_background,
                                 std::string_view{line_number_buf, end});
    }

    if (folding_model != nullptr && (!soft_wrap || row_meta.visual_start == 0) &&
        folding_model->FoldStartingAt(line_index).has_value()) {
      const bool collapsed = folding_model->IsCollapsedAtOpener(line_index);
      const float mark_size = std::max(4.0f, std::min(8.0f, metrics.line_height - 6.0f));
      const float mark_x = gutter.x + gutter.w - 4.0f - mark_size;
      const float mark_y = y + (metrics.line_height - mark_size) * 0.5f;
      const SDL_FRect mark_rect = SDL_FRect{mark_x, mark_y, mark_size, mark_size};
      const SDL_Color color = selected ? theme.text_primary : theme.text_secondary;
      SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
      if (collapsed) {
        SDL_RenderFillRect(renderer, &mark_rect);
      } else {
        // Outlined marker: paint a 1px border so the expanded state is visually
        // distinct from the collapsed (filled) state without a separate glyph.
        SDL_FRect top = SDL_FRect{mark_x, mark_y, mark_size, 1.0f};
        SDL_FRect bottom = SDL_FRect{mark_x, mark_y + mark_size - 1.0f, mark_size, 1.0f};
        SDL_FRect left = SDL_FRect{mark_x, mark_y, 1.0f, mark_size};
        SDL_FRect right = SDL_FRect{mark_x + mark_size - 1.0f, mark_y, 1.0f, mark_size};
        SDL_RenderFillRect(renderer, &top);
        SDL_RenderFillRect(renderer, &bottom);
        SDL_RenderFillRect(renderer, &left);
        SDL_RenderFillRect(renderer, &right);
      }
    }

    if (draw_caret && selected && layout.caret_visible) {
      const float caret_x = metrics.text_x +
                            static_cast<float>(layout.caret_column) * text_renderer.CharWidth();
      const SDL_FRect caret = SDL_FRect{std::round(caret_x), y - 1.0f, 1.0f, metrics.line_height};
      SDL_SetRenderDrawColor(renderer, theme.cursor.r, theme.cursor.g, theme.cursor.b,
                             theme.cursor.a);
      SDL_RenderFillRect(renderer, &caret);
    }

    if (draw_caret) {
      while (secondary_caret_index < secondary_carets.size() &&
             secondary_carets[secondary_caret_index].line < line_index) {
        ++secondary_caret_index;
      }
      for (std::size_t idx = secondary_caret_index;
           idx < secondary_carets.size() && secondary_carets[idx].line == line_index; ++idx) {
        const std::size_t visual_column = TextLayout::VisualColumnForTextColumn(
            lines[line_index], secondary_carets[idx].column, viewport.tab_size());
        const std::size_t row_start_visual = row_meta.visual_start;
        const bool caret_hits_last_column =
            visual_column == row_meta.visual_end && row_meta.visual_end == row_layout.visual_columns;
        if (visual_column < row_start_visual ||
            (visual_column >= row_meta.visual_end && !caret_hits_last_column)) {
          continue;
        }
        const float caret_x =
            metrics.text_x +
            static_cast<float>(visual_column - row_start_visual) *
                text_renderer.CharWidth();
        const SDL_FRect caret =
            SDL_FRect{std::round(caret_x), y - 1.0f, 1.0f, metrics.line_height};
        SDL_SetRenderDrawColor(renderer, theme.cursor.r, theme.cursor.g, theme.cursor.b,
                               theme.cursor.a);
        SDL_RenderFillRect(renderer, &caret);
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
}

}  // namespace microide::editor
