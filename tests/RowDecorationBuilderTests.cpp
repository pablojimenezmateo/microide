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
  input.text = line_case.text;
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
  input.text = line_case.text;
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

// When the row renders on the cell grid (layout set), changed-span underlines must
// land on tab-expanded grid columns (visual * char_width), aligned with the grid
// text/caret/selection — not on the proportional MeasureWidth positions. A leading
// tab makes the two models produce different x, so this pins the grid geometry.
void TestChangedSpanUnderlinesUseGridWhenLayoutSet() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());
  const render::Theme& theme = OracleTheme();

  const std::string text = "\treturn x;";  // '\t' + "return x;"
  const LayoutLine layout = TextLayout::BuildVisibleLine(text, 0, 40, kTabSize);
  const std::vector<SyntaxTokenKind> tokens;
  // Changed span covers bytes [1, 7) == "return"; byte 1 sits at visual column 4
  // (the tab expands to kTabSize=4 cells), byte 7 at visual column 10.
  const std::vector<compare::CompareTextSpan> spans = {compare::CompareTextSpan{.start = 1, .end = 7}};

  RowDecorationInput input;
  input.text_x = 10.0f;
  input.y = 5.0f;
  input.char_width = 8.0f;
  input.line_height = 16.0f;
  input.row_visual_start = 0;
  input.row_visual_end = 40;
  input.text = text;
  input.tokens = &tokens;
  input.plain_color = theme.text_secondary;
  input.layout = &layout;  // grid path
  input.changed_spans = std::span<const compare::CompareTextSpan>(spans);
  input.changed_span_color = SDL_Color{200, 40, 40, 255};
  input.text_renderer = &text_renderer;
  input.theme = &theme;

  DecoratedTextRow row;
  editor::BuildDecoratedRow(row, input);

  Expect(row.underlines.size() == 1, "one changed-span underline should be emitted");
  const SDL_FRect& rect = row.underlines.front().rect;
  // Grid: start at visual col 4 -> x = 10 + 4*8 = 42; width = 6 cells -> 48.
  Expect(rect.x == 10.0f + 4.0f * 8.0f, "underline must start at the tab-expanded grid column");
  Expect(rect.w == 6.0f * 8.0f, "underline width must span the changed cells on the grid");
  // The appender must push the source color at FULL intensity: RenderRow applies the
  // single 0.55 dim at draw time, so a pre-dim here would fade diff underlines twice.
  Expect(row.underlines.front().color.a == 255,
         "changed-span underline must carry full source alpha (RenderRow dims once)");
}

// When the row renders on the cell grid (layout set), plugin text-style under/strike
// lines must land on tab-expanded grid columns (visual * char_width), matching the
// grid text — not on the proportional MeasureWidth positions. A leading tab makes the
// two models diverge, so this pins the grid geometry for the text-style path (the
// changed-span sibling above pins the same for compare's changed spans).
void TestTextStyleUnderlinesUseGridWhenLayoutSet() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());
  const render::Theme& theme = OracleTheme();

  const std::string text = "\treturn x;";  // '\t' + "return x;"
  const LayoutLine layout = TextLayout::BuildVisibleLine(text, 0, 40, kTabSize);
  const std::vector<SyntaxTokenKind> tokens;
  // Underline covers bytes [1, 7) == "return"; byte 1 is at visual column 4 (the tab
  // expands to kTabSize=4 cells), byte 7 at visual column 10.
  std::vector<editor::TextStyleDecoration> text_styles;
  editor::TextStyleDecoration ul;
  ul.line = 0;
  ul.start_column = 1;
  ul.end_column = 7;
  ul.line_color = SDL_Color{200, 40, 40, 255};
  ul.flags = editor::kDecorationUnderline;
  text_styles.push_back(ul);

  RowDecorationInput input;
  input.text_x = 10.0f;
  input.y = 5.0f;
  input.char_width = 8.0f;
  input.line_height = 16.0f;
  input.row_visual_start = 0;
  input.row_visual_end = 40;
  input.text = text;
  input.tokens = &tokens;
  input.plain_color = theme.text_secondary;
  input.layout = &layout;  // grid path
  input.text_styles = text_styles;
  input.text_renderer = &text_renderer;
  input.theme = &theme;

  DecoratedTextRow row;
  editor::BuildDecoratedRow(row, input);

  Expect(row.underlines.size() == 1, "one text-style underline should be emitted");
  const SDL_FRect& rect = row.underlines.front().rect;
  // Grid: start at visual col 4 -> x = 10 + 4*8 = 42; width = 6 cells -> 48. The old
  // proportional path put it at x = 10 + MeasureWidth("\t") = 18 (one cell), width 48.
  Expect(rect.x == 10.0f + 4.0f * 8.0f, "underline must start at the tab-expanded grid column");
  Expect(rect.w == 6.0f * 8.0f, "underline width must span the styled cells on the grid");
}

