#pragma once

#include <utility>
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "util/StringUtil.h"

namespace microide::editor {

struct LayoutLine {
  std::string text;
  std::vector<std::size_t> source_columns;
  std::vector<std::size_t> text_offsets;
  std::size_t visual_columns = 0;
  std::size_t caret_column = 0;
  bool caret_visible = false;
};

// What a caller already knows about a line, so `TextLayout::BuildVisibleLineInto`
// does not re-derive it. Both facts cost O(line) to compute and both are already
// maintained per line by `TextLayoutCache`, which is the hot caller.
//
// Without them a visible-row build is O(line) twice over on a line with no
// newlines in it (a minified bundle): once to measure the full width for the
// scrollbar and end-of-line decorations, and once to step code points from column
// 0 up to the horizontal scroll offset to find the first visible cell. With the
// caret a megabyte in, that is a megabyte of decoding per rendered row, per frame
// (TD-2026-08-05-132).
struct LineLayoutFacts {
  // The line's full visual width. Read only when `known`.
  std::size_t visual_columns = 0;
  // Every byte of the line is a plain single-cell ASCII character: no tab, no
  // byte >= 0x80. Visual column then equals byte column at every offset on the
  // line, which is what lets the builder jump straight to the first visible cell
  // instead of walking to it. Read only when `known`.
  bool plain_ascii = false;
  bool known = false;
  // `leading_tabs` tabs and then only plain single-cell ASCII: every indented
  // line of a tab-indented file. Column conversions are then arithmetic
  // (`leading_tabs * tab_size` cells of indentation, one cell per byte after).
  // Read only when `known`; false whenever the run does not fit the table.
  bool tab_indented = false;
  std::size_t leading_tabs = 0;
};

class TextLayout {
 public:
  // True when every byte offset in `line` IS its visual column — no tab (which
  // expands to a stop) and no byte >= 0x80 (which begins a multi-byte code point).
  // The identity mapping is then exact, so a caller that would build a
  // `LineVisualColumnMap` to answer two or three queries can skip it: that is two
  // heap vectors and an O(line) fill per row, and source lines are overwhelmingly
  // this shape (TD-2026-08-06-159).
  static bool VisualColumnsAreIdentity(std::string_view line);

  static std::size_t VisualColumnForTextColumn(std::string_view line,
                                               std::size_t text_column,
                                               std::size_t tab_size);
  // Visual column reached by walking `text` starting from `visual_column`. Lets a
  // caller that already knows the visual column at some byte offset extend it over
  // a run appended there, instead of composing the joined string and re-walking
  // its whole prefix.
  static std::size_t AdvanceVisualColumnsOver(std::size_t visual_column,
                                              std::string_view text,
                                              std::size_t tab_size);
  static std::size_t TextColumnForVisualColumn(std::string_view line,
                                               std::size_t visual_column,
                                               std::size_t tab_size);
  static std::size_t ClampTextColumn(std::string_view line, std::size_t text_column);
  static std::size_t PreviousTextColumn(std::string_view line, std::size_t text_column);
  static std::size_t NextTextColumn(std::string_view line, std::size_t text_column);
  // Builds into a caller-owned LayoutLine, reusing its `text`, `source_columns`
  // and `text_offsets` capacity. Scrolling through fresh content misses the
  // visible-line cache on every row, and each miss otherwise allocates all three
  // buffers (~3.4 KB for a 200-column window) and then frees them again a few
  // hundred rows later when the entry is evicted.
  static void BuildVisibleLineInto(std::string_view line,
                                   std::size_t horizontal_scroll,
                                   std::size_t visible_columns,
                                   std::size_t tab_size,
                                   LayoutLine& out,
                                   LineLayoutFacts facts = {});

  // A line handed to the builder as a bounded byte window instead of whole.
  //
  // Rendering a row reads a screenful of bytes, but asking for them as
  // `lines[index]` asks for the whole line -- and on a piece-tree source that
  // materializes a copy of any line that spans pieces, i.e. every line an in-line
  // edit has touched. On a file with no line breaks in it that is a multi-megabyte
  // copy per frame (TD-2026-08-05-133).
  //
  // `bytes` are the line's bytes beginning at absolute byte column `start_byte`,
  // which must be a position where visual column still equals byte column -- i.e.
  // no tab and no byte >= 0x80 precedes it. That is what lets the walk start there
  // instead of at column 0, and it is the same precondition the whole-line build
  // already relies on. `ComputeVisibleLineWindowStart` is the one place that
  // picks it.
  struct VisibleLineWindow {
    std::string_view bytes;
    std::size_t start_byte = 0;
    std::size_t line_length = 0;
  };

