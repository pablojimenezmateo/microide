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
  std::size_t last = 0;  // inclusive

  std::size_t line_count() const { return last - first + 1; }
};

// The line span one caret asks a shaping op to touch: its selection's lines when
// it has one, its own line when it does not. A selection that ends at column 0 of
// a line has not reached into that line, so it is excluded — that is what makes a
// whole-line drag select N lines rather than N+1.
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

// Every line region the carets ask a shaping op to touch, as a SORTED SET OF
// DISJOINT ranges — one per caret, with overlapping-or-touching neighbours merged.
//
// This used to be a single `min..max` span over every caret, and the ops rewrote
// every line in it. Carets on lines 10 and 100 with no selection therefore made
// `Ctrl+/` comment all 91 lines; VSCode comments two (TD-2026-08-07-160). It was a
// fidelity bug first and a cost bug second, and the disjoint set fixes both: each
// op now emits one edit per region inside an undo group, which the group frame
// already aggregates into the multi-range undo entry TD-2026-08-06-157 shipped.
//
// Touching ranges are merged so two carets on adjacent lines produce one edit
// rather than two abutting ones, and so two carets on the SAME line collapse
// instead of applying an op to that line twice.
// `expand_collapsed_folds`: a bare caret on the opener of a collapsed fold names
// the whole fold, as it does for the verbs that move, duplicate, cut or delete
// "the line" in VS Code -- the block is one row on screen. Indent, comment and
// sort keep acting on the opener alone, as VS Code's do.
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
  // A ranged secondary contributes the lines its anchor spans as well as the ones
  // under its cursor (A-120): a Ctrl-D selection whose anchor sits on a different
  // line than its cursor must not be partially missed.
  for (const SecondaryCaret& secondary : secondaries) {
    ranges.push_back(secondary.selection_anchor.has_value()
                         ? RangeForCaret(*secondary.selection_anchor, secondary.position)
                         : caret_range(secondary.position.line));
  }

  if (line_count == 0) {
    ranges.clear();
    return ranges;
  }
  // Clamp to the buffer, then drop anything that started past its end. Clamping
  // first is deliberate: a stale caret one line past EOF still means "the last
  // line", the same answer the single-span form gave.
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
    // `<= last + 1` rather than `<= last`: adjacent regions merge, so a run of
    // per-line carets is one edit and not one per line.
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

// If `content` — ignoring leading/trailing horizontal whitespace — is already
// wrapped in `open`…`close`, returns the un-wrapped text with the surrounding
// whitespace preserved. Otherwise returns nullopt. This makes ToggleBlockComment
// a true toggle instead of nesting `/* /* x */ */` on repeat. The one padding
// space the wrap puts inside each marker (`/* x */`, as VS Code writes it) comes
// off with the marker; any other whitespace inside stays.
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

// The position just past `text` inserted at `start`.
TextPosition EndOfInsertion(TextPosition start, std::string_view text) {
  const std::size_t last_newline = text.rfind('\n');
  if (last_newline == std::string_view::npos) {
    return TextPosition{start.line, start.column + text.size()};
  }
  const std::size_t newlines =
      static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  return TextPosition{start.line + newlines, text.size() - last_newline - 1};
}

// Replaces `range` with `text` and leaves [`select_from`, `select_to`) of the
// inserted text selected, so a second toggle sees what the first one made.
bool ReplaceAndSelect(TextViewport& viewport,
                      const SelectionRange& range,
                      const std::string& text,
                      std::size_t select_from,
                      std::size_t select_to) {
  if (!viewport.ReplaceRange(range, text, /*record_undo=*/true)) {
    return false;
  }
  const std::string_view inserted = text;
  const TextPosition from = EndOfInsertion(range.start, inserted.substr(0, select_from));
  const TextPosition to = EndOfInsertion(range.start, inserted.substr(0, select_to));
  viewport.MoveCursorTo(from.line, from.column, false);
  viewport.MoveCursorTo(to.line, to.column, true);
  return true;
}

// The wrap ToggleBlockComment leaves the inner text selected, so the next toggle
// sees a selection whose markers sit just OUTSIDE it (a padding space allowed).
// Returns the range widened over those markers, or nullopt.
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

}  // namespace