// A mid-line inlay hint must (a) split and shift the real text runs to its right
// by its whole-cell width, (b) leave text to its left untouched, and (c) emit its
// own glyph run in the reserved phantom cells. Backend-less TextRenderer => 8px
// cells and MeasureWidth == 8*len, so the geometry below is exact.
void TestInlayHintsShiftRunsAndDrawGlyph() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());
  const render::Theme& theme = OracleTheme();
  const float char_width = text_renderer.CharWidth();  // 8
  Expect(char_width == 8.0f, "backend-less cell width is 8px");

  const std::string text = "let x = 5;";  // col 5 is the space after 'x'
  const LayoutLine layout = TextLayout::BuildVisibleLine(text, 0, 40, kTabSize);
  const std::vector<SyntaxTokenKind> tokens;  // one plain color -> one run without hints

  // Type hint " i32" (4 glyphs => 4 cells) anchored before column 5.
  std::vector<editor::InlineTextDecoration> inline_texts;
  editor::InlineTextDecoration hint;
  hint.anchor_column = 5;
  hint.text = " i32";
  hint.color = SDL_Color{120, 120, 120, 255};
  inline_texts.push_back(hint);

  std::vector<editor::InlayCellSpan> spans;
  std::size_t total_cells = 0;
  editor::BuildInlayRowSpans(inline_texts, &layout, nullptr, 0, 40, text_renderer, char_width, spans,
                             &total_cells);
  Expect(spans.size() == 1, "one mid-line hint span");
  Expect(spans.front().anchor_visual_column == 5, "anchor at visual column 5");
  Expect(spans.front().cell_width == 4, "\" i32\" occupies 4 cells");
  Expect(total_cells == 4, "total line phantom cells");

  // A plugin/semantic underline on bytes [6, 10) ("= 5;"), i.e. to the RIGHT of the
  // hint, must follow the shifted glyphs (uniform underline post-pass).
  std::vector<editor::TextStyleDecoration> text_styles;
  editor::TextStyleDecoration ul;
  ul.line = 0;
  ul.start_column = 6;
  ul.end_column = 10;
  ul.line_color = SDL_Color{200, 40, 40, 255};
  ul.flags = editor::kDecorationUnderline;
  text_styles.push_back(ul);

  const float text_x = 10.0f;
  RowDecorationInput input;
  input.text_x = text_x;
  input.y = 5.0f;
  input.char_width = char_width;
  input.line_height = 16.0f;
  input.row_visual_start = 0;
  input.row_visual_end = 40;
  input.text = text;
  input.tokens = &tokens;
  input.plain_color = theme.text_secondary;
  input.layout = &layout;
  input.text_styles = text_styles;
  input.inlay = editor::InlayRowDisplacement(spans);
  input.inlay_inline_texts = inline_texts;
  input.inlay_color = theme.text_disabled;
  input.text_renderer = &text_renderer;
  input.theme = &theme;

  DecoratedTextRow row;
  editor::BuildDecoratedRow(row, input);

  // Locate the three expected runs by their text.
  const editor::DecoratedTextRun* left = nullptr;
  const editor::DecoratedTextRun* right = nullptr;
  const editor::DecoratedTextRun* glyph = nullptr;
  for (const editor::DecoratedTextRun& run : row.runs) {
    if (run.text == "let x") left = &run;
    else if (run.text == " = 5;") right = &run;
    else if (run.text == " i32") glyph = &run;
  }
  Expect(left != nullptr, "unshifted run left of the hint is emitted");
  Expect(right != nullptr, "run right of the hint is emitted");
  Expect(glyph != nullptr, "the hint glyph run is emitted");
  // Left run stays at the origin; right run shifts by 4 cells (32px); the hint
  // sits in the 4 phantom cells between them (display cols 5..8 -> x=text_x+40).
  Expect(left->x == text_x, "text left of the hint is not shifted");
  Expect(glyph->x == text_x + 5.0f * char_width, "hint draws at the phantom region start");
  Expect(right->x == text_x + (5.0f + 4.0f) * char_width,
         "text right of the hint shifts by the hint's cell width");

  // The underline starts at byte 6 (visual col 6, right of the hint at 5) and must
  // shift by the hint's 4 cells: base x = text_x + 6*8, shifted x = + 4*8.
  Expect(row.underlines.size() == 1, "one underline emitted");
  Expect(row.underlines.front().rect.x == text_x + (6.0f + 4.0f) * char_width,
         "an underline right of the hint follows the shifted glyphs");
}