  // First byte the visible walk can start at, given what is known about the line.
  // `plain_prefix_end` is the line's first tab-or-multibyte byte (`line_length`
  // when there is none), which the caller scans for -- possibly in chunks, since
  // that scan is itself bounded by `horizontal_scroll`.
  static std::size_t ComputeVisibleLineWindowStart(std::size_t horizontal_scroll,
                                                   std::size_t line_length,
                                                   std::size_t plain_prefix_end) {
    if (horizontal_scroll == 0 || line_length == 0) {
      return 0;
    }
    return std::min({horizontal_scroll, line_length, plain_prefix_end});
  }

  // Bytes the visible walk can read past `start_byte`, and therefore the smallest
  // window that is guaranteed to hold everything it visits. The walk stops once
  // the visual column reaches `horizontal_scroll + visible_columns`, and every
  // step advances that by at least one while consuming at most four bytes (the
  // longest UTF-8 sequence); the trailing `+ 4` covers the step that crosses the
  // edge. Clamping to the line is the caller's job (LineWindow does it).
  static std::size_t VisibleLineWindowBytes(std::size_t start_byte,
                                            std::size_t horizontal_scroll,
                                            std::size_t visible_columns) {
    const std::size_t reach = horizontal_scroll > start_byte ? horizontal_scroll - start_byte : 0;
    return (reach + visible_columns) * 4 + 4;
  }

  // Window form of `BuildVisibleLineInto`. `facts` must be known: the line's full
  // visual width cannot be derived from a window, and the caller that has a window
  // has a width table (TextLayoutCache).
  static void BuildVisibleLineWindowInto(const VisibleLineWindow& window,
                                         std::size_t horizontal_scroll,
                                         std::size_t visible_columns,
                                         std::size_t tab_size,
                                         LayoutLine& out,
                                         LineLayoutFacts facts);
  static LayoutLine BuildVisibleLine(std::string_view line,
                                     std::size_t horizontal_scroll,
                                     std::size_t visible_columns,
                                     std::size_t tab_size,
                                     LineLayoutFacts facts = {});

  // Measures both `LineLayoutFacts` in one pass. The plain-ASCII prefix scan that
  // `VisualColumnForTextColumn` already runs is exactly the question
  // `plain_ascii` asks, so measuring both together costs no more than measuring
  // the width alone.
  static LineLayoutFacts MeasureLineFacts(std::string_view line, std::size_t tab_size);

  // O(N) prefix walk that captures every text-byte boundary's visual column for `line`. Once
  // built, `VisualColumnFor(text_column)` is O(log N). Use when more than one VisualColumn query
  // hits the same line within a frame (compare/merge row render, hover target geometry) so the
  // per-line tab/UTF-8 walk runs once instead of once per query.
  class LineVisualColumnMap {
   public:
    LineVisualColumnMap(std::string_view line, std::size_t tab_size);
    std::size_t VisualColumnFor(std::size_t text_column) const;
    std::size_t LineVisualWidth() const { return line_visual_width_; }

   private:
    // Parallel arrays: boundaries_[i] is a text byte offset (in monotonic order); visuals_[i] is
    // the visual column at that boundary. Both include 0 and line.size() as endpoints.
    std::vector<std::size_t> boundaries_;
    std::vector<std::size_t> visuals_;
    std::size_t line_visual_width_ = 0;
  };

  // Resolve a source byte column to its visual column using an already-built `LayoutLine`. This
  // avoids the O(line_length) tab-stop walk that VisualColumnForTextColumn performs.
  //
  // The layout describes only the visible cells `[row_start_visual, row_end_visual)`. For source
  // columns outside that window the returned value is intentionally beyond the window so callers
  // that clip via `std::max(start, row_start_visual)` / `std::min(end, row_end_visual)` get the
  // correct clipped value without a fallback. Specifically:
  //   - if `source_column` precedes the leftmost visible source byte: returns
  //     `row_start_visual > 0 ? row_start_visual - 1 : 0` (safe lower-bound sentinel).
  //   - if `source_column` is past the last visible cell: returns `row_end_visual + 1`.
  //   - otherwise: returns `row_start_visual + cell_index` where cell_index is the first cell
  //     in `layout.source_columns` with `source_column[cell_index] >= source_column`.
  static std::size_t VisualColumnFromLayoutClipped(const LayoutLine& layout,
                                                   std::size_t row_start_visual,
                                                   std::size_t row_end_visual,
                                                   std::size_t source_column);

