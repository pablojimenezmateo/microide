#include "TestSupport.h"
#include "support/RowPixelOracle.h"

#include "editor/DiagnosticsRender.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/TextLayout.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <vector>

namespace microide::tests {

namespace {

using editor::DecoratedTextFill;
using editor::DecoratedTextRow;
using editor::LayoutLine;
using editor::PublishedDiagnostic;
using editor::RowDecorationInput;
using editor::RowFillSpan;
using editor::SyntaxTokenKind;
using editor::TextLayout;

// One painted band; large enough to hold every decoration in the battery.
constexpr int kCanvasWidth = 640;
constexpr int kCanvasHeight = 40;
constexpr float kTextX = 28.0f;
constexpr float kRowY = 14.0f;
constexpr float kLineHeight = 16.0f;
constexpr std::size_t kTabSize = 4;

// A fixed, representative line battery: pure ASCII, leading tabs, mixed
// tabs+spaces, and UTF-8 multibyte. Each is exercised at scroll 0 and scrolled.
struct LineCase {
  const char* label;
  std::string text;
  std::size_t horizontal_scroll;
  std::size_t visible_columns;
};

const std::vector<LineCase>& Battery() {
  static const std::vector<LineCase> cases = {
      {"ascii", "let value = compute(input);", 0, 40},
      {"ascii-scrolled", "let value = compute(input);", 6, 30},
      {"leading-tabs", "\t\treturn handler(state);", 0, 40},
      {"mixed-tabs-spaces", "\tif (x) \t{ go(); }", 0, 40},
      {"utf8", "café — naïve façade", 0, 40},
      {"utf8-scrolled", "café — naïve façade", 3, 24},
  };
  return cases;
}

render::Theme& OracleTheme() {
  static render::Theme theme;
  return theme;
}

// --- Legacy replicas of the per-surface fill resolution -------------------
// These reproduce the exact inline math the production renderers used before
// the unified builder, so a zero pixel diff proves the builder is faithful.

DecoratedTextFill ResolveRangeFillLayout(const LayoutLine& layout, std::size_t row_start,
                                         std::size_t row_end, float char_width, std::size_t start_col,
                                         std::size_t end_col, SDL_Color color, bool* emitted) {
  const std::size_t start_visual =
      TextLayout::VisualColumnFromLayoutClipped(layout, row_start, row_end, start_col);
  const std::size_t end_visual =
      TextLayout::VisualColumnFromLayoutClipped(layout, row_start, row_end, end_col);
  const std::size_t visible_start = std::max(start_visual, row_start);
  const std::size_t visible_end = std::min(end_visual, row_end);
  *emitted = visible_end > visible_start;
  return DecoratedTextFill{
      .rect = SDL_FRect{kTextX + static_cast<float>(visible_start - row_start) * char_width,
                        kRowY - 1.0f,
                        static_cast<float>(visible_end - visible_start) * char_width, kLineHeight},
      .color = color,
  };
}

DecoratedTextFill ResolveSingleCellLayout(const LayoutLine& layout, std::size_t row_start,
                                          std::size_t row_end, float char_width, std::size_t col,
                                          SDL_Color color, bool* emitted) {
  const std::size_t cell_visual =
      TextLayout::VisualColumnFromLayoutClipped(layout, row_start, row_end, col);
  *emitted = cell_visual >= row_start && cell_visual < row_end;
  return DecoratedTextFill{
      .rect = SDL_FRect{kTextX + static_cast<float>(cell_visual - row_start) * char_width,
                        kRowY - 1.0f, char_width, kLineHeight},
      .color = color,
  };
}

// Drives one comparison: builds the expected row via the legacy formulas and
// the actual row via BuildDecoratedRow, then asserts byte-identical pixels.
void ExpectLayoutRowMatches(const render::TextRenderer& text_renderer, const LineCase& line_case,
                            bool selected, std::optional<std::pair<std::size_t, std::size_t>> selection,
                            std::optional<std::size_t> bracket_column,
                            const std::vector<PublishedDiagnostic>& diagnostics) {
  const render::Theme& theme = OracleTheme();
  const float char_width = text_renderer.CharWidth();
  const std::size_t row_start = line_case.horizontal_scroll;
  const std::size_t row_end = line_case.horizontal_scroll + line_case.visible_columns;
  const LayoutLine layout = TextLayout::BuildVisibleLine(
      line_case.text, line_case.horizontal_scroll, line_case.visible_columns, kTabSize);
  const std::vector<SyntaxTokenKind> tokens;  // plain text: syntax delegated identically
  const SDL_Color plain_color = selected ? theme.text_primary : theme.text_secondary;
  const SDL_Color background_color = theme.row_highlight;
  const DecoratedTextFill background_fill{
      .rect = SDL_FRect{1.0f, kRowY - 1.0f, static_cast<float>(kCanvasWidth) - 2.0f, kLineHeight},
      .color = background_color,
  };

  // Expected: legacy assembly order (background -> selection -> bracket ->
  // syntax -> diagnostics).
  DecoratedTextRow expected;
  std::vector<RowFillSpan> column_fills;
  if (selected) {
    expected.fills.push_back(background_fill);
  }
  if (selection.has_value()) {
    bool emitted = false;
    const DecoratedTextFill fill = ResolveRangeFillLayout(layout, row_start, row_end, char_width,
                                                          selection->first, selection->second,
                                                          theme.selection_fill, &emitted);
    if (emitted) {
      expected.fills.push_back(fill);
    }
    column_fills.push_back(RowFillSpan{.start_column = selection->first,
                                       .end_column = selection->second,
                                       .color = theme.selection_fill,
                                       .geometry = RowFillSpan::Geometry::kRange});
  }
  if (bracket_column.has_value()) {
    bool emitted = false;
    const DecoratedTextFill fill = ResolveSingleCellLayout(
        layout, row_start, row_end, char_width, *bracket_column, theme.bracket_match_background,
        &emitted);
    if (emitted) {
      expected.fills.push_back(fill);
    }
    column_fills.push_back(RowFillSpan{.start_column = *bracket_column,
                                       .end_column = *bracket_column + 1,
                                       .color = theme.bracket_match_background,
                                       .geometry = RowFillSpan::Geometry::kSingleCell});
  }
  editor::AppendLayoutSyntaxTextRuns(expected, text_renderer, theme, kTextX, kRowY, layout,
                                     plain_color, tokens);
  editor::AppendDiagnosticUnderlines(expected, text_renderer, theme, kTextX, kRowY, kLineHeight,
                                     line_case.text, 0, line_case.horizontal_scroll,
                                     line_case.visible_columns, kTabSize,
                                     std::span<const PublishedDiagnostic>(diagnostics));

  // Actual: through the unified builder.
  RowDecorationInput input;
  input.text_x = kTextX;
  input.y = kRowY;
  input.char_width = char_width;
  input.line_height = kLineHeight;
  input.row_visual_start = row_start;
  input.row_visual_end = row_end;
  input.text = &line_case.text;
  input.tokens = &tokens;
  input.plain_color = plain_color;
  input.layout = &layout;
  input.has_background_fill = selected;
  input.background_fill = background_fill;
  input.column_fills = column_fills;
  input.diagnostics = std::span<const PublishedDiagnostic>(diagnostics);
  input.diagnostic_line_index = 0;
  input.diagnostic_horizontal_scroll = line_case.horizontal_scroll;
  input.diagnostic_visible_columns = line_case.visible_columns;
  input.tab_size = kTabSize;
  input.text_renderer = &text_renderer;
  input.theme = &theme;

  DecoratedTextRow actual;
  editor::BuildDecoratedRow(actual, input);

  const std::size_t diff = oracle::RowPixelDifference(text_renderer, expected, actual, kCanvasWidth,
                                                      kCanvasHeight, theme.editor_background);
  Expect(diff == 0,
         std::string("builder layout-path row must paint byte-identically: ") + line_case.label);
}

// Compare/merge visible-window path (layout == null, optional visual map).
void ExpectVisibleRowMatches(const render::TextRenderer& text_renderer, const LineCase& line_case,
                             std::optional<std::pair<std::size_t, std::size_t>> selection,
                             const std::vector<compare::CompareTextSpan>& changed_spans) {
  const render::Theme& theme = OracleTheme();
  const float char_width = text_renderer.CharWidth();
  const std::size_t row_start = line_case.horizontal_scroll;
  const std::size_t row_end = line_case.horizontal_scroll + line_case.visible_columns;
  const TextLayout::LineVisualColumnMap visual_map(line_case.text, kTabSize);
  const std::vector<SyntaxTokenKind> tokens;
  const SDL_Color plain_color = theme.text_secondary;
  const SDL_Color changed_color = theme.diff_modified;
  const DecoratedTextFill background_fill{
      .rect = SDL_FRect{0.0f, kRowY - 1.0f, static_cast<float>(kCanvasWidth), kLineHeight},
      .color = theme.editor_background,
  };

  DecoratedTextRow expected;
  std::vector<RowFillSpan> column_fills;
  expected.fills.push_back(background_fill);
  if (selection.has_value()) {
    const std::size_t start_visual = visual_map.VisualColumnFor(selection->first);
    const std::size_t end_visual = visual_map.VisualColumnFor(selection->second);
    const std::size_t visible_start = std::max(start_visual, row_start);
    const std::size_t visible_end = std::min(end_visual, row_end);
    if (visible_end > visible_start) {
      expected.fills.push_back(DecoratedTextFill{
          .rect = SDL_FRect{kTextX + static_cast<float>(visible_start - row_start) * char_width,
                            kRowY - 1.0f,
                            static_cast<float>(visible_end - visible_start) * char_width,
                            kLineHeight},
          .color = theme.selection_fill,
      });
    }
    column_fills.push_back(RowFillSpan{.start_column = selection->first,
                                       .end_column = selection->second,
                                       .color = theme.selection_fill,
                                       .geometry = RowFillSpan::Geometry::kRange});
  }
  editor::AppendVisibleSyntaxTextRuns(expected, text_renderer, theme, kTextX, kRowY, line_case.text,
                                      line_case.horizontal_scroll, line_case.visible_columns,
                                      plain_color, tokens);
  editor::AppendChangedSpanUnderlines(expected, text_renderer, kTextX, kRowY, kLineHeight,
                                      line_case.text, line_case.horizontal_scroll,
                                      line_case.visible_columns,
                                      std::span<const compare::CompareTextSpan>(changed_spans),
                                      changed_color);

  RowDecorationInput input;
  input.text_x = kTextX;
  input.y = kRowY;
  input.char_width = char_width;
  input.line_height = kLineHeight;
  input.row_visual_start = row_start;
  input.row_visual_end = row_end;
  input.text = &line_case.text;
  input.tokens = &tokens;
  input.plain_color = plain_color;
  input.layout = nullptr;
  input.visual_map = &visual_map;
  input.has_background_fill = true;
  input.background_fill = background_fill;
  input.column_fills = column_fills;
  input.changed_spans = std::span<const compare::CompareTextSpan>(changed_spans);
  input.changed_span_color = changed_color;
  input.text_renderer = &text_renderer;
  input.theme = &theme;

  DecoratedTextRow actual;
  editor::BuildDecoratedRow(actual, input);

  const std::size_t diff = oracle::RowPixelDifference(text_renderer, expected, actual, kCanvasWidth,
                                                      kCanvasHeight, theme.editor_background);
  Expect(diff == 0,
         std::string("builder visible-path row must paint byte-identically: ") + line_case.label);
}

PublishedDiagnostic MakeDiagnostic(std::size_t start_column, std::size_t end_column) {
  PublishedDiagnostic diagnostic;
  diagnostic.severity = editor::DiagnosticSeverity::Warning;
  diagnostic.range.start.line = 0;
  diagnostic.range.start.column = start_column;
  diagnostic.range.end.line = 0;
  diagnostic.range.end.column = end_column;
  return diagnostic;
}

void TestBuilderLayoutPathMatchesLegacyEditorAssembly() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());