namespace {

// Comment/uncomment ONE contiguous region into `updated`, which is cleared first
// (the caller hands the same vector to ReplaceLines, which takes it by value, so
// it comes back moved-from). Returns false when the region has no non-blank line —
// there is nothing to toggle, and a region that contributes nothing must not
// contribute an empty edit either.
//
// The add/remove decision and the insertion column are computed PER REGION, which
// is what VSCode does (one LineCommentCommand per selection): a caret in a
// commented block and a caret in an uncommented one each toggle their own way,
// and a deeply indented region keeps its own alignment instead of being commented
// at some unrelated region's indent.
bool BuildToggledCommentRegion(const TextBuffer& lines,
                               LineRange range,
                               std::string_view marker,
                               LineBlob* updated) {
  bool all_commented = true;
  bool any_non_blank = false;
  std::size_t min_indent = std::string::npos;
  std::size_t content_bytes = 0;
  for (std::size_t i = range.first; i <= range.last; ++i) {
    // LineView, not lines[i]: LineRef copies the line and interns it in the
    // buffer's line cache, so the two passes below cost two allocations per line
    // before any of the work the toggle actually needs (TD-2026-08-06-159).
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
  // Size the blob's byte buffer too. Without it the region's bytes are appended
  // onto std::string's doubling curve -- ~12 reallocations for a 300 KB region,
  // each copying everything appended so far -- and the pass above has already
  // measured the exact input. Commenting adds `marker + ' '` per line and
  // uncommenting only removes, so this bound holds either way.
  updated->reserve_bytes(content_bytes + range.line_count() * (marker.size() + 1));
  for (std::size_t i = range.first; i <= range.last; ++i) {
    const std::string_view line = lines.LineView(i);
    if (LineIsEmptyOrWhitespace(line)) {
      updated->push_back(line);
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
      // Straight into the blob: the toggled line no longer needs an owned string
      // of its own, which was the per-line allocation TD-2026-08-11-182 named.
      updated->push_joined(line.substr(0, lead), line.substr(pos));
    } else {
      updated->push_joined(line.substr(0, min_indent), marker, std::string_view(" "),
                           line.substr(min_indent));
    }
  }
  return true;
}

}  // namespace

namespace {

// A selection ending at column 0 of the line BELOW its last content line is what a
// whole-line drag produces, and every line op already normalizes it away
// (RangeForCaret: "a whole-line drag selects N lines rather than N+1"). The block
// toggle has to as well, or the closing marker lands at the start of a line the
// user never selected and joins it to the comment. VS Code shrinks the selection
// the same way.
SelectionRange ShrinkTrailingLineStart(const TextViewport& viewport, SelectionRange range) {
  if (range.end.line > range.start.line && range.end.column == 0) {
    range.end.line -= 1;
    range.end.column = viewport.lines().LineLength(range.end.line);
  }
  return range;
}

// Every caret's own target range for a block-comment toggle: its selection when
// it has one, else its whole line. VS Code toggles each cursor's region
// independently, and every other shaping verb in this file already unions the
// caret set through ResolveLineRanges -- ToggleBlockComment was the one that read
// only the primary, so with three cursors it wrapped one line and left the other
// two alone.
//
// Descending, and overlapping regions dropped: the edits apply highest-first so
// the ones below stay valid, and two carets on one line must not wrap it twice.
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