  // Resolve a source byte column to its visual column using whichever mapper the caller has for
  // the row: the cached cell-grid `layout` (clipped to `[row_visual_start, row_visual_end)`), else
  // the prebuilt per-line `visual_map`, else identity (source column == visual column). This is the
  // single mapper the row-decoration builder and the inlay-hint column resolver both call, so
  // inlay hints land on exactly the same visual grid the text, fills, selections and diagnostics do.
  static std::size_t ResolveVisualColumn(const LayoutLine* layout,
                                         const LineVisualColumnMap* visual_map,
                                         std::size_t row_visual_start,
                                         std::size_t row_visual_end,
                                         std::size_t source_column);

  // Visual column after advancing past `character` from `visual_column`. Tabs advance to the next
  // multiple of `tab_size`; every other byte advances by one cell. Defined inline: this is the one
  // authoritative tab-stop/width step shared by every visual-column walk in the editor (the layout
  // walks in this class, the wrapped-row builder, and the whitespace-marker / indent-guide render
  // paths), so hot per-codepoint loops keep it zero-cost while there is exactly one implementation.
  static std::size_t AdvanceVisualColumn(std::size_t visual_column,
                                         char character,
                                         std::size_t tab_size) {
    if (character != '\t') {
      return visual_column + 1;
    }
    const std::size_t safe_tab_size = std::max<std::size_t>(1, tab_size);
    const std::size_t remainder = visual_column % safe_tab_size;
    return visual_column + (remainder == 0 ? safe_tab_size : safe_tab_size - remainder);
  }

  // Soft-wrap a single logical line into visual rows, emitting each one as
  // `emit(visual_start, visual_end, indent)` — a half-open visual-column span plus
  // the hanging indent a continuation row renders under. Always emits at least one
  // row, even for an empty line. `wrap_columns` must already be clamped to >= 1.
  //
  // The one wrap implementation in the tree: the editor's wrapped-row table and the
  // compare/merge diff surfaces both go through it, so a break decision cannot
  // differ between a file opened in the editor and the same file opened in a diff.
  // A template over the sink rather than a vector out-param so neither caller pays
  // a per-line copy through shared scratch.
  template <typename EmitFn>
  static void WrapLineSegments(std::string_view line_text,
                               std::size_t tab_size,
                               std::size_t wrap_columns,
                               EmitFn&& emit) {
    if (line_text.empty()) {
      emit(std::size_t{0}, std::size_t{0}, std::size_t{0});
      return;
    }
    WrapLineSegmentsFrom(line_text, tab_size, wrap_columns, /*visual_base=*/0,
                         HangingIndentFor(line_text, tab_size, wrap_columns),
                         std::forward<EmitFn>(emit));
  }

  // Hanging indent: continuation rows of a line render aligned under the line's
  // leading whitespace, clamped so a deep indent never collapses continuation
  // rows to a degenerate width. Exposed because the resume form below cannot
  // derive it -- a tail does not contain the line's leading whitespace.
  static std::size_t HangingIndentFor(std::string_view line_text,
                                      std::size_t tab_size,
                                      std::size_t wrap_columns) {
    std::size_t indent_visual = 0;
    for (std::size_t k = 0; k < line_text.size(); ++k) {
      const char ch = line_text[k];
      if (ch != ' ' && ch != '\t') {
        break;
      }
      indent_visual = AdvanceVisualColumn(indent_visual, ch, tab_size);
    }
    return std::min(indent_visual, wrap_columns / 2);
  }

