#include "workspace/render/EditorRowUnits.h"

#include <algorithm>
#include <string_view>

#include "util/Fnv1a.h"

namespace microide::workspace {
namespace {

struct RowRef {
  std::size_t line = 0;
  std::size_t row = 0;
};

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) { return util::Fnv1aValue(hash, value); }

// Folds `value` into every row showing `line`. `rows` is sorted by line.
template <typename Fold>
void ForRowsOfLine(std::span<const RowRef> rows, std::size_t line, Fold&& fold) {
  auto it = std::lower_bound(rows.begin(), rows.end(), line,
                             [](const RowRef& ref, std::size_t wanted) { return ref.line < wanted; });
  for (; it != rows.end() && it->line == line; ++it) {
    fold(it->row);
  }
}

// Folds a range's part on each line it covers into that line's rows: the
// columns it starts and ends at on the line (an open end runs to the line's end).
template <typename Fold>
void ForRangeLines(std::span<const RowRef> rows, const editor::SelectionRange& range, Fold&& fold) {
  if (rows.empty()) {
    return;
  }
  const bool forward = range.start.line < range.end.line ||
                       (range.start.line == range.end.line && range.start.column <= range.end.column);
  const editor::TextPosition& start = forward ? range.start : range.end;
  const editor::TextPosition& end = forward ? range.end : range.start;
  const std::size_t first = std::max(start.line, rows.front().line);
  const std::size_t last = std::min(end.line, rows.back().line);
  for (std::size_t line = first; line <= last; ++line) {
    const std::uint64_t from = line == start.line ? start.column : 0;
    const std::uint64_t to = line == end.line ? end.column : ~std::uint64_t{0};
    ForRowsOfLine(rows, line, [&](std::size_t row) { fold(row, from, to); });
  }
}

}  // namespace