  // Each region runs the SAME three-way decision the single-caret path makes --
  // strip an existing wrap, un-wrap markers sitting just outside the selection, or
  // wrap -- and leaves the inner text SELECTED. That last part is what makes the
  // toggle its own inverse: the second press has to see the ranges the first press
  // made, and a caret set restored as bare positions would instead act on whole
  // lines and nest a second pair of markers inside the first. The invariant sweep
  // caught exactly that (`/* also /* short */ */`).
  //
  // Regions are disjoint and applied highest-first, so each one's resulting range
  // is computable locally: the edits below it have not happened yet and the edits
  // above it did not touch its lines.
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
    if (!viewport.ReplaceRange(target, text, /*record_undo=*/true)) {
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

  // `results` is in descending document order (regions were). The primary takes
  // the first region in READING order so the caret ends up where the user's
  // primary was, and the rest become ranged secondaries.
  const SelectionRange primary = results.back();
  viewport.MoveCursorTo(primary.start.line, primary.start.column, false);
  viewport.MoveCursorTo(primary.end.line, primary.end.column, true);
  results.pop_back();
  viewport.SetSecondaryCaretsWithRanges(results);
  return true;
}

}  // namespace

bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close) {
  if (open.empty() || close.empty()) return false;
  // More than one caret: each one toggles its own region, as VS Code does. The
  // single-caret path below is left exactly as it was -- it keeps the toggled text
  // SELECTED so a second press strips instead of nesting, which cannot be done for
  // N regions at once and is not worth losing for the one that matters.
  if (viewport.has_multiple_carets()) {
    return ToggleBlockCommentAtEveryCaret(viewport, open, close);
  }
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
    replacement.reserve(open.size() + line.size() + close.size() + 2);
    replacement.append(open).push_back(' ');
    replacement.append(line).push_back(' ');
    replacement.append(close);
    return viewport.ReplaceRange(r, replacement, /*record_undo=*/true);
  }
  SelectionRange n = *sel;
  if (n.start.line > n.end.line ||
      (n.start.line == n.end.line && n.start.column > n.end.column)) {
    std::swap(n.start, n.end);
  }
  n = ShrinkTrailingLineStart(viewport, n);
  // TextInRange, not SelectedText(): the range above may be SHORTER than the live
  // selection now, and wrapping the text of a range you are not replacing writes
  // the trailing newline back into the middle of the comment.
  std::string content = viewport.TextInRange(n);
  if (auto stripped = TryStripBlockComment(content, open, close)) {
    return ReplaceAndSelect(viewport, n, *stripped, 0, stripped->size());
  }
  // The selection a wrap leaves behind is the inner text; its markers sit just
  // outside. Strip those, keeping the text selected.
  if (const auto outer = RangeOverSurroundingMarkers(viewport, n, open, close)) {
    return ReplaceAndSelect(viewport, *outer, content, 0, content.size());
  }
  // `/* text */`, as VS Code writes it, with the text kept selected so the next
  // toggle strips instead of nesting.
  std::string wrapped;
  wrapped.reserve(open.size() + content.size() + close.size() + 2);
  wrapped.append(open).push_back(' ');
  wrapped.append(content).push_back(' ');
  wrapped.append(close);
  return ReplaceAndSelect(viewport, n, wrapped, open.size() + 1,
                          open.size() + 1 + content.size());
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

// The region containing `line`, or nullptr. Regions are sorted and disjoint, so
// this is a binary search — a linear scan here would be O(carets x regions), and
// both are the caret count on a select-all-occurrences edit.
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

