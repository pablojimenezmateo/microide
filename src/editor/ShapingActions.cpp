#include "editor/ShapingActions.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

using SecondaryCaret = TextViewportUndoHistory::SecondaryCaret;

struct LineRange {
  std::size_t first = 0;
  std::size_t last = 0;

  std::size_t line_count() const { return last - first + 1; }
};

LineRange RangeForCaret(TextPosition anchor, TextPosition cursor) {
  if (cursor.line < anchor.line ||
      (cursor.line == anchor.line && cursor.column < anchor.column)) {
    std::swap(anchor, cursor);
  }
  LineRange r{anchor.line, cursor.line};
  if (r.last > r.first && cursor.column == 0) {
    --r.last;
  }
  return r;
}

std::vector<LineRange> ResolveLineRanges(const TextViewport& viewport,
                                         bool expand_collapsed_folds = false) {
  const std::size_t line_count = viewport.line_count();
  std::vector<LineRange> ranges;
  const std::span<const SecondaryCaret> secondaries = viewport.secondary_caret_range_view();
  ranges.reserve(secondaries.size() + 1);
  const auto caret_range = [&](std::size_t line) {
    return LineRange{line, expand_collapsed_folds ? viewport.CollapsedFoldEndAt(line) : line};
  };

  if (auto sel = viewport.selection_range()) {
    ranges.push_back(RangeForCaret(sel->start, sel->end));
  } else {
    ranges.push_back(caret_range(viewport.cursor_line()));
  }
  for (const SecondaryCaret& secondary : secondaries) {
    ranges.push_back(secondary.selection_anchor.has_value()
                         ? RangeForCaret(*secondary.selection_anchor, secondary.position)
                         : caret_range(secondary.position.line));
  }

  if (line_count == 0) {
    ranges.clear();
    return ranges;
  }
  for (LineRange& r : ranges) {
    r.first = std::min(r.first, line_count - 1);
    r.last = std::min(r.last, line_count - 1);
    if (r.last < r.first) {
      r.last = r.first;
    }
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const LineRange& a, const LineRange& b) { return a.first < b.first; });
  std::size_t merged = 0;
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first <= ranges[merged].last + 1) {
      ranges[merged].last = std::max(ranges[merged].last, ranges[i].last);
      continue;
    }
    ranges[++merged] = ranges[i];
  }
  ranges.resize(merged + 1);
  return ranges;
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
  if (core.size() < open.size() + close.size()) return std::nullopt;
  if (!core.starts_with(open) || !core.ends_with(close)) return std::nullopt;
  std::string_view inner =
      core.substr(open.size(), core.size() - open.size() - close.size());
  if (inner.starts_with(' ')) inner.remove_prefix(1);
  if (inner.ends_with(' ')) inner.remove_suffix(1);
  std::string result;
  result.reserve(content.size());
  result.append(content.substr(0, begin));
  result.append(inner);
  result.append(content.substr(end));
  return result;
}

TextPosition EndOfInsertion(TextPosition start, std::string_view text) {
  const std::size_t last_newline = text.rfind('\n');
  if (last_newline == std::string_view::npos) {
    return TextPosition{start.line, start.column + text.size()};
  }
  const std::size_t newlines =
      static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  return TextPosition{start.line + newlines, text.size() - last_newline - 1};
}

bool ReplaceAndSelect(TextViewport& viewport,
                      const SelectionRange& range,
                      const std::string& text,
                      std::size_t select_from,
                      std::size_t select_to) {
  if (!viewport.ReplaceRange(range, text, true)) {
    return false;
  }
  const std::string_view inserted = text;
  const TextPosition from = EndOfInsertion(range.start, inserted.substr(0, select_from));
  const TextPosition to = EndOfInsertion(range.start, inserted.substr(0, select_to));
  viewport.MoveCursorTo(from.line, from.column, false);
  viewport.MoveCursorTo(to.line, to.column, true);
  return true;
}

std::optional<SelectionRange> RangeOverSurroundingMarkers(const TextViewport& viewport,
                                                          const SelectionRange& range,
                                                          std::string_view open,
                                                          std::string_view close) {
  const std::string& first = viewport.lines()[range.start.line];
  const std::string& last = viewport.lines()[range.end.line];
  const std::string_view before = std::string_view(first).substr(0, range.start.column);
  const std::string_view after = std::string_view(last).substr(range.end.column);
  std::size_t open_len = 0;
  if (before.ends_with(open)) {
    open_len = open.size();
  } else if (before.size() > open.size() && before.back() == ' ' &&
             before.substr(0, before.size() - 1).ends_with(open)) {
    open_len = open.size() + 1;
  }
  std::size_t close_len = 0;
  if (after.starts_with(close)) {
    close_len = close.size();
  } else if (after.size() > close.size() && after.front() == ' ' &&
             after.substr(1).starts_with(close)) {
    close_len = close.size() + 1;
  }
  if (open_len == 0 || close_len == 0) {
    return std::nullopt;
  }
  return SelectionRange{TextPosition{range.start.line, range.start.column - open_len},
                        TextPosition{range.end.line, range.end.column + close_len}};
}

}