void AppendEditorRowUnits(const EditorRowKeyInputs& inputs, std::vector<std::uint64_t>& row_keys,
                          std::vector<DerivedDamage::Unit>& units) {
  row_keys.clear();
  if (inputs.viewport == nullptr) {
    return;
  }
  const editor::TextViewport& viewport = *inputs.viewport;
  const editor::EditorViewMetrics& metrics = inputs.metrics;
  thread_local std::vector<RowRef> rows;
  rows.clear();

  // Each row's own place and bytes, exactly the rows the renderer walks.
  const std::size_t scroll = viewport.scroll_line();
  const std::size_t visual_count = viewport.visual_line_count();
  const std::size_t line_count = viewport.line_count();
  const editor::TextBuffer& lines = viewport.lines();
  // An edit that left its carried syntax state alone leaves the lines below it
  // highlighting as before: settled, their tokens are current, and the rows
  // below the edit keep their keys.
  viewport.SettleHighlightAfterLastEdit();
  for (std::size_t row = 0; row < metrics.visible_rows; ++row) {
    const std::size_t visual = scroll + row;
    if (visual >= visual_count) {
      break;
    }
    const auto meta = viewport.WrappedVisualRowLayout(visual);
    if (meta.line_index >= line_count) {
      break;
    }
    std::uint64_t key = Mix(inputs.pane_key, meta.line_index);
    key = Mix(key, meta.visual_start);
    key = Mix(key, meta.visual_end);
    key = Mix(key, meta.indent);
    const std::string_view text = lines.LineView(meta.line_index);
    key = Mix(key, text.size());
    key = text.size() <= kEditorRowKeyMaxHashedBytes ? util::Fnv1aBytes(key, text)
                                                     : Mix(key, viewport.content_revision());
    // The row's syntax colours. A line whose tokens are not current is tokenized
    // here, which the row's repaint would do anyway; a settled edit leaves that
    // to its own lines.
    const std::vector<editor::SyntaxTokenKind>& tokens = viewport.HighlightedLineTokens(meta.line_index);
    key = Mix(key, tokens.size());
    key = util::Fnv1aBytes(key, std::string_view(reinterpret_cast<const char*>(tokens.data()),
                                                 tokens.size() * sizeof(editor::SyntaxTokenKind)));
    // Line numbers right-align to the widest one: a row's gutter text moves when
    // the document gains a digit, which the gutter width (in the pane key) tracks.
    if (meta.visual_start == 0 && inputs.folds != nullptr) {
      const bool opens = inputs.folds->FoldStartingAt(meta.line_index).has_value();
      key = Mix(key, opens ? (inputs.folds->IsCollapsedAtOpener(meta.line_index) ? 3u : 2u) : 1u);
    }
    row_keys.push_back(key);
    rows.push_back(RowRef{.line = meta.line_index, .row = row});
  }
  const std::span<const RowRef> by_line(rows);
  const auto fold_into = [&](std::size_t row, std::uint64_t tag, std::uint64_t a, std::uint64_t b) {
    row_keys[row] = Mix(Mix(Mix(row_keys[row], tag), a), b);
  };

  // The carets and selections on each row.
  ForRowsOfLine(by_line, viewport.cursor_line(),
                [&](std::size_t row) { fold_into(row, 0xCA, viewport.cursor_column(), 0); });
  if (const auto selection = viewport.selection_range(); selection.has_value()) {
    ForRangeLines(by_line, *selection,
                  [&](std::size_t row, std::uint64_t from, std::uint64_t to) { fold_into(row, 0x5E, from, to); });
  }
  for (const auto& caret : viewport.secondary_caret_range_view()) {
    ForRowsOfLine(by_line, caret.position.line,
                  [&](std::size_t row) { fold_into(row, 0xCB, caret.position.column, 0); });
    if (caret.selection_anchor.has_value()) {
      ForRangeLines(by_line, editor::SelectionRange{.start = *caret.selection_anchor, .end = caret.position},
                    [&](std::size_t row, std::uint64_t from, std::uint64_t to) { fold_into(row, 0x5F, from, to); });
    }
  }
  // Marks laid over the text.
  for (const editor::PublishedDiagnostic& diagnostic : inputs.diagnostics) {
    ForRangeLines(by_line, diagnostic.range, [&](std::size_t row, std::uint64_t from, std::uint64_t to) {
      fold_into(row, 0xD0 + static_cast<std::uint64_t>(diagnostic.severity), from, to);
    });
  }
  for (const auto& [line, hash] : inputs.occurrence_lines) {
    ForRowsOfLine(by_line, line, [&](std::size_t row) { fold_into(row, 0x0C, hash, 0); });
  }
  for (const auto& [line, hash] : inputs.extra_line_keys) {
    ForRowsOfLine(by_line, line, [&](std::size_t row) { fold_into(row, 0xE0, hash, 0); });
  }
  if (inputs.bracket_pair.has_value()) {
    const editor::BracketMatchPair& pair = *inputs.bracket_pair;
    ForRowsOfLine(by_line, pair.open_line, [&](std::size_t row) { fold_into(row, 0xB0, pair.open_column, 0); });
    ForRowsOfLine(by_line, pair.close_line, [&](std::size_t row) { fold_into(row, 0xB1, pair.close_column, 0); });
  }
  if (inputs.active_search_match.has_value()) {
    ForRangeLines(by_line, *inputs.active_search_match,
                  [&](std::size_t row, std::uint64_t from, std::uint64_t to) { fold_into(row, 0xF1, from, to); });
  }
  if (inputs.blame != nullptr && inputs.blame->visible) {
    for (const editor::EditorBlameLine& blame : inputs.blame->lines) {
      const std::uint64_t text = util::Fnv1aBytes(util::kFnv1aOffsetBasis, blame.text);
      ForRowsOfLine(by_line, blame.line_index, [&](std::size_t row) { fold_into(row, 0xB7, text, 0); });
    }
  }

  // The units: the pane's head above everything else, a row band each, the sticky
  // band above them, and the empty stretch below the document's last row. The head
  // is the few pixels between the pane's top and its first row, which the gutter's
  // separator crosses: a widened gutter moved it there too (qa doc-comment-enter).
  const float pane_bottom = inputs.pane_rect.y + inputs.pane_rect.h;
  const float band_height = metrics.first_line_y - metrics.sticky_band_top_y;
  const bool sticky_band = metrics.sticky_scroll_rows > 0 && band_height > 0.0f;
  const float head_bottom = std::min(sticky_band ? metrics.sticky_band_top_y : metrics.first_line_y, pane_bottom);
  if (head_bottom > inputs.pane_rect.y) {
    units.push_back({SDL_FRect{inputs.pane_rect.x, inputs.pane_rect.y, inputs.pane_rect.w,
                               head_bottom - inputs.pane_rect.y},
                     Mix(inputs.pane_key, 0x4EAD)});
  }
  if (sticky_band) {
    units.push_back({SDL_FRect{inputs.pane_rect.x, metrics.sticky_band_top_y, inputs.pane_rect.w, band_height},
                     Mix(inputs.pane_key, 0x57)});
  }
  if (inputs.rows_have_gaps) {
    std::uint64_t all = Mix(inputs.pane_key, row_keys.size());
    for (const std::uint64_t key : row_keys) {
      all = Mix(all, key);
    }
    units.push_back({SDL_FRect{inputs.pane_rect.x, metrics.first_line_y, inputs.pane_rect.w,
                               std::max(0.0f, pane_bottom - metrics.first_line_y)},
                     all});
    return;
  }
  for (std::size_t row = 0; row < row_keys.size(); ++row) {
    const float top = metrics.first_line_y + static_cast<float>(row) * metrics.line_height;
    const float height = std::min(metrics.line_height, pane_bottom - top);
    units.push_back({SDL_FRect{inputs.pane_rect.x, top, inputs.pane_rect.w, std::max(0.0f, height)}, row_keys[row]});
  }
  const float tail_top = metrics.first_line_y + static_cast<float>(row_keys.size()) * metrics.line_height;
  units.push_back({SDL_FRect{inputs.pane_rect.x, tail_top, inputs.pane_rect.w, std::max(0.0f, pane_bottom - tail_top)},
                   Mix(inputs.pane_key, 0x7A11)});
}

}  // namespace microide::workspace