// Restore every caret and selection after a whole-line transform, given the two
// line remaps that transform implies. `shift` maps a line INSIDE a region;
// `shift_boundary` maps the exclusive end line of a whole-line selection, which
// is the line just past a region and therefore needs its own rule.
//
// A line move and a line copy differ only in those two functions, so they share
// this body rather than each owning a copy of the selection-covers-the-block
// reasoning below.
template <typename ShiftFn, typename ShiftBoundaryFn>
void RestoreCaretsAfterLineTransform(TextViewport& viewport,
                                     const LineMoveCaretSnapshot& snapshot,
                                     std::span<const LineRange> regions,
                                     ShiftFn&& shift,
                                     ShiftBoundaryFn&& shift_boundary) {
  viewport.ClearSecondaryCarets();
  // The primary selection lives inside exactly one region (it is what produced
  // it), so "covers the block" is a question about that region alone.
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
    // A whole-line selection names its exclusive end as column 0 of the line
    // AFTER the block. When the transform lands the block against the bottom of
    // the buffer there is no such line, and `MoveCursorTo` clamps the row to the
    // last line while keeping column 0 — which reads back through RangeForCaret
    // as "the selection stops one line short", so the NEXT line op acts on one
    // line less than the last one moved. Select all in a file that ends with a
    // newline, then Alt+Down / Alt+Up: the buffer came back with a stray blank
    // line and its last line stranded below the block. Express the same
    // selection the only way the buffer can — through the end of its last line.
    const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
    if (end_line > last_line) {
      end_line = last_line;
      end_column = viewport.lines().LineLength(last_line);
    }
    viewport.MoveCursorTo(end_line, end_column, /*extend_selection=*/true);
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

void RestoreCaretsAfterLineMove(TextViewport& viewport,
                                const LineMoveCaretSnapshot& snapshot,
                                std::span<const LineRange> moved,
                                std::ptrdiff_t delta) {
  const auto shift = [&](std::size_t line) -> std::size_t {
    if (RegionContaining(moved, line) == nullptr) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  // A whole-line selection ends at column 0 of the line AFTER the block, so its
  // end.line is region.last + 1 (ResolveLineRanges excludes that trailing line
  // from the moved region). That exclusive boundary moved with the block, so
  // extend the shiftable range by one for the end. Without this the guard below
  // rejected the selection and the whole thing fell to the single-caret branch,
  // silently dropping the selection after the move.
  const auto shift_boundary = [&](std::size_t line) -> std::size_t {
    if (RegionContaining(moved, line, /*include_exclusive_end=*/true) == nullptr) return line;
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  RestoreCaretsAfterLineTransform(viewport, snapshot, moved, shift, shift_boundary);
}

// The moved block plus the neighbour it swaps with, as one replacement vector.
// One piece-tree walk for the block: the per-line `lines[i]` this replaced goes
// through TextBuffer::LineRef, which materialises a string AND inserts it into the
// buffer's line cache.
LineBlob BuildLineMoveReplacement(const TextBuffer& lines, LineRange range, bool downward) {
  LineBlob updated;
  updated.reserve_lines(range.line_count() + 1);
  if (downward) {
    updated.push_back(lines.LineView(range.last + 1));
  }
  // Appended, not sliced-then-moved: SliceLines' return vector was one heap
  // allocation per caret per keystroke for a vector that only ever fed this one.
  lines.AppendLines(range.first, range.last + 1, updated);
  if (!downward) {
    updated.push_back(lines.LineView(range.first - 1));
  }
  return updated;
}

// Shared body of MoveLineUp / MoveLineDown.
//
// Regions that cannot move (a region already at the top of the buffer for an
// up-move, at the bottom for a down-move) are dropped and the rest still move,
// which is what VSCode does — each selection gets its own MoveLinesCommand and a
// pinned one no-ops alone.
//
// Regions are disjoint with at least one line between them (ResolveLineRanges
// merges touching ones), and each edit reaches exactly one line beyond its region
// in the direction of travel, so no two region edits overlap and the line count is
// preserved. That is what lets them be applied in plain ascending order.
bool MoveLines(TextViewport& viewport, bool downward) {
  std::vector<LineRange> regions = ResolveLineRanges(viewport, /*expand_collapsed_folds=*/true);
  const TextBuffer& lines = viewport.lines();
  const std::size_t line_count = lines.size();
  std::erase_if(regions, [&](const LineRange& r) {
    return downward ? r.last + 1 >= line_count : r.first == 0;
  });
  if (regions.empty()) return false;

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // Wrap the replaces + caret restore in one undo group so the aggregate entry's
  // after_state is captured AFTER RestoreCaretsAfterLineMove — otherwise the
  // ReplaceLines entry snaps after_state to (range_first, 0) and redo loses the
  // real column and every secondary caret.
  viewport.BeginUndoGroup();
  bool changed = false;
  for (const LineRange& range : regions) {
    LineBlob updated = BuildLineMoveReplacement(lines, range, downward);
    const std::size_t start = downward ? range.first : range.first - 1;
    const std::size_t end = downward ? range.last + 2 : range.last + 1;
    changed |= viewport.ReplaceLines(start, end, std::move(updated), /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  RestoreCaretsAfterLineMove(viewport, snapshot, regions, downward ? +1 : -1);
  viewport.EndUndoGroup();
  return true;
}

}  // namespace

bool JoinLinesAtCarets(TextViewport& viewport) {
  const TextBuffer& lines = viewport.lines();
  if (lines.size() == 0) return false;
  // Whole-line regions, one per caret, merged -- the same set every other line
  // verb acts on, so two carets on adjacent lines join once rather than fighting.
  // Collapsed folds are NOT expanded: joining a folded block onto one line is a
  // destructive edit the user cannot see the result of, and VS Code does not do it.
  std::vector<LineRange> regions = ResolveLineRanges(viewport);
  if (regions.empty()) return false;

  // A region of ONE line joins with the line below it; that is what makes a
  // repeated press pull a block up a line at a time. A region already spanning
  // lines joins exactly what it covers.
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
  // Descending: a join removes lines, so applying later regions first leaves the
  // earlier regions' indices valid.
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    std::string joined(lines.LineView(region->first));
    for (std::size_t line = region->first + 1; line <= region->last; ++line) {
      const std::string_view text = lines.LineView(line);
      const std::size_t content = LeadingWhitespaceCount(text);
      const std::string_view trimmed = text.substr(content);
      if (trimmed.empty()) {
        continue;  // an empty line contributes nothing, not a stray space
      }
      // One separating space, and never a second: a first line that already ends
      // in whitespace (or is empty) needs none.
      if (!joined.empty() && joined.back() != ' ' && joined.back() != '\t') {
        joined.push_back(' ');
      }
      joined.append(trimmed);
    }
    LineBlob replacement;
    replacement.reserve_lines(1);
    replacement.push_back(joined);
    changed |= viewport.ReplaceLines(region->first, region->last + 1, std::move(replacement),
                                     /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  // Every region above a line collapsed to one, so a caret below slides up by the
  // lines those regions swallowed; a caret inside a region lands on the joined
  // line, which is the region's first.
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
      // The exclusive end of a whole-line selection sits one past its region, so
      // it lands one past the joined line.
      [&](std::size_t line) { return line == 0 ? line : shift_by(line - 1) + 1; });
  viewport.EndUndoGroup();
  return true;
}

bool MoveLineUp(TextViewport& viewport) { return MoveLines(viewport, /*downward=*/false); }

bool MoveLineDown(TextViewport& viewport) { return MoveLines(viewport, /*downward=*/true); }

bool CopyLines(TextViewport& viewport, bool downward) {
  const std::vector<LineRange> regions =
      ResolveLineRanges(viewport, /*expand_collapsed_folds=*/true);
  const TextBuffer& lines = viewport.lines();
  if (regions.empty() || lines.size() == 0) return false;

  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);
  // One undo group for the edits plus the caret restore, for the same reason
  // MoveLines needs one: the per-region ReplaceLines entry snaps after_state to
  // (region_first, 0), so redo would lose the real column and every secondary.
  viewport.BeginUndoGroup();
  bool changed = false;
  // Descending: a region's insertion shifts every line after it, so applying the
  // later regions first leaves the earlier regions' indices still valid.
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    LineBlob updated;
    updated.reserve_lines(region->line_count() * 2);
    lines.AppendLines(region->first, region->last + 1, updated);
    lines.AppendLines(region->first, region->last + 1, updated);
    changed |= viewport.ReplaceLines(region->first, region->last + 1, std::move(updated),
                                     /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  // Every region above `line` has doubled, so a caret below them slides down by
  // the total inserted; a caret inside a region slides only when the copy went
  // downward (VS Code: copyLinesDown leaves the caret on the lower copy,
  // copyLinesUp on the upper one -- which is the same text, a different caret).
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
      [&](std::size_t line) { return shift_by(line, /*include_own_region=*/true); },
      // The exclusive end of a whole-line selection sits one past its region, so
      // it must slide with that region rather than counting it as "above".
      [&](std::size_t line) { return shift_by(line == 0 ? line : line - 1, true) +
                                     (line == 0 ? 0 : 1); });
  viewport.EndUndoGroup();
  return true;
}

bool InsertLineBelow(TextViewport& viewport) {
  const TextBuffer& lines = viewport.lines();
  const std::size_t line = viewport.cursor_line();
  if (line >= lines.size()) return false;
  // End-of-line then Enter, which is exactly what the action means and gets the
  // language's smart indent (and its brace bump) for free rather than
  // reimplementing AutoIndentForNewline here. EVERY caret goes to the end of
  // its own line first: the multi-caret Enter opens a line below each of them
  // (VS Code's lineInsertAfter), where moving only the primary left the
  // secondaries mid-line and Enter split their lines in two.
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
  // The new line takes the indent of the line it is pushing down, which is what
  // makes Ctrl+Shift+Enter inside a block land at the block's depth. Deliberately
  // NOT the smart-indent form: the line below has not been opened by anything.
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
  // One new line above each caret's line (carets sharing a line share it), each
  // with that line's own indent. Inserting in ascending order means the k-th
  // target has slid down k lines by the time it is reached.
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
                                /*record_undo=*/true);
  }
  viewport.ClearSecondaryCarets();
  viewport.MoveCursorTo(landed[primary_slot].line, landed[primary_slot].column);
  landed.erase(landed.begin() + static_cast<std::ptrdiff_t>(primary_slot));
  viewport.SetSecondaryCarets(std::move(landed));
  viewport.EndUndoGroup();
  return true;
}

bool DeleteLine(TextViewport& viewport) {
  // No caret has a selection: every caret names its own line (and the whole
  // block when it sits on a collapsed fold's opener), which is exactly what the
  // viewport's own path deletes. Delegate to it rather than reimplementing its
  // tuned multi-caret aggregate undo entry -- and because Cut-with-nothing-
  // selected calls the same method and must keep behaving identically.
  const std::span<const SecondaryCaret> secondaries = viewport.secondary_caret_range_view();
  const bool any_selection =
      viewport.has_selection() ||
      std::any_of(secondaries.begin(), secondaries.end(),
                  [](const SecondaryCaret& caret) { return caret.selection_anchor.has_value(); });
  if (!any_selection) {
    return viewport.DeleteCurrentLine();
  }

  // With a selection, Delete Line deletes every line the selection TOUCHES
  // (VS Code's Ctrl+Shift+K), which is what `ResolveLineRanges` already means and
  // what every other line op in this file already does with it. DeleteLine was
  // the one verb that still deleted only `cursor_line_`, so Ctrl+A then
  // Ctrl+Shift+K removed a single line -- the phantom line after the final
  // newline -- out of the middle of the selection and left the rest of the
  // buffer standing.
  const std::vector<LineRange> regions =
      ResolveLineRanges(viewport, /*expand_collapsed_folds=*/true);
  const TextBuffer& lines = viewport.lines();
  if (regions.empty() || lines.size() == 0) {
    return false;
  }

  viewport.BeginUndoGroup();
  bool changed = false;
  // Descending, for the same reason CopyLines walks backwards: deleting a region
  // shifts every line after it, so applying the later regions first leaves the
  // earlier regions' indices valid. `lines` is the live buffer, so the size and
  // length reads below see each deletion as it lands.
  for (auto region = regions.rbegin(); region != regions.rend(); ++region) {
    const std::size_t last = std::min(region->last, lines.size() - 1);
    const std::size_t first = std::min(region->first, last);
    SelectionRange range;
    if (last + 1 < lines.size()) {
      // Take the line break that FOLLOWS the block, so everything below moves up.
      range = SelectionRange{TextPosition{first, 0}, TextPosition{last + 1, 0}};
    } else if (first > 0) {
      // Last block in the buffer: take the break that PRECEDES it instead, or the
      // delete leaves the trailing empty line the block used to end with.
      range = SelectionRange{TextPosition{first - 1, lines.LineLength(first - 1)},
                             TextPosition{last, lines.LineLength(last)}};
    } else {
      // The block is the whole buffer: there is no break to take, so empty the
      // text and let the document keep its one remaining (blank) line.
      range = SelectionRange{TextPosition{0, 0}, TextPosition{last, lines.LineLength(last)}};
    }
    changed |= viewport.ReplaceRange(range, "", /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  // The selection and every secondary caret named lines that no longer exist.
  // Land a single caret at the start of the line that took the first deleted
  // block's place, which is where the viewport's own delete path leaves it.
  viewport.ClearSecondaryCarets();
  viewport.ClearSelection();
  viewport.MoveCursorTo(std::min(regions.front().first, lines.size() - 1), 0);
  viewport.EndUndoGroup();
  return true;
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

// Per-line column deltas for a SET of disjoint regions, concatenated into one
// flat run in region order. Two vectors for the whole edit rather than two per
// region, which matters because "one region per caret" is the point of the
// disjoint form and a select-all-occurrences edit has thousands of them.
struct RegionColumnDeltas {
  std::vector<std::ptrdiff_t> flat;
  // Index into `flat` where each region's run starts. Stored rather than derived
  // so a lookup stays O(log regions) instead of O(regions).
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

// Restore carets after an indent/outdent that changed each line's leading
// whitespace IN PLACE (lines are not reordered). Each caret's column shifts by
// the delta applied to its line (clamped at 0 and the new line length); the
// primary selection and every secondary caret shift the same way. Used for the
// multi-caret and single-caret-no-selection paths, which the selection-only
// RestoreSelectionAcrossLines could not carry — it dropped every secondary caret
// and snapped the primary to (first_line, 0).
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

// Shared body of IndentSelection / OutdentSelection: a per-line leading-whitespace
// transform applied to each resolved region.
//
// `transform(source, out)` writes the replacement line into `out` and returns the
// column delta it applied; a zero delta means the line is unchanged, and a region
// where every line is unchanged makes no edit at all.
//
// Regions are visited in ascending order: every transform here rewrites a line in
// place, so the edit preserves the line count and no region's indices move when
// another region is applied.
template <typename Transform>
bool ReindentRegions(TextViewport& viewport, Transform&& transform) {
  const std::vector<LineRange> regions = ResolveLineRanges(viewport);
  if (regions.empty()) return false;
  const TextBuffer& lines = viewport.lines();

  const bool had_selection = viewport.has_selection();
  const bool multi = viewport.has_multiple_carets();
  const LineMoveCaretSnapshot snapshot = SnapshotCaretsForLineMove(viewport);

  RegionColumnDeltas deltas;
  std::size_t total_lines = 0;
  for (const LineRange& region : regions) {
    total_lines += region.line_count();
  }
  deltas.flat.reserve(total_lines);
  deltas.region_offset.reserve(regions.size());

  LineBlob updated;
  // One reusable scratch for the transform's output, appended into the blob and
  // then reset: `std::string out` inside the loop was one allocation per line of
  // the region, per region (TD-2026-08-11-182).
  std::string out;
  bool changed = false;
  viewport.BeginUndoGroup();
  for (const LineRange& region : regions) {
    deltas.BeginRegion();
    updated.clear();
    updated.reserve_lines(region.line_count());
    bool region_changed = false;
    for (std::size_t i = region.first; i <= region.last; ++i) {
      // LineView, not lines[i]: LineRef would also insert every line of the
      // region into the buffer's line cache (see BuildLineMoveReplacement).
      out.clear();
      const std::ptrdiff_t delta = transform(lines.LineView(i), &out);
      deltas.flat.push_back(delta);
      region_changed |= delta != 0;
      updated.push_back(out);
    }
    if (!region_changed) continue;
    changed |= viewport.ReplaceLines(region.first, region.last + 1, std::move(updated),
                                     /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }
  // Grouping the replaces WITH the caret restore is what makes the aggregate undo
  // entry's after_state capture the restored carets; without it redo snaps to
  // (first, 0) and drops every secondary caret.
  if (multi || !had_selection) {
    RestoreCaretsAfterIndentEdit(viewport, snapshot, regions, deltas);
  } else {
    // A plain single selection is exactly one region, and this path is only
    // reachable with no secondary carets.
    RestoreSelectionAcrossLines(viewport, regions.front().first, regions.front().last,
                                had_selection);
  }
  viewport.EndUndoGroup();
  return true;
}

}  // namespace

bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker) {
  if (line_marker.empty()) return false;
  const std::vector<LineRange> ranges = ResolveLineRanges(viewport);
  if (ranges.empty()) return false;
  const TextBuffer& lines = viewport.lines();
  // The carets and selection survive the toggle, columns shifted by what each
  // line gained or lost (VS Code). ReplaceLines alone snapped the caret to the
  // start of the region, so a second toggle on a selection acted on one line.
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
  // One region is one undo entry on its own; a group only for several.
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
                                     /*record_undo=*/true);
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
          // An empty line gains no indent, so it also gains no column shift.
          return 0;
        }
        out->reserve(indent_unit.size() + source.size());
        out->append(indent_unit);
        out->append(source);
        return static_cast<std::ptrdiff_t>(indent_unit.size());
      });
}

