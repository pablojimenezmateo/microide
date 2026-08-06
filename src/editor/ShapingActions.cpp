#include "editor/ShapingActions.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

using SecondaryCaret = TextViewportUndoHistory::SecondaryCaret;

struct LineRange {
  std::size_t first = 0;
  std::size_t last = 0;  // inclusive
};

LineRange ResolveLineRange(const TextViewport& viewport) {
  LineRange r;
  r.first = viewport.cursor_line();
  r.last = viewport.cursor_line();

  if (auto sel = viewport.selection_range()) {
    SelectionRange n = sel->start.line <= sel->end.line ? *sel
                                                         : SelectionRange{sel->end, sel->start};
    if (sel->start.line > sel->end.line ||
        (sel->start.line == sel->end.line && sel->start.column > sel->end.column)) {
      n = {sel->end, sel->start};
    }
    r.first = n.start.line;
    r.last = n.end.line;
    if (r.last > r.first && n.end.column == 0) {
      // Selection ends at start of a line; don't include that line.
      --r.last;
    }
  }

  // Cover every line a secondary caret touches, INCLUDING lines spanned only by
  // a ranged caret's selection anchor (A-120) — a Ctrl-D selection whose anchor
  // sits on a different line than its cursor must not be partially missed.
  for (const SecondaryCaret& secondary : viewport.secondary_caret_ranges()) {
    r.first = std::min(r.first, secondary.position.line);
    r.last = std::max(r.last, secondary.position.line);
    if (secondary.selection_anchor.has_value()) {
      r.first = std::min(r.first, secondary.selection_anchor->line);
      r.last = std::max(r.last, secondary.selection_anchor->line);
    }
  }
  if (r.last >= viewport.line_count()) {
    r.last = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
  }
  return r;
}

bool LineIsEmptyOrWhitespace(std::string_view s) {
  for (char c : s) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

std::size_t LeadingWhitespaceCount(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return i;
}

// If `content` — ignoring leading/trailing horizontal whitespace — is already
// wrapped in `open`…`close`, returns the un-wrapped text with the surrounding
// whitespace preserved. Otherwise returns nullopt. This makes ToggleBlockComment
// a true toggle instead of nesting `/* /* x */ */` on repeat.
std::optional<std::string> TryStripBlockComment(std::string_view content,
                                                std::string_view open,
                                                std::string_view close) {
  std::size_t begin = 0;
  while (begin < content.size() && (content[begin] == ' ' || content[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = content.size();
  while (end > begin && (content[end - 1] == ' ' || content[end - 1] == '\t')) {
    --end;
  }
  const std::string_view core = content.substr(begin, end - begin);
  // Require non-overlapping markers so a fragment shorter than both cannot be
  // mistaken for a wrapped block (e.g. `/*` alone must not strip to nothing).
  if (core.size() < open.size() + close.size()) return std::nullopt;
  if (!core.starts_with(open) || !core.ends_with(close)) return std::nullopt;
  const std::string_view inner =
      core.substr(open.size(), core.size() - open.size() - close.size());
  std::string result;
  result.reserve(content.size());
  result.append(content.substr(0, begin));
  result.append(inner);
  result.append(content.substr(end));
  return result;
}

}  // namespace

bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker) {
  if (line_marker.empty()) return false;
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.first >= lines.size()) return false;

  // Determine whether all non-blank lines in range start with the marker
  // (after leading whitespace). If yes, uncomment; otherwise, comment.
  const std::string_view marker = line_marker;
  bool all_commented = true;
  bool any_non_blank = false;
  std::size_t min_indent = std::string::npos;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    // LineView, not lines[i]: LineRef copies the line and interns it in the
    // buffer's line cache, so the two passes below cost two allocations per line
    // before any of the work the toggle actually needs (TD-2026-08-06-159).
    const std::string_view line = lines.LineView(i);
    if (LineIsEmptyOrWhitespace(line)) continue;
    any_non_blank = true;
    std::size_t lead = LeadingWhitespaceCount(line);
    if (lead < min_indent) min_indent = lead;
    if (line.compare(lead, marker.size(), marker) != 0) {
      all_commented = false;
    }
  }
  if (!any_non_blank) return false;
  if (min_indent == std::string::npos) min_indent = 0;

  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 1);
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string_view line = lines.LineView(i);
    if (LineIsEmptyOrWhitespace(line)) {
      updated.emplace_back(line);
      continue;
    }
    if (all_commented) {
      // Strip first occurrence of marker after leading whitespace. Built into one
      // exactly-sized buffer: the previous form took a `substr` for the head, a
      // second for the tail, and grew the head to fit it -- three allocations to
      // produce one string.
      std::size_t lead = LeadingWhitespaceCount(line);
      std::size_t pos = lead + marker.size();
      // Strip a single space after marker if present (common style).
      if (pos < line.size() && line[pos] == ' ') ++pos;
      std::string out;
      out.reserve(lead + (line.size() - pos));
      out.append(line, 0, lead);
      out.append(line, pos, line.size() - pos);
      updated.push_back(std::move(out));
    } else {
      std::string out;
      out.reserve(line.size() + marker.size() + 1);
      out.append(line, 0, min_indent);
      out.append(marker);
      out.push_back(' ');
      out.append(line.begin() + static_cast<std::ptrdiff_t>(min_indent), line.end());
      updated.push_back(std::move(out));
    }
  }
  return viewport.ReplaceLines(range.first, range.last + 1, std::move(updated),
                               /*record_undo=*/true);
}

bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close) {
  if (open.empty() || close.empty()) return false;
  auto sel = viewport.selection_range();
  if (!sel) {
    // Toggle a single line: strip an existing wrap, otherwise wrap.
    std::size_t line_index = viewport.cursor_line();
    if (line_index >= viewport.lines().size()) return false;
    const std::string& line = viewport.lines()[line_index];
    SelectionRange r{{line_index, 0}, {line_index, line.size()}};
    if (auto stripped = TryStripBlockComment(line, open, close)) {
      return viewport.ReplaceRange(r, *stripped, /*record_undo=*/true);
    }
    std::string replacement;
    replacement.reserve(open.size() + line.size() + close.size());
    replacement.append(open);
    replacement.append(line);
    replacement.append(close);
    return viewport.ReplaceRange(r, replacement, /*record_undo=*/true);
  }
  SelectionRange n = *sel;
  if (n.start.line > n.end.line ||
      (n.start.line == n.end.line && n.start.column > n.end.column)) {
    std::swap(n.start, n.end);
  }
  std::string content = viewport.SelectedText();
  if (auto stripped = TryStripBlockComment(content, open, close)) {
    return viewport.ReplaceRange(n, *stripped, /*record_undo=*/true);
  }
  std::string wrapped;
  wrapped.reserve(open.size() + content.size() + close.size());
  wrapped.append(open);
  wrapped.append(content);
  wrapped.append(close);
  return viewport.ReplaceRange(n, wrapped, /*record_undo=*/true);
}