  // Resume form: wrap `tail`, which is the part of a line beginning at visual
  // column `visual_base`, for a line whose hanging indent is `hanging_indent`.
  //
  // Restarting here reproduces exactly the rows the whole-line wrap would emit
  // from this point, PROVIDED `visual_base` is a row boundary: the wrap is a
  // left-to-right greedy pass whose only carried state is the current row's start
  // (visual and text), the last break opportunity within it, and the running
  // visual column -- all of which are reset at a row boundary. `visual_base` is
  // threaded through the visual arithmetic rather than assumed zero so a tab in
  // the tail still expands against its ABSOLUTE column, which is the one thing a
  // naive "wrap the substring" would get wrong.
  //
  // Callers must supply a real row boundary. The incremental splice in
  // TextLayoutCache cross-checks the whole table against a full re-wrap under
  // NDEBUG-off builds, which is what makes that a checked claim.
  template <typename EmitFn>
  static void WrapLineSegmentsFrom(std::string_view tail,
                                   std::size_t tab_size,
                                   std::size_t wrap_columns,
                                   std::size_t visual_base,
                                   std::size_t hanging_indent,
                                   EmitFn&& emit) {
    const std::string_view line_text = tail;
    if (line_text.empty()) {
      emit(visual_base, visual_base, visual_base == 0 ? std::size_t{0} : hanging_indent);
      return;
    }

    // Single pass: walk the line tracking the visual column, the last whitespace
    // break opportunity, and the current row's text start. Break before any
    // character that would push the row past the row's available width; prefer
    // breaking after the most recent whitespace if one is available inside the
    // current row. Hard-break inside a long word only when no whitespace fits.
    // Continuation rows (row_start_visual > 0) lose `hanging_indent` columns of
    // width to the rendered indent.
    std::size_t row_start_visual = visual_base;
    std::size_t row_start_text = 0;
    std::size_t last_break_visual = visual_base;
    std::size_t last_break_text = 0;
    std::size_t visual = visual_base;
    std::size_t i = 0;
    const std::size_t line_size = line_text.size();
    while (i < line_size) {
      const char ch = line_text[i];
      const std::size_t seq_len = util::Utf8SequenceLength(line_text, i);
      const std::size_t next_visual = AdvanceVisualColumn(visual, ch, tab_size);
      const std::size_t effective_wrap =
          wrap_columns - (row_start_visual == 0 ? 0 : hanging_indent);

      if (next_visual - row_start_visual > effective_wrap && i > row_start_text) {
        std::size_t break_visual;
        std::size_t break_text;
        if (last_break_text > row_start_text) {
          break_visual = last_break_visual;
          break_text = last_break_text;
        } else {
          break_visual = visual;
          break_text = i;
        }
        emit(row_start_visual, break_visual, row_start_visual == 0 ? std::size_t{0} : hanging_indent);
        row_start_visual = break_visual;
        row_start_text = break_text;
        last_break_visual = row_start_visual;
        last_break_text = row_start_text;
        visual = break_visual;
        i = break_text;
        continue;
      }

      visual = next_visual;
      i += seq_len;
      if (ch == ' ' || ch == '\t') {
        last_break_visual = visual;
        last_break_text = i;
      }
    }
    emit(row_start_visual, visual, row_start_visual == 0 ? std::size_t{0} : hanging_indent);
  }

  // Number of visual rows `WrapLineSegments` would emit for this line. Kept beside
  // it so the two cannot disagree; used where only the row budget is needed (the
  // diff surfaces' per-row max across panes).
  static std::size_t WrapLineRowCount(std::string_view line_text,
                                      std::size_t tab_size,
                                      std::size_t wrap_columns) {
    std::size_t rows = 0;
    WrapLineSegments(line_text, tab_size, wrap_columns,
                     [&rows](std::size_t, std::size_t, std::size_t) { ++rows; });
    return rows;
  }

  // A half-open byte range [start, end) within a line. Empty when start >= end.
  struct ByteRange {
    std::size_t start = 0;
    std::size_t end = 0;
    bool empty() const { return start >= end; }
  };

  // Identifier range containing the byte at `text_column`, or an empty range when that byte is
  // not an identifier character (whitespace, punctuation, past end-of-line). The core run is
  // `[A-Za-z0-9_]+`; the range is then extended leftward across member-access operators (`.` and
  // `->`) so a hover on the trailing member of `foo.bar`, `ptr->field`, or a nested `a.b.c` chain
  // resolves the full member expression for `evaluate`. Used by debug hover-to-inspect. Identifier
  // bytes are ASCII, so multibyte UTF-8 bytes (>= 0x80) terminate every run and bound the chain.
  // Only leftward member access is absorbed; the range never extends rightward past the hovered
  // member, keeping the boundary deterministic.
  static ByteRange IdentifierRangeAt(std::string_view line, std::size_t text_column);
};

}  // namespace microide::editor