// Regression: AppendDiagnosticUnderlines builds a per-line visual-column map ONCE
// and reuses it for every diagnostic. Its rects must stay byte-identical to the
// per-diagnostic DiagnosticUnderlineRect (which does the uncached tab-stop walk),
// including on a line with tabs and a multibyte char where visual != byte column.
void TestDiagnosticUnderlineCacheMatchesUncachedPath() {
  oracle::EnsureDummyVideo();
  oracle::OracleCanvas init_canvas(kCanvasWidth, kCanvasHeight);
  render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(init_canvas.renderer());

  // Tabs make visual columns diverge from byte columns; the trailing "é" (2 bytes)
  // exercises the code-point-boundary snap in the cached lookup.
  const std::string line_text = "\tab\tcde\tfghïj";
  const std::size_t len = line_text.size();
  constexpr std::size_t kScroll = 0;
  constexpr std::size_t kVisibleColumns = 200;
  const std::vector<PublishedDiagnostic> diagnostics = {
      MakeDiagnostic(0, 3),  MakeDiagnostic(4, 7),  MakeDiagnostic(1, len),
      MakeDiagnostic(7, len), MakeDiagnostic(3, 4),
  };

  const render::Theme& theme = OracleTheme();
  DecoratedTextRow row;
  editor::AppendDiagnosticUnderlines(row, text_renderer, theme, kTextX, kRowY, kLineHeight,
                                     line_text, 0, kScroll, kVisibleColumns, kTabSize,
                                     std::span<const PublishedDiagnostic>(diagnostics));

  std::vector<SDL_FRect> expected;
  for (const PublishedDiagnostic& diagnostic : diagnostics) {
    if (const auto rect = editor::DiagnosticUnderlineRect(text_renderer, kTextX, kRowY, kLineHeight,
                                                          line_text, 0, kScroll, kVisibleColumns,
                                                          kTabSize, diagnostic);
        rect.has_value()) {
      expected.push_back(*rect);
    }
  }

  Expect(row.underlines.size() == expected.size(),
         "cached AppendDiagnosticUnderlines must emit the same number of underlines");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const SDL_FRect& a = row.underlines[i].rect;
    const SDL_FRect& b = expected[i];
    Expect(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h,
           "cached underline rect must byte-match the uncached DiagnosticUnderlineRect");
  }
}

}  // namespace

void RegisterRowDecorationBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RowDecorationBuilder diagnostic underline cache matches uncached path",
          TestDiagnosticUnderlineCacheMatchesUncachedPath);
  AddTest(tests, "RowDecorationBuilder inlay hints shift runs and draw the glyph",
          TestInlayHintsShiftRunsAndDrawGlyph);
  AddTest(tests, "RowDecorationBuilder text-style underlines use the grid under a layout",
          TestTextStyleUnderlinesUseGridWhenLayoutSet);
  AddTest(tests, "RowDecorationBuilder changed-span underlines use the grid under a layout",
          TestChangedSpanUnderlinesUseGridWhenLayoutSet);
  AddTest(tests, "RowDecorationBuilder layout path matches legacy editor row assembly",
          TestBuilderLayoutPathMatchesLegacyEditorAssembly);
  AddTest(tests, "RowDecorationBuilder visible path matches legacy compare row assembly",
          TestBuilderVisiblePathMatchesLegacyCompareAssembly);
}

}  // namespace microide::tests