namespace {

// Captures caret state before a line-move edit, then re-applies shifted
// positions after the edit so the primary caret, selection, and secondary
// carets all follow the moved lines (carets outside the moved range stay
// put). Used by MoveLineUp / MoveLineDown — the underlying ReplaceLines
// snaps the caret to (range_first, 0) inside its history-entry after
// state, which loses the original column and any secondary carets.
struct LineMoveCaretSnapshot {
  std::size_t primary_line = 0;
  std::size_t primary_column = 0;
  std::optional<SelectionRange> selection;
  // Full secondary carets (with anchors) so a ranged Ctrl-D selection survives
  // the transform instead of collapsing to a bare caret (A-120).
  std::vector<SecondaryCaret> secondaries;
};

LineMoveCaretSnapshot SnapshotCaretsForLineMove(const TextViewport& viewport) {
  return LineMoveCaretSnapshot{
      .primary_line = viewport.cursor_line(),
      .primary_column = viewport.cursor_column(),
      .selection = viewport.selection_range(),
      .secondaries = viewport.secondary_caret_ranges(),
  };
}

// Restore the snapshot's secondary carets, preserving each caret's selection
// anchor. `remap` maps a snapshot TextPosition (line+column) to its post-edit
// position. A plain caret (no anchor) restores as an empty range = bare caret,
// so the position-only behavior is unchanged for column-only caret sets.
template <typename Remap>
void RestoreSecondaryCaretRanges(TextViewport& viewport,
                                 const std::vector<SecondaryCaret>& secondaries,
                                 Remap&& remap) {
  std::vector<SelectionRange> ranges;
  ranges.reserve(secondaries.size());
  for (const SecondaryCaret& secondary : secondaries) {
    const TextPosition cursor = remap(secondary.position);
    const TextPosition anchor =
        secondary.selection_anchor.has_value() ? remap(*secondary.selection_anchor) : cursor;
    ranges.push_back(SelectionRange{anchor, cursor});
  }
  viewport.SetSecondaryCaretsWithRanges(ranges);
}

void RestoreCaretsAfterLineMove(TextViewport& viewport,
                                const LineMoveCaretSnapshot& snapshot,
                                std::size_t range_first,
                                std::size_t range_last,
                                std::ptrdiff_t delta) {
  const auto shift = [&](std::size_t line) -> std::size_t {
    if (line < range_first || line > range_last) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  // A whole-line selection ends at column 0 of the line AFTER the block, so its
  // end.line is range_last + 1 (ResolveLineRange excludes that trailing line from
  // the moved range). That exclusive boundary moved with the block, so extend the
  // shiftable range to range_last + 1 for the end. Without this the guard below
  // rejected the selection and the whole thing fell to the single-caret branch,
  // silently dropping the selection after the move.
  const auto shift_boundary = [&](std::size_t line) -> std::size_t {
    if (line < range_first || line > range_last + 1) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  viewport.ClearSecondaryCarets();
  const bool selection_covers_block =
      snapshot.selection.has_value() && snapshot.selection->start.line >= range_first &&
      (snapshot.selection->end.line <= range_last ||
       (snapshot.selection->end.line == range_last + 1 && snapshot.selection->end.column == 0));
  if (selection_covers_block) {
    viewport.MoveCursorTo(shift(snapshot.selection->start.line),
                          snapshot.selection->start.column);
    viewport.MoveCursorTo(shift_boundary(snapshot.selection->end.line),
                          snapshot.selection->end.column, /*extend_selection=*/true);
  } else {
    viewport.MoveCursorTo(shift(snapshot.primary_line), snapshot.primary_column);
  }

  // Rebuild the shifted secondary carets in a single pass, preserving each
  // caret's selection anchor (A-120): a line move shifts a caret's line while
  // its column is unchanged.
  RestoreSecondaryCaretRanges(viewport, snapshot.secondaries, [&](const TextPosition& pos) {
    return TextPosition{shift(pos.line), pos.column};
  });
}

}  // namespace

bool MoveLineUp(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  if (range.first == 0) return false;
  const auto& lines = viewport.lines();
  if (range.last >= lines.size()) return false;
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // One piece-tree walk for the moved block. The per-line `lines[i]` this replaced
  // goes through TextBuffer::LineRef, which materialises a string AND inserts it
  // into the buffer's line cache — and a multi-caret line op covers every line
  // between the outermost carets, so it also evicted the cache across that span.
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 2);
  {
    std::vector<std::string> block = lines.SliceLines(range.first, range.last + 1);
    updated.insert(updated.end(), std::make_move_iterator(block.begin()),
                   std::make_move_iterator(block.end()));
  }
  updated.push_back(std::string(lines.LineView(range.first - 1)));
  // Wrap the replace + caret restore in one undo group so the aggregate entry's
  // after_state is captured AFTER RestoreCaretsAfterLineMove — otherwise the
  // ReplaceLines entry snaps after_state to (range_first, 0) and redo loses the
  // real column and every secondary caret.
  viewport.BeginUndoGroup();
  if (!viewport.ReplaceLines(range.first - 1, range.last + 1, std::move(updated),
                             /*record_undo=*/true)) {
    viewport.EndUndoGroup();
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, range.first, range.last, -1);
  viewport.EndUndoGroup();
  return true;
}

bool MoveLineDown(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.last + 1 >= lines.size()) return false;
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // See MoveLineUp for why this is one SliceLines walk and not a per-line read.
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 2);
  updated.push_back(std::string(lines.LineView(range.last + 1)));
  std::vector<std::string> block = lines.SliceLines(range.first, range.last + 1);
  updated.insert(updated.end(), std::make_move_iterator(block.begin()),
                 std::make_move_iterator(block.end()));
  // See MoveLineUp: group the replace + caret restore so redo keeps the carets.
  viewport.BeginUndoGroup();
  if (!viewport.ReplaceLines(range.first, range.last + 2, std::move(updated),
                             /*record_undo=*/true)) {
    viewport.EndUndoGroup();
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, range.first, range.last, +1);
  viewport.EndUndoGroup();
  return true;
}