namespace {

// VS Code: a single-line selection is replaced by the indent unless it runs
// from column 1 to the line's max column, in which case the line is shifted.
bool SelectionIndentsAsBlock(const TextViewport& viewport, const SelectionRange& range) {
  if (range.start.line != range.end.line) {
    return true;
  }
  return range.start.column == 0 && range.end.column > 0 &&
         range.start.line < viewport.line_count() &&
         range.end.column >= viewport.lines().LineLength(range.start.line);
}

}  // namespace

namespace {

// One cursor's share of a Tab, in pre-edit coordinates. `block` sites shift every
// line their range covers; the rest replace their own range with a tab to the next
// stop at their own column.
struct TabKeySite {
  SelectionRange range;
  bool block = false;
};

SelectionRange NormalizedCaretRange(TextPosition anchor, TextPosition cursor) {
  const bool anchor_first =
      anchor.line < cursor.line || (anchor.line == cursor.line && anchor.column <= cursor.column);
  return anchor_first ? SelectionRange{anchor, cursor} : SelectionRange{cursor, anchor};
}

// The primary first, then every secondary, in the viewport's own order. The
// primary stays first so the merged caret set can put it back as the primary.
//
// A visitor rather than a vector because `ClassifyTabKey` runs on EVERY Tab press
// and only needs to know whether the sites disagree — materializing them there
// would put an allocation on a keystroke path that had none.
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

}  // namespace

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

  // Every line a block site shifts, deduped: two block sites may cover one line,
  // and it must gain one indent, not two.
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

  // One replacement per edit site, in pre-edit coordinates. A block line's edit is
  // an insert of the indent unit at column 0; a point site replaces its own range
  // with the padding that reaches ITS next tab stop.
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
    // An empty line gains no indent, exactly as IndentSelection's transform says.
    if (lines.LineLength(line) == 0) {
      continue;
    }
    edits.push_back({SelectionRange{TextPosition{line, 0}, TextPosition{line, 0}},
                     indent_unit.size(), /*indent_unit_text=*/true, {}});
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
    edits.push_back({range, text.size(), /*indent_unit_text=*/false, std::move(text)});
  }
  if (edits.empty()) {
    return false;
  }

  // Ascending by (line, column) so `remap` can binary-search to a line and walk
  // only that line's edits. Scanning the whole edit list per caret instead is
  // sites x edits, and both are unbounded on the same gesture: an
  // add-cursor-at-all-matches run over a large file can put thousands of carets
  // in the set while one of them selects thousands of lines.
  std::sort(edits.begin(), edits.end(), [](const TabKeyEdit& a, const TabKeyEdit& b) {
    if (a.range.start.line != b.range.start.line) {
      return a.range.start.line < b.range.start.line;
    }
    return a.range.start.column < b.range.start.column;
  });

  // Where a pre-edit position ends up. Only same-line edits that end at or before
  // the position can move it: the edits are disjoint, none inserts a line break,
  // and a block insert at column 0 is exactly the `shift_col` rule the pure-block
  // path already applies to every caret on a shifted line.
  const auto remap = [&](TextPosition position) {
    position = clamp_position(position);
    const auto first = std::lower_bound(
        edits.begin(), edits.end(), position.line,
        [](const TabKeyEdit& edit, std::size_t line) { return edit.range.start.line < line; });
    std::ptrdiff_t delta = 0;
    for (auto it = first; it != edits.end() && it->range.start.line == position.line; ++it) {
      if (it->range.end.column > position.column) {
        // Ascending within the line, so nothing after this one can qualify either.
        break;
      }
      delta += static_cast<std::ptrdiff_t>(it->insert_length) -
               static_cast<std::ptrdiff_t>(it->range.end.column - it->range.start.column);
    }
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(position.column) + delta;
    position.column = shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
    return position;
  };

  // Final carets, computed from the pre-edit document before anything moves.
  std::vector<SelectionRange> final_ranges;
  final_ranges.reserve(sites.size());
  for (const TabKeySite& site : sites) {
    if (site.block) {
      final_ranges.push_back(SelectionRange{remap(site.range.start), remap(site.range.end)});
      continue;
    }
    // A point site's caret lands just past what it inserted, which is what remap
    // of its range END reports (its own edit is the last one counted there).
    const TextPosition caret = remap(site.range.end);
    final_ranges.push_back(SelectionRange{caret, caret});
  }

  // Highest-first, so every edit still standing is expressed in coordinates the
  // document has not moved yet. Already sorted ascending for `remap`, so this is
  // a reverse rather than a second sort.
  std::reverse(edits.begin(), edits.end());

  viewport.BeginUndoGroup();
  // The secondaries are rebuilt below; leaving them installed would have every
  // ReplaceRange adjust a set that is about to be replaced wholesale.
  viewport.ClearSecondaryCarets();
  bool changed = false;
  for (const TabKeyEdit& edit : edits) {
    changed |= viewport.ReplaceRange(edit.range, edit.Text(indent_unit), /*record_undo=*/true);
  }
  if (!changed) {
    viewport.EndUndoGroup();
    return false;
  }

  // Grouping the caret restore WITH the edits is what makes the aggregate entry's
  // after_state carry the restored carets, exactly as ReindentRegions does.
  const SelectionRange& primary = final_ranges.front();
  viewport.MoveCursorTo(primary.start.line, primary.start.column, /*extend_selection=*/false);
  if (primary.end != primary.start) {
    viewport.MoveCursorTo(primary.end.line, primary.end.column, /*extend_selection=*/true);
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
  // A one-line region has nothing to sort. That is also the pre-existing
  // single-caret-no-selection answer, generalised: sorting is a range operation,
  // so a bare caret contributes no range.
  std::erase_if(regions, [](const LineRange& r) { return r.first == r.last; });
  if (regions.empty()) return false;
  const TextBuffer& lines = viewport.lines();

  // Sorted by PERMUTATION, not by moving strings: the region is sliced into one
  // blob, an index vector is sorted by the lines it points at, and the result is
  // appended in that order. Three allocations for the whole region instead of one
  // per line for the slice plus whatever the sort's moves cost.
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
                                 /*record_undo=*/true);
  };

  // The selection (or carets) survive the sort, as in VS Code; ReplaceLines
  // alone snapped the caret to the region start.
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

}  // namespace microide::editor
