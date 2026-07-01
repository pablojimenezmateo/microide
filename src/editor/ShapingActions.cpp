#include "editor/ShapingActions.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

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

  for (const TextPosition& secondary : viewport.secondary_caret_positions()) {
    r.first = std::min(r.first, secondary.line);
    r.last = std::max(r.last, secondary.line);
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

}  // namespace

bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker) {
  if (line_marker.empty()) return false;
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.first >= lines.size()) return false;

  // Determine whether all non-blank lines in range start with the marker
  // (after leading whitespace). If yes, uncomment; otherwise, comment.
  std::string marker(line_marker);
  bool all_commented = true;
  bool any_non_blank = false;
  std::size_t min_indent = std::string::npos;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string& line = lines[i];
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
    const std::string& line = lines[i];
    if (LineIsEmptyOrWhitespace(line)) {
      updated.push_back(line);
      continue;
    }
    if (all_commented) {
      // Strip first occurrence of marker after leading whitespace.
      std::size_t lead = LeadingWhitespaceCount(line);
      std::string out = line.substr(0, lead);
      std::size_t pos = lead + marker.size();
      // Strip a single space after marker if present (common style).
      if (pos < line.size() && line[pos] == ' ') ++pos;
      out += line.substr(pos);
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
  return viewport.ReplaceLines(range.first, range.last + 1, updated, /*record_undo=*/true);
}

bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close) {
  if (open.empty() || close.empty()) return false;
  auto sel = viewport.selection_range();
  if (!sel) {
    // Wrap a single line.
    std::size_t line_index = viewport.cursor_line();
    if (line_index >= viewport.lines().size()) return false;
    const std::string& line = viewport.lines()[line_index];
    SelectionRange r{{line_index, 0}, {line_index, line.size()}};
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
  std::vector<TextPosition> secondaries;
};

LineMoveCaretSnapshot SnapshotCaretsForLineMove(const TextViewport& viewport) {
  return LineMoveCaretSnapshot{
      .primary_line = viewport.cursor_line(),
      .primary_column = viewport.cursor_column(),
      .selection = viewport.selection_range(),
      .secondaries = viewport.secondary_carets(),
  };
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

  viewport.ClearSecondaryCarets();
  if (snapshot.selection.has_value() &&
      snapshot.selection->start.line >= range_first &&
      snapshot.selection->end.line <= range_last) {
    viewport.MoveCursorTo(shift(snapshot.selection->start.line),
                          snapshot.selection->start.column);
    viewport.MoveCursorTo(shift(snapshot.selection->end.line),
                          snapshot.selection->end.column, /*extend_selection=*/true);
  } else {
    viewport.MoveCursorTo(shift(snapshot.primary_line), snapshot.primary_column);
  }

  // Rebuild the shifted secondary carets in a single SetSecondaryCarets pass
  // instead of an AddSecondaryCaret-per-caret loop, which re-sorted the whole
  // set on every insert (O(N^2 log N) for a line move with many carets).
  std::vector<TextPosition> shifted;
  shifted.reserve(snapshot.secondaries.size());
  for (const TextPosition& secondary : snapshot.secondaries) {
    shifted.push_back(TextPosition{shift(secondary.line), secondary.column});
  }
  viewport.SetSecondaryCarets(std::move(shifted));
}

}  // namespace

bool MoveLineUp(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  if (range.first == 0) return false;
  const auto& lines = viewport.lines();
  if (range.last >= lines.size()) return false;
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 2);
  for (std::size_t i = range.first; i <= range.last; ++i) updated.push_back(lines[i]);
  updated.push_back(lines[range.first - 1]);
  if (!viewport.ReplaceLines(range.first - 1, range.last + 1, updated,
                             /*record_undo=*/true)) {
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, range.first, range.last, -1);
  return true;
}

bool MoveLineDown(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.last + 1 >= lines.size()) return false;
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 2);
  updated.push_back(lines[range.last + 1]);
  for (std::size_t i = range.first; i <= range.last; ++i) updated.push_back(lines[i]);
  if (!viewport.ReplaceLines(range.first, range.last + 2, updated, /*record_undo=*/true)) {
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, range.first, range.last, +1);
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
  return viewport.ReplaceLines(line_index, line_index + 1, updated, /*record_undo=*/true);
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
  viewport.MoveCursorTo(last_line, viewport.lines()[last_line].size(),
                        /*extend_selection=*/true);
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
  for (std::size_t i = range.first; i <= range.last; ++i) {
    if (lines[i].empty()) {
      updated.push_back(lines[i]);
    } else {
      std::string line = indent_unit;
      line += lines[i];
      updated.push_back(std::move(line));
    }
  }
  const bool had_selection = viewport.has_selection();
  const bool changed = viewport.ReplaceLines(range.first, range.last + 1, updated,
                                             /*record_undo=*/true);
  if (changed) {
    RestoreSelectionAcrossLines(viewport, range.first, range.last, had_selection);
  }
  return changed;
}

bool OutdentSelection(TextViewport& viewport) {
  LineRange range = ResolveLineRange(viewport);
  const auto& lines = viewport.lines();
  if (range.first >= lines.size()) return false;
  std::size_t indent_width = viewport.indent_width() == 0 ? 4 : viewport.indent_width();
  std::vector<std::string> updated;
  updated.reserve(range.last - range.first + 1);
  bool any_change = false;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string& line = lines[i];
    if (line.empty()) {
      updated.push_back(line);
      continue;
    }
    std::size_t strip = 0;
    if (line[0] == '\t') {
      strip = 1;
    } else {
      while (strip < indent_width && strip < line.size() && line[strip] == ' ') ++strip;
    }
    if (strip == 0) {
      updated.push_back(line);
    } else {
      updated.emplace_back(line.substr(strip));
      any_change = true;
    }
  }
  if (!any_change) return false;
  const bool had_selection = viewport.has_selection();
  const bool changed = viewport.ReplaceLines(range.first, range.last + 1, updated,
                                             /*record_undo=*/true);
  if (changed) {
    RestoreSelectionAcrossLines(viewport, range.first, range.last, had_selection);
  }
  return changed;
}

bool SortLines(TextViewport& viewport, bool ascending) {
  LineRange range = ResolveLineRange(viewport);
  if (range.first == range.last) return false;
  const auto& lines = viewport.lines();
  if (range.last >= lines.size()) return false;
  std::vector<std::string> sorted;
  sorted.reserve(range.last - range.first + 1);
  for (std::size_t i = range.first; i <= range.last; ++i) sorted.push_back(lines[i]);
  if (ascending) {
    std::sort(sorted.begin(), sorted.end());
  } else {
    std::sort(sorted.begin(), sorted.end(), std::greater<std::string>());
  }
  return viewport.ReplaceLines(range.first, range.last + 1, sorted, /*record_undo=*/true);
}

}  // namespace microide::editor