bool DuplicateSelection(TextViewport& viewport) {
  auto sel = viewport.selection_range();
  const auto& lines = viewport.lines();
  if (sel) {
    SelectionRange n = *sel;
    if (n.start.line > n.end.line ||
        (n.start.line == n.end.line && n.start.column > n.end.column)) {
      std::swap(n.start, n.end);
    }
    std::string text = viewport.SelectedText();
    SelectionRange empty{n.end, n.end};
    return viewport.ReplaceRange(empty, text, /*record_undo=*/true);
  }
  std::size_t line_index = viewport.cursor_line();
  if (line_index >= lines.size()) return false;
  std::vector<std::string> updated{lines[line_index], lines[line_index]};
  return viewport.ReplaceLines(line_index, line_index + 1, std::move(updated),
                               /*record_undo=*/true);
}

bool DeleteLine(TextViewport& viewport) {
  return viewport.DeleteCurrentLine();
}

namespace {

// Restore a multi-line selection across `[first_line, last_line]` after a
// per-line indent/outdent transform. Without this, the editor's history /
// line-edit pipeline clears the selection (via `BuildLineHistoryEntry`),
// which would force successive Tab / Shift+Tab presses to fall back to
// single-caret semantics. Preserving the selection matches VSCode/Zed
// behavior so a sequence of indent / outdent actions stays anchored on the
// original block.
void RestoreSelectionAcrossLines(TextViewport& viewport,
                                 std::size_t first_line,
                                 std::size_t last_line,
                                 bool had_prior_selection) {
  if (!had_prior_selection) {
    return;
  }
  if (viewport.line_count() == 0) return;
  if (first_line >= viewport.line_count()) {
    first_line = viewport.line_count() - 1;
  }
  if (last_line >= viewport.line_count()) {
    last_line = viewport.line_count() - 1;
  }
  viewport.MoveCursorTo(first_line, 0, /*extend_selection=*/false);
  viewport.MoveCursorTo(last_line, viewport.lines().LineLength(last_line),
                        /*extend_selection=*/true);
}

// Restore carets after an indent/outdent that changed each line's leading
// whitespace IN PLACE (lines are not reordered). Each caret's column shifts by
// the delta applied to its line (clamped at 0 and the new line length); the
// primary selection and every secondary caret shift the same way. Used for the
// multi-caret and single-caret-no-selection paths, which the selection-only
// RestoreSelectionAcrossLines could not carry — it dropped every secondary caret
// and snapped the primary to (first_line, 0).
void RestoreCaretsAfterIndentEdit(TextViewport& viewport,
                                  const LineMoveCaretSnapshot& snapshot,
                                  std::size_t first_line,
                                  const std::vector<std::ptrdiff_t>& per_line_delta) {
  const auto shift_col = [&](std::size_t line, std::size_t column) -> std::size_t {
    std::size_t result = column;
    if (line >= first_line && line - first_line < per_line_delta.size()) {
      const std::ptrdiff_t shifted =
          static_cast<std::ptrdiff_t>(column) + per_line_delta[line - first_line];
      result = shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
    }
    if (line < viewport.line_count()) {
      result = std::min(result, viewport.lines().LineLength(line));
    }
    return result;
  };

  viewport.ClearSecondaryCarets();
  if (snapshot.selection.has_value()) {
    viewport.MoveCursorTo(snapshot.selection->start.line,
                          shift_col(snapshot.selection->start.line, snapshot.selection->start.column));
    viewport.MoveCursorTo(snapshot.selection->end.line,
                          shift_col(snapshot.selection->end.line, snapshot.selection->end.column),
                          /*extend_selection=*/true);
  } else {
    viewport.MoveCursorTo(snapshot.primary_line,
                          shift_col(snapshot.primary_line, snapshot.primary_column));
  }

  // An indent/outdent shifts each caret's column by its line's delta while the
  // line stays put; preserve every secondary caret's selection anchor (A-120).
  RestoreSecondaryCaretRanges(viewport, snapshot.secondaries, [&](const TextPosition& pos) {
    return TextPosition{pos.line, shift_col(pos.line, pos.column)};
  });
}

}  // namespace