namespace {

bool BuildToggledCommentRegion(const TextBuffer& lines,
                               LineRange range,
                               std::string_view marker,
                               LineBlob* updated) {
  bool all_commented = true;
  bool any_non_blank = false;
  std::size_t min_indent = std::string::npos;
  std::size_t content_bytes = 0;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string_view line = lines.LineView(i);
    content_bytes += line.size();
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

  updated->clear();
  updated->reserve_lines(range.line_count());
  updated->reserve_bytes(content_bytes + range.line_count() * (marker.size() + 1));
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string_view line = lines.LineView(i);
    if (LineIsEmptyOrWhitespace(line)) {
      updated->push_back(line);
      continue;
    }
    if (all_commented) {
      std::size_t lead = LeadingWhitespaceCount(line);
      std::size_t pos = lead + marker.size();
      if (pos < line.size() && line[pos] == ' ') ++pos;
      updated->push_joined(line.substr(0, lead), line.substr(pos));
    } else {
      updated->push_joined(line.substr(0, min_indent), marker, std::string_view(" "),
                           line.substr(min_indent));
    }
  }
  return true;
}

}

namespace {

SelectionRange ShrinkTrailingLineStart(const TextViewport& viewport, SelectionRange range) {
  if (range.end.line > range.start.line && range.end.column == 0) {
    range.end.line -= 1;
    range.end.column = viewport.lines().LineLength(range.end.line);
  }
  return range;
}

std::vector<SelectionRange> BlockCommentRegions(const TextViewport& viewport) {
  const std::size_t line_count = viewport.line_count();
  std::vector<SelectionRange> regions;
  if (line_count == 0) return regions;
  const auto whole_line = [&](std::size_t line) {
    const std::size_t clamped = std::min(line, line_count - 1);
    return SelectionRange{{clamped, 0}, {clamped, viewport.lines().LineLength(clamped)}};
  };
  if (const auto sel = viewport.selection_range()) {
    regions.push_back(ShrinkTrailingLineStart(viewport, *sel));
  } else {
    regions.push_back(whole_line(viewport.cursor_line()));
  }
  for (const SecondaryCaret& caret : viewport.secondary_caret_range_view()) {
    if (caret.selection_anchor.has_value() &&
        !(*caret.selection_anchor == caret.position)) {
      SelectionRange range{*caret.selection_anchor, caret.position};
      if (range.end.line < range.start.line ||
          (range.end.line == range.start.line && range.end.column < range.start.column)) {
        std::swap(range.start, range.end);
      }
      regions.push_back(ShrinkTrailingLineStart(viewport, range));
    } else {
      regions.push_back(whole_line(caret.position.line));
    }
  }
  for (SelectionRange& range : regions) {
    if (range.end.line < range.start.line ||
        (range.end.line == range.start.line && range.end.column < range.start.column)) {
      std::swap(range.start, range.end);
    }
    range.start.line = std::min(range.start.line, line_count - 1);
    range.end.line = std::min(range.end.line, line_count - 1);
  }
  std::sort(regions.begin(), regions.end(), [](const SelectionRange& a, const SelectionRange& b) {
    return a.start.line != b.start.line ? a.start.line > b.start.line
                                        : a.start.column > b.start.column;
  });
  std::vector<SelectionRange> disjoint;
  for (const SelectionRange& range : regions) {
    if (!disjoint.empty()) {
      const SelectionRange& lower = disjoint.back();
      const bool overlaps = range.end.line > lower.start.line ||
                            (range.end.line == lower.start.line &&
                             range.end.column > lower.start.column);
      if (overlaps) {
        continue;
      }
    }
    disjoint.push_back(range);
  }
  return disjoint;
}

bool ToggleBlockCommentAtEveryCaret(TextViewport& viewport,
                                    std::string_view open,
                                    std::string_view close) {
  const std::vector<SelectionRange> regions = BlockCommentRegions(viewport);
  if (regions.empty()) return false;

  viewport.BeginUndoGroup();
  bool changed = false;
  std::vector<SelectionRange> results;
  results.reserve(regions.size());
  for (const SelectionRange& region : regions) {
    const std::string content = viewport.TextInRange(region);
    SelectionRange target = region;
    std::string text;
    std::size_t select_from = 0;
    std::size_t select_to = 0;
    if (const auto stripped = TryStripBlockComment(content, open, close)) {
      text = *stripped;
      select_to = text.size();
    } else if (const auto outer = RangeOverSurroundingMarkers(viewport, region, open, close)) {
      target = *outer;
      text = content;
      select_to = text.size();
    } else {
      text.reserve(open.size() + content.size() + close.size() + 2);
      text.append(open).push_back(' ');
      text.append(content).push_back(' ');
      text.append(close);
      select_from = open.size() + 1;
      select_to = select_from + content.size();
    }
    if (!viewport.ReplaceRange(target, text, true)) {
      continue;
    }
    changed = true;
    results.push_back(SelectionRange{
        EndOfInsertion(target.start, std::string_view(text).substr(0, select_from)),
        EndOfInsertion(target.start, std::string_view(text).substr(0, select_to)),
    });
  }
  viewport.EndUndoGroup();
  if (!changed) {
    return false;
  }

  const SelectionRange primary = results.back();
  viewport.MoveCursorTo(primary.start.line, primary.start.column, false);
  viewport.MoveCursorTo(primary.end.line, primary.end.column, true);
  results.pop_back();
  viewport.SetSecondaryCaretsWithRanges(results);
  return true;
}

}

bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close) {
  if (open.empty() || close.empty()) return false;
  if (viewport.has_multiple_carets()) {
    return ToggleBlockCommentAtEveryCaret(viewport, open, close);
  }
  auto sel = viewport.selection_range();
  if (!sel) {
    std::size_t line_index = viewport.cursor_line();
    if (line_index >= viewport.lines().size()) return false;
    const std::string& line = viewport.lines()[line_index];
    SelectionRange r{{line_index, 0}, {line_index, line.size()}};
    if (auto stripped = TryStripBlockComment(line, open, close)) {
      return viewport.ReplaceRange(r, *stripped, true);
    }
    std::string replacement;
    replacement.reserve(open.size() + line.size() + close.size() + 2);
    replacement.append(open).push_back(' ');
    replacement.append(line).push_back(' ');
    replacement.append(close);
    return viewport.ReplaceRange(r, replacement, true);
  }
  SelectionRange n = *sel;
  if (n.start.line > n.end.line ||
      (n.start.line == n.end.line && n.start.column > n.end.column)) {
    std::swap(n.start, n.end);
  }
  n = ShrinkTrailingLineStart(viewport, n);
  std::string content = viewport.TextInRange(n);
  if (auto stripped = TryStripBlockComment(content, open, close)) {
    return ReplaceAndSelect(viewport, n, *stripped, 0, stripped->size());
  }
  if (const auto outer = RangeOverSurroundingMarkers(viewport, n, open, close)) {
    return ReplaceAndSelect(viewport, *outer, content, 0, content.size());
  }
  std::string wrapped;
  wrapped.reserve(open.size() + content.size() + close.size() + 2);
  wrapped.append(open).push_back(' ');
  wrapped.append(content).push_back(' ');
  wrapped.append(close);
  return ReplaceAndSelect(viewport, n, wrapped, open.size() + 1,
                          open.size() + 1 + content.size());
}