  for (const LineCase& line_case : Battery()) {
    // Plain row.
    ExpectLayoutRowMatches(text_renderer, line_case, /*selected=*/false, std::nullopt, std::nullopt,
                           {});
    // Selected row with a selection spanning a window edge + a bracket cell.
    ExpectLayoutRowMatches(text_renderer, line_case, /*selected=*/true,
                           std::make_pair<std::size_t, std::size_t>(2, 9), std::size_t{4}, {});
    // Diagnostic at end of line.
    const std::size_t len = line_case.text.size();
    ExpectLayoutRowMatches(text_renderer, line_case, /*selected=*/false, std::nullopt, std::nullopt,
                           {MakeDiagnostic(len > 4 ? len - 4 : 0, len)});
  }
}

void TestBuilderVisiblePathMatchesLegacyCompareAssembly() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());

  for (const LineCase& line_case : Battery()) {
    ExpectVisibleRowMatches(text_renderer, line_case, std::nullopt, {});
    ExpectVisibleRowMatches(text_renderer, line_case,
                            std::make_pair<std::size_t, std::size_t>(1, 7), {});
    // Changed span crossing the window boundary.
    const std::size_t len = line_case.text.size();
    std::vector<compare::CompareTextSpan> spans = {
        compare::CompareTextSpan{.start = 2, .end = len > 6 ? len - 2 : len}};
    ExpectVisibleRowMatches(text_renderer, line_case, std::nullopt, spans);
  }
}

}  // namespace

void RegisterRowDecorationBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RowDecorationBuilder layout path matches legacy editor row assembly",
          TestBuilderLayoutPathMatchesLegacyEditorAssembly);
  AddTest(tests, "RowDecorationBuilder visible path matches legacy compare row assembly",
          TestBuilderVisiblePathMatchesLegacyCompareAssembly);
}

}  // namespace microide::tests