bool IndentSelection(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.first >= lines.size()) return false;
  std::string indent_unit;
  if (viewport.soft_tabs()) {
    indent_unit.assign(viewport.indent_width(), ' ');
  } else {
    indent_unit = "\t";
  }
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 1);
  std::vector<std::ptrdiff_t> per_line_delta;
  per_line_delta.reserve(range.last - range.first + 1);
  for (std::size_t i = range.first; i <= range.last; ++i) {
    // LineView, not lines[i]: LineRef would also insert every line of the span
    // into the buffer's line cache (see MoveLineUp).
    const std::string_view source = lines.LineView(i);
    if (source.empty()) {
      updated.emplace_back();
      per_line_delta.push_back(0);
    } else {
      std::string line = indent_unit;
      line += source;
      updated.push_back(std::move(line));
      per_line_delta.push_back(static_cast<std::ptrdiff_t>(indent_unit.size()));
    }
  }
  const bool had_selection = viewport.has_selection();
  const bool multi = viewport.has_multiple_carets();
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // Group the replace + caret restore so the aggregate undo entry's after_state
  // captures the restored carets (else redo snaps to (first,0) and drops carets).
  viewport.BeginUndoGroup();
  const bool changed = viewport.ReplaceLines(range.first, range.last + 1, std::move(updated),
                                             /*record_undo=*/true);
  if (changed) {
    if (multi || !had_selection) {
      RestoreCaretsAfterIndentEdit(viewport, snapshot, range.first, per_line_delta);
    } else {
      RestoreSelectionAcrossLines(viewport, range.first, range.last, had_selection);
    }
  }
  viewport.EndUndoGroup();
  return changed;
}

bool OutdentSelection(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.first >= lines.size()) return false;
  std::size_t indent_width = viewport.indent_width() == 0 ? 4 : viewport.indent_width();
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 1);
  std::vector<std::ptrdiff_t> per_line_delta;
  per_line_delta.reserve(range.last - range.first + 1);
  bool any_change = false;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    // LineView, not lines[i]: see IndentSelection.
    const std::string_view line = lines.LineView(i);
    if (line.empty()) {
      updated.emplace_back();
      per_line_delta.push_back(0);
      continue;
    }
    std::size_t strip = 0;
    if (line[0] == '\t') {
      strip = 1;
    } else {
      while (strip < indent_width && strip < line.size() && line[strip] == ' ') ++strip;
    }
    if (strip == 0) {
      updated.emplace_back(line);
      per_line_delta.push_back(0);
    } else {
      updated.emplace_back(line.substr(strip));
      per_line_delta.push_back(-static_cast<std::ptrdiff_t>(strip));
      any_change = true;
    }
  }
  if (!any_change) return false;
  const bool had_selection = viewport.has_selection();
  const bool multi = viewport.has_multiple_carets();
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // See IndentSelection: group the replace + caret restore so redo keeps carets.
  viewport.BeginUndoGroup();
  const bool changed = viewport.ReplaceLines(range.first, range.last + 1, std::move(updated),
                                             /*record_undo=*/true);
  if (changed) {
    if (multi || !had_selection) {
      RestoreCaretsAfterIndentEdit(viewport, snapshot, range.first, per_line_delta);
    } else {
      RestoreSelectionAcrossLines(viewport, range.first, range.last, had_selection);
    }
  }
  viewport.EndUndoGroup();
  return changed;
}

bool SortLines(TextViewport& viewport, bool ascending) {
  LineRange range = ResolveLineRange(viewport);
  if (range.first == range.last) return false;
  const auto& lines = viewport.lines();
  if (range.last >= lines.size()) return false;
  // See MoveLineUp for why this is one SliceLines walk and not a per-line read.
  std::vector<std::string> sorted = lines.SliceLines(range.first, range.last + 1);
  if (ascending) {
    std::sort(sorted.begin(), sorted.end());
  } else {
    std::sort(sorted.begin(), sorted.end(), std::greater<std::string>());
  }
  return viewport.ReplaceLines(range.first, range.last + 1, std::move(sorted),
                               /*record_undo=*/true);
}

}  // namespace microide::editor