namespace {

struct LineMoveCaretSnapshot {
  std::size_t primary_line = 0;
  std::size_t primary_column = 0;
  std::optional<SelectionRange> selection;
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

const LineRange* RegionContaining(std::span<const LineRange> regions,
                                  std::size_t line,
                                  bool include_exclusive_end = false) {
  auto it = std::upper_bound(regions.begin(), regions.end(), line,
                             [](std::size_t value, const LineRange& r) { return value < r.first; });
  if (it == regions.begin()) {
    return nullptr;
  }
  --it;
  const std::size_t last = include_exclusive_end ? it->last + 1 : it->last;
  return line <= last ? &*it : nullptr;
}

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

template <typename ShiftFn, typename ShiftBoundaryFn>
void RestoreCaretsAfterLineTransform(TextViewport& viewport,
                                     const LineMoveCaretSnapshot& snapshot,
                                     std::span<const LineRange> regions,
                                     ShiftFn&& shift,
                                     ShiftBoundaryFn&& shift_boundary) {
  viewport.ClearSecondaryCarets();
  const LineRange* selection_region =
      snapshot.selection.has_value()
          ? RegionContaining(regions, snapshot.selection->start.line)
          : nullptr;
  const bool selection_covers_block =
      selection_region != nullptr &&
      (snapshot.selection->end.line <= selection_region->last ||
       (snapshot.selection->end.line == selection_region->last + 1 &&
        snapshot.selection->end.column == 0));
  if (selection_covers_block) {
    viewport.MoveCursorTo(shift(snapshot.selection->start.line),
                          snapshot.selection->start.column);
    std::size_t end_line = shift_boundary(snapshot.selection->end.line);
    std::size_t end_column = snapshot.selection->end.column;
    const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
    if (end_line > last_line) {
      end_line = last_line;
      end_column = viewport.lines().LineLength(last_line);
    }
    viewport.MoveCursorTo(end_line, end_column, true);
  } else {
    viewport.MoveCursorTo(shift(snapshot.primary_line), snapshot.primary_column);
  }

  RestoreSecondaryCaretRanges(viewport, snapshot.secondaries, [&](const TextPosition& pos) {
    return TextPosition{shift(pos.line), pos.column};
  });
}

void RestoreCaretsAfterLineMove(TextViewport& viewport,
                                const LineMoveCaretSnapshot& snapshot,
                                std::span<const LineRange> moved,
                                std::ptrdiff_t delta) {
  const auto shift = [&](std::size_t line) -> std::size_t {
    if (RegionContaining(moved, line) == nullptr) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  const auto shift_boundary = [&](std::size_t line) -> std::size_t {
    if (RegionContaining(moved, line, true) == nullptr) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  RestoreCaretsAfterLineTransform(viewport, snapshot, moved, shift, shift_boundary);
}

LineBlob BuildLineMoveReplacement(const TextBuffer& lines, LineRange range, bool downward) {
  LineBlob updated;
  updated.reserve_lines(range.line_count() + 1);
  if (downward) {
    updated.push_back(lines.LineView(range.last + 1));
  }
  lines.AppendLines(range.first, range.last + 1, updated);
  if (!downward) {
    updated.push_back(lines.LineView(range.first - 1));
  }
  return updated;
}

bool MoveLines(TextViewport& viewport, bool downward) {
  std::vector<LineRange> regions = ResolveLineRanges(viewport, true);
  const TextBuffer& lines = viewport.lines();
  const std::size_t line_count = lines.size();
  std::erase_if(regions, [&](const LineRange& r) {
    return downward ? r.last + 1 >= line_count : r.first == 0;
  });
  if (regions.empty()) return false;

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  viewport.BeginUndoGroup();
  bool changed = false;
  for (const LineRange& range : regions) {
    LineBlob updated = BuildLineMoveReplacement(lines, range, downward);
    const std::size_t start = downward ? range.first : range.first - 1;
    const std::size_t end = downward ? range.last + 2 : range.last + 1;
    changed |= viewport.ReplaceLines(start, end, std::move(updated), true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, regions, downward ? +1 : -1);
  viewport.EndUndoGroup();
  return true;
}

}

bool JoinLinesAtCarets(TextViewport& viewport) {
  const TextBuffer& lines = viewport.lines();
  if (lines.size() == 0) return false;
  std::vector<LineRange> regions = ResolveLineRanges(viewport);
  if (regions.empty()) return false;

  for (LineRange& region : regions) {
    if (region.first == region.last && region.last + 1 < lines.size()) {
      region.last += 1;
    }
  }
  std::erase_if(regions, [](const LineRange& region) { return region.first >= region.last; });
  if (regions.empty()) return false;

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  viewport.BeginUndoGroup();
  bool changed = false;
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    std::string joined(lines.LineView(region->first));
    for (std::size_t line = region->first + 1; line <= region->last; ++line) {
      const std::string_view text = lines.LineView(line);
      const std::size_t content = LeadingWhitespaceCount(text);
      const std::string_view trimmed = text.substr(content);
      if (trimmed.empty()) {
        continue;
      }
      if (!joined.empty() && joined.back() != ' ' && joined.back() != '\t') {
        joined.push_back(' ');
      }
      joined.append(trimmed);
    }
    LineBlob replacement;
    replacement.reserve_lines(1);
    replacement.push_back(joined);
    changed |= viewport.ReplaceLines(region->first, region->last + 1, std::move(replacement),
                                     true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  const auto shift_by = [&](std::size_t line) -> std::size_t {
    std::size_t removed = 0;
    for (const LineRange& region : regions) {
      if (region.last < line) {
        removed += region.line_count() - 1;
        continue;
      }
      if (region.first <= line) {
        return region.first - std::min(region.first, removed);
      }
      break;
    }
    return line - std::min(line, removed);
  };
  RestoreCaretsAfterLineTransform(
      viewport, snapshot, regions, shift_by,
      [&](std::size_t line) { return line == 0 ? line : shift_by(line - 1) + 1; });
  viewport.EndUndoGroup();
  return true;
}

bool MoveLineUp(TextViewport& viewport) { return MoveLines(viewport, false); }

bool MoveLineDown(TextViewport& viewport) { return MoveLines(viewport, true); }

bool CopyLines(TextViewport& viewport, bool downward) {
  const std::vector<LineRange> regions =
      ResolveLineRanges(viewport, true);
  const TextBuffer& lines = viewport.lines();
  if (regions.empty() || lines.size() == 0) return false;

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  viewport.BeginUndoGroup();
  bool changed = false;
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    LineBlob updated;
    updated.reserve_lines(region->line_count() * 2);
    lines.AppendLines(region->first, region->last + 1, updated);
    lines.AppendLines(region->first, region->last + 1, updated);
    changed |= viewport.ReplaceLines(region->first, region->last + 1, std::move(updated),
                                     true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  const auto shift_by = [&](std::size_t line, bool include_own_region) -> std::size_t {
    std::size_t offset = 0;
    for (const LineRange& region : regions) {
      if (region.last < line) {
        offset += region.line_count();
        continue;
      }
      if (include_own_region && downward && region.first <= line) {
        offset += region.line_count();
      }
      break;
    }
    return line + offset;
  };
  RestoreCaretsAfterLineTransform(
      viewport, snapshot, regions,
      [&](std::size_t line) { return shift_by(line, true); },
      [&](std::size_t line) { return shift_by(line == 0 ? line : line - 1, true) +
                                     (line == 0 ? 0 : 1); });
  viewport.EndUndoGroup();
  return true;
}

bool InsertLineBelow(TextViewport& viewport) {
  const TextBuffer& lines = viewport.lines();
  const std::size_t line = viewport.cursor_line();
  if (line >= lines.size()) return false;
  std::vector<TextPosition> secondaries;
  for (const TextPosition& caret : viewport.secondary_caret_positions()) {
    const std::size_t caret_line = std::min(caret.line, lines.size() - 1);
    secondaries.push_back(TextPosition{caret_line, lines.LineLength(caret_line)});
  }
  viewport.MoveCursorTo(line, lines.LineLength(line));
  if (!secondaries.empty()) {
    viewport.SetSecondaryCarets(std::move(secondaries));
  }
  viewport.InsertNewline();
  return true;
}

bool InsertLineAbove(TextViewport& viewport) {
  const TextBuffer& lines = viewport.lines();
  const std::size_t line = viewport.cursor_line();
  if (line >= lines.size()) return false;
  const auto indent_of = [&](std::size_t at) {
    return std::string(lines.LineView(at).substr(0, LeadingWhitespaceCount(lines.LineView(at))));
  };
  if (!viewport.has_multiple_carets()) {
    std::string inserted = indent_of(line);
    const std::size_t indent_bytes = inserted.size();
    inserted.push_back('\n');
    viewport.MoveCursorTo(line, 0);
    viewport.InsertText(inserted);
    viewport.MoveCursorTo(line, indent_bytes);
    return true;
  }
  std::vector<std::size_t> target_lines{line};
  for (const TextPosition& caret : viewport.secondary_caret_positions()) {
    target_lines.push_back(std::min(caret.line, lines.size() - 1));
  }
  std::sort(target_lines.begin(), target_lines.end());
  target_lines.erase(std::unique(target_lines.begin(), target_lines.end()), target_lines.end());
  const std::size_t primary_slot = static_cast<std::size_t>(
      std::lower_bound(target_lines.begin(), target_lines.end(), line) - target_lines.begin());
  viewport.BeginUndoGroup();
  std::vector<TextPosition> landed;
  landed.reserve(target_lines.size());
  for (std::size_t k = 0; k < target_lines.size(); ++k) {
    const std::size_t at = target_lines[k] + k;
    std::string inserted = indent_of(at);
    landed.push_back(TextPosition{at, inserted.size()});
    inserted.push_back('\n');
    (void)viewport.ReplaceRange(SelectionRange{TextPosition{at, 0}, TextPosition{at, 0}}, inserted,
                                true);
  }
  viewport.ClearSecondaryCarets();
  viewport.MoveCursorTo(landed[primary_slot].line, landed[primary_slot].column);
  landed.erase(landed.begin() + static_cast<std::ptrdiff_t>(primary_slot));
  viewport.SetSecondaryCarets(std::move(landed));
  viewport.EndUndoGroup();
  return true;
}

bool DeleteLine(TextViewport& viewport) {
  const std::span<const SecondaryCaret> secondaries = viewport.secondary_caret_range_view();
  const bool any_selection =
      viewport.has_selection() ||
      std::any_of(secondaries.begin(), secondaries.end(),
                  [](const SecondaryCaret& caret) { return caret.selection_anchor.has_value(); });
  if (!any_selection) {
    return viewport.DeleteCurrentLine();
  }

  const std::vector<LineRange> regions =
      ResolveLineRanges(viewport, true);
  const TextBuffer& lines = viewport.lines();
  if (regions.empty() || lines.size() == 0) {
    return false;
  }

  viewport.BeginUndoGroup();
  bool changed = false;
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    const std::size_t last = std::min(region->last, lines.size() - 1);
    const std::size_t first = std::min(region->first, last);
    SelectionRange range;
    if (last + 1 < lines.size()) {
      range = SelectionRange{TextPosition{first, 0}, TextPosition{last + 1, 0}};
    } else if (first > 0) {
      range = SelectionRange{TextPosition{first - 1, lines.LineLength(first - 1)},
                             TextPosition{last, lines.LineLength(last)}};
    } else {
      range = SelectionRange{TextPosition{0, 0}, TextPosition{last, lines.LineLength(last)}};
    }
    changed |= viewport.ReplaceRange(range, "", true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  viewport.ClearSecondaryCarets();
  viewport.ClearSelection();
  viewport.MoveCursorTo(std::min(regions.front().first, lines.size() - 1), 0);
  viewport.EndUndoGroup();
  return true;
}

namespace {

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
  viewport.MoveCursorTo(first_line, 0, false);
  viewport.MoveCursorTo(last_line, viewport.lines().LineLength(last_line),
                        true);
}

struct RegionColumnDeltas {
  std::vector<std::ptrdiff_t> flat;
  std::vector<std::size_t> region_offset;

  void BeginRegion() { region_offset.push_back(flat.size()); }

  std::ptrdiff_t For(std::span<const LineRange> regions, std::size_t line) const {
    const LineRange* region = RegionContaining(regions, line);
    if (region == nullptr) return 0;
    const std::size_t index = static_cast<std::size_t>(region - regions.data());
    if (index >= region_offset.size()) return 0;
    const std::size_t slot = region_offset[index] + (line - region->first);
    return slot < flat.size() ? flat[slot] : 0;
  }
};

void RestoreCaretsAfterIndentEdit(TextViewport& viewport,
                                  const LineMoveCaretSnapshot& snapshot,
                                  std::span<const LineRange> regions,
                                  const RegionColumnDeltas& deltas) {
  const auto shift_col = [&](std::size_t line, std::size_t column) -> std::size_t {
    const std::ptrdiff_t shifted =
        static_cast<std::ptrdiff_t>(column) + deltas.For(regions, line);
    std::size_t result = shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
    if (line < viewport.line_count()) {
      result = std::min(result, viewport.lines().LineLength(line));
    }
    return result;
  };

  viewport.ClearSecondaryCarets();
  if (snapshot.selection.has_value()) {
    const SelectionRange& selection = *snapshot.selection;
    const bool caret_at_start =
        snapshot.primary_line == selection.start.line && snapshot.primary_column == selection.start.column;
    const TextPosition& anchor = caret_at_start ? selection.end : selection.start;
    const TextPosition& caret = caret_at_start ? selection.start : selection.end;
    viewport.MoveCursorTo(anchor.line, shift_col(anchor.line, anchor.column));
    viewport.MoveCursorTo(caret.line, shift_col(caret.line, caret.column), true);
  } else {
    viewport.MoveCursorTo(snapshot.primary_line,
                          shift_col(snapshot.primary_line, snapshot.primary_column));
  }

  RestoreSecondaryCaretRanges(viewport, snapshot.secondaries, [&](const TextPosition& pos) {
    return TextPosition{pos.line, shift_col(pos.line, pos.column)};
  });
}

template <typename Transform>
bool ReindentRegions(TextViewport& viewport, Transform&& transform) {
  const std::vector<LineRange> regions = ResolveLineRanges(viewport);
  if (regions.empty()) return false;
  const TextBuffer& lines = viewport.lines();

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);

  RegionColumnDeltas deltas;
  std::size_t total_lines = 0;
  for (const LineRange& region : regions) {
    total_lines += region.line_count();
  }
  deltas.flat.reserve(total_lines);
  deltas.region_offset.reserve(regions.size());

  LineBlob updated;
  std::string out;
  bool changed = false;
  viewport.BeginUndoGroup();
  for (const LineRange& region : regions) {
    deltas.BeginRegion();
    updated.clear();
    updated.reserve_lines(region.line_count());
    bool region_changed = false;
    for (std::size_t i = region.first; i <= region.last; ++i) {
      out.clear();
      const std::ptrdiff_t delta = transform(lines.LineView(i), &out);
      deltas.flat.push_back(delta);
      region_changed |= delta != 0;
      updated.push_back(out);
    }
    if (!region_changed) continue;
    changed |= viewport.ReplaceLines(region.first, region.last + 1, std::move(updated),
                                     true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  RestoreCaretsAfterIndentEdit(viewport, snapshot, regions, deltas);
  viewport.EndUndoGroup();
  return true;
}

}

bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker) {
  if (line_marker.empty()) return false;
  const std::vector<LineRange> ranges = ResolveLineRanges(viewport);
  if (ranges.empty()) return false;
  const TextBuffer& lines = viewport.lines();
  const bool had_selection = viewport.has_selection();
  const bool multi = viewport.has_multiple_carets();
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  RegionColumnDeltas deltas;
  std::size_t total_lines = 0;
  for (const LineRange& range : ranges) {
    total_lines += range.line_count();
  }
  deltas.flat.reserve(total_lines);
  deltas.region_offset.reserve(ranges.size());
  LineBlob updated;
  bool changed = false;
  const bool grouped = ranges.size() > 1;
  if (grouped) viewport.BeginUndoGroup();
  for (const LineRange& range : ranges) {
    deltas.BeginRegion();
    if (!BuildToggledCommentRegion(lines, range, line_marker, &updated)) {
      for (std::size_t i = range.first; i <= range.last; ++i) deltas.flat.push_back(0);
      continue;
    }
    for (std::size_t i = range.first; i <= range.last; ++i) {
      const std::size_t offset = i - range.first;
      deltas.flat.push_back(offset < updated.size()
                                ? static_cast<std::ptrdiff_t>(updated[offset].size()) -
                                      static_cast<std::ptrdiff_t>(lines.LineLength(i))
                                : 0);
    }
    changed |= viewport.ReplaceLines(range.first, range.last + 1, std::move(updated),
                                     true);
  }
  if (!changed) {
    if (grouped) viewport.EndUndoGroup();
    return false;
  }
  if (multi || !had_selection) {
    RestoreCaretsAfterIndentEdit(viewport, snapshot, ranges, deltas);
  } else {
    RestoreSelectionAcrossLines(viewport, ranges.front().first, ranges.front().last,
                                had_selection);
  }
  if (grouped) viewport.EndUndoGroup();
  return true;
}

bool IndentSelection(TextViewport& viewport) {
  std::string indent_unit;
  if (viewport.soft_tabs()) {
    indent_unit.assign(viewport.indent_width(), ' ');
  } else {
    indent_unit = "\t";
  }
  return ReindentRegions(
      viewport, [&](std::string_view source, std::string* out) -> std::ptrdiff_t {
        if (source.empty()) {
          return 0;
        }
        out->reserve(indent_unit.size() + source.size());
        out->append(indent_unit);
        out->append(source);
        return static_cast<std::ptrdiff_t>(indent_unit.size());
      });
}

namespace {

bool SelectionIndentsAsBlock(const TextViewport& viewport, const SelectionRange& range) {
  if (range.start.line != range.end.line) {
    return true;
  }
  return range.start.column == 0 && range.end.column > 0 &&
         range.start.line < viewport.line_count() &&
         range.end.column >= viewport.lines().LineLength(range.start.line);
}

}

namespace {

struct TabKeySite {
  SelectionRange range;
  bool block = false;
};

SelectionRange NormalizedCaretRange(TextPosition anchor, TextPosition cursor) {
  const bool anchor_first =
      anchor.line < cursor.line || (anchor.line == cursor.line && anchor.column <= cursor.column);
  return anchor_first ? SelectionRange{anchor, cursor} : SelectionRange{cursor, anchor};
}

template <typename Visit>
void ForEachTabKeySite(const TextViewport& viewport, Visit&& visit) {
  if (const std::optional<SelectionRange> selection = viewport.selection_range();
      selection.has_value()) {
    visit(TabKeySite{*selection, SelectionIndentsAsBlock(viewport, *selection)});
  } else {
    const TextPosition caret{viewport.cursor_line(), viewport.cursor_column()};
    visit(TabKeySite{SelectionRange{caret, caret}, false});
  }
  for (const SecondaryCaret& secondary : viewport.secondary_caret_range_view()) {
    if (!secondary.selection_anchor.has_value() ||
        *secondary.selection_anchor == secondary.position) {
      visit(TabKeySite{SelectionRange{secondary.position, secondary.position}, false});
      continue;
    }
    const SelectionRange range =
        NormalizedCaretRange(*secondary.selection_anchor, secondary.position);
    visit(TabKeySite{range, SelectionIndentsAsBlock(viewport, range)});
  }
}

std::vector<TabKeySite> CollectTabKeySites(const TextViewport& viewport) {
  std::vector<TabKeySite> sites;
  sites.reserve(viewport.secondary_caret_range_view().size() + 1);
  ForEachTabKeySite(viewport, [&](const TabKeySite& site) { sites.push_back(site); });
  return sites;
}

}

TabKeyIntent ClassifyTabKey(const TextViewport& viewport, bool shift_held) {
  if (shift_held) {
    return TabKeyIntent::kOutdent;
  }
  bool any_block = false;
  bool any_point = false;
  ForEachTabKeySite(viewport, [&](const TabKeySite& site) {
    (site.block ? any_block : any_point) = true;
  });
  if (any_block && any_point) {
    return TabKeyIntent::kMixed;
  }
  return any_block ? TabKeyIntent::kIndentBlock : TabKeyIntent::kInsertTab;
}

bool ApplyMixedTabKey(TextViewport& viewport) {
  const std::size_t line_count = viewport.line_count();
  if (line_count == 0) {
    return false;
  }
  const std::vector<TabKeySite> sites = CollectTabKeySites(viewport);
  const std::size_t indent_width = std::max<std::size_t>(1, viewport.indent_width());
  const bool soft_tabs = viewport.soft_tabs();
  const std::string indent_unit = soft_tabs ? std::string(indent_width, ' ') : std::string("\t");
  const TextBuffer& lines = viewport.lines();

  const auto clamp_line = [&](std::size_t line) { return std::min(line, line_count - 1); };
  const auto clamp_position = [&](TextPosition position) {
    position.line = clamp_line(position.line);
    position.column = std::min(position.column, lines.LineLength(position.line));
    return position;
  };

  std::vector<std::size_t> block_lines;
  for (const TabKeySite& site : sites) {
    if (!site.block) {
      continue;
    }
    LineRange region = RangeForCaret(site.range.start, site.range.end);
    region.first = clamp_line(region.first);
    region.last = std::max(region.first, clamp_line(region.last));
    for (std::size_t line = region.first; line <= region.last; ++line) {
      block_lines.push_back(line);
    }
  }
  std::sort(block_lines.begin(), block_lines.end());
  block_lines.erase(std::unique(block_lines.begin(), block_lines.end()), block_lines.end());

  struct TabKeyEdit {
    SelectionRange range;
    std::size_t insert_length = 0;
    bool indent_unit_text = false;
    std::string text;

    std::string_view Text(std::string_view unit) const {
      return indent_unit_text ? unit : std::string_view(text);
    }
  };
  std::vector<TabKeyEdit> edits;
  edits.reserve(block_lines.size() + sites.size());
  for (const std::size_t line : block_lines) {
    if (lines.LineLength(line) == 0) {
      continue;
    }
    edits.push_back({SelectionRange{TextPosition{line, 0}, TextPosition{line, 0}},
                     indent_unit.size(), true, {}});
  }
  for (const TabKeySite& site : sites) {
    if (site.block) {
      continue;
    }
    const SelectionRange range{clamp_position(site.range.start), clamp_position(site.range.end)};
    std::string text;
    if (soft_tabs) {
      const std::size_t remainder =
          viewport.VisualColumnAt(range.start.line, range.start.column) % indent_width;
      text.assign(remainder == 0 ? indent_width : indent_width - remainder, ' ');
    } else {
      text = "\t";
    }
    edits.push_back({range, text.size(), false, std::move(text)});
  }
  if (edits.empty()) {
    return false;
  }

  std::sort(edits.begin(), edits.end(), [](const TabKeyEdit& a, const TabKeyEdit& b) {
    if (a.range.start.line != b.range.start.line) {
      return a.range.start.line < b.range.start.line;
    }
    return a.range.start.column < b.range.start.column;
  });

  const auto remap = [&](TextPosition position) {
    position = clamp_position(position);
    const auto first = std::lower_bound(
        edits.begin(), edits.end(), position.line,
        [](const TabKeyEdit& edit, std::size_t line) { return edit.range.start.line < line; });
    std::ptrdiff_t delta = 0;
    for (auto it = first; it != edits.end() && it->range.start.line == position.line; ++it) {
      if (it->range.end.column > position.column) {
        break;
      }
      delta += static_cast<std::ptrdiff_t>(it->insert_length) -
               static_cast<std::ptrdiff_t>(it->range.end.column - it->range.start.column);
    }
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(position.column) + delta;
    position.column = shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
    return position;
  };

  std::vector<SelectionRange> final_ranges;
  final_ranges.reserve(sites.size());
  for (const TabKeySite& site : sites) {
    if (site.block) {
      final_ranges.push_back(SelectionRange{remap(site.range.start), remap(site.range.end)});
      continue;
    }
    const TextPosition caret = remap(site.range.end);
    final_ranges.push_back(SelectionRange{caret, caret});
  }

  std::reverse(edits.begin(), edits.end());

  viewport.BeginUndoGroup();
  viewport.ClearSecondaryCarets();
  bool changed = false;
  for (const TabKeyEdit& edit : edits) {
    changed |= viewport.ReplaceRange(edit.range, edit.Text(indent_unit), true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  const SelectionRange& primary = final_ranges.front();
  viewport.MoveCursorTo(primary.start.line, primary.start.column, false);
  if (primary.end != primary.start) {
    viewport.MoveCursorTo(primary.end.line, primary.end.column, true);
  }
  if (final_ranges.size() > 1) {
    viewport.SetSecondaryCaretsWithRanges(
        std::span<const SelectionRange>(final_ranges).subspan(1));
  }
  viewport.EndUndoGroup();
  return true;
}

bool OutdentSelection(TextViewport& viewport) {
  const std::size_t indent_width = viewport.indent_width() == 0 ? 4 : viewport.indent_width();
  return ReindentRegions(
      viewport, [&](std::string_view source, std::string* out) -> std::ptrdiff_t {
        std::size_t strip = 0;
        if (!source.empty()) {
          if (source[0] == '\t') {
            strip = 1;
          } else {
            while (strip < indent_width && strip < source.size() && source[strip] == ' ') ++strip;
          }
        }
        out->assign(source.substr(strip));
        return -static_cast<std::ptrdiff_t>(strip);
      });
}

bool SortLines(TextViewport& viewport, bool ascending) {
  std::vector<LineRange> regions = ResolveLineRanges(viewport);
  std::erase_if(regions, [](const LineRange& r) { return r.first == r.last; });
  if (regions.empty()) return false;
  const TextBuffer& lines = viewport.lines();

  std::vector<std::uint32_t> order;
  LineBlob region_lines;
  LineBlob sorted;
  const auto sort_region = [&](const LineRange& region) {
    region_lines.clear();
    lines.AppendLines(region.first, region.last + 1, region_lines);
    order.clear();
    order.reserve(region_lines.size());
    for (std::uint32_t i = 0; i < region_lines.size(); ++i) {
      order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                return ascending ? region_lines[a] < region_lines[b]
                                 : region_lines[b] < region_lines[a];
              });
    sorted.clear();
    sorted.reserve_lines(order.size());
    sorted.reserve_bytes(region_lines.content_bytes());
    for (const std::uint32_t index : order) {
      sorted.push_back(region_lines[index]);
    }
    return viewport.ReplaceLines(region.first, region.last + 1, sorted,
                                 true);
  };

  const bool had_selection = viewport.has_selection();
  const bool multi = viewport.has_multiple_carets();
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  const auto restore = [&] {
    if (multi || !had_selection) {
      RegionColumnDeltas no_deltas;
      for (const LineRange& region : regions) {
        no_deltas.BeginRegion();
        for (std::size_t i = region.first; i <= region.last; ++i) no_deltas.flat.push_back(0);
      }
      RestoreCaretsAfterIndentEdit(viewport, snapshot, regions, no_deltas);
    } else {
      RestoreSelectionAcrossLines(viewport, regions.front().first, regions.front().last,
                                  had_selection);
    }
  };
  if (regions.size() == 1) {
    if (!sort_region(regions.front())) return false;
    restore();
    return true;
  }
  bool changed = false;
  viewport.BeginUndoGroup();
  for (const LineRange& region : regions) {
    changed |= sort_region(region);
  }
  if (changed) restore();
  viewport.EndUndoGroup();
  return changed;
}

}
