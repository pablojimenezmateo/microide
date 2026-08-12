#pragma once

// Internal helpers shared between the TextViewport.cpp translation units
// (TextViewport.cpp and TextViewportLanguageBehavior.cpp). Not part of the
// public editor API. The `detail` namespace and the .h naming both signal
// "do not include this from anywhere outside src/editor/TextViewport*.cpp".

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/TextViewport.h"

namespace microide::editor::detail {

// Highlight checkpoint spacing. One SyntaxState is snapshotted every
// `kHighlightCheckpointInterval` lines so resuming highlighting after a jump
// is O(checkpoint-interval) rather than O(line-index). Shared between
// TextViewport.cpp (InvalidateDerivedCaches checkpoint-bookkeeping) and
// TextViewportHighlightCache.cpp (the cache itself).
inline constexpr std::size_t kHighlightCheckpointInterval = 128;

inline bool PositionLess(const TextPosition& lhs, const TextPosition& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line;
  }
  return lhs.column < rhs.column;
}

// Order secondary carets by their caret position. Shared by the
// AddSecondaryCaret / AddSecondaryCaretWithRange / DedupeSecondaryCaretsAgainstPrimary
// sort sites so they cannot drift apart.
inline bool SecondaryCaretPositionLess(const TextViewportUndoHistory::SecondaryCaret& lhs,
                                       const TextViewportUndoHistory::SecondaryCaret& rhs) {
  return PositionLess(lhs.position, rhs.position);
}

inline std::optional<SelectionRange> SelectionRangeForSecondaryCaret(
    const TextPosition& position,
    const std::optional<TextPosition>& selection_anchor) {
  if (!selection_anchor.has_value()) {
    return std::nullopt;
  }
  if (selection_anchor->line == position.line && selection_anchor->column == position.column) {
    return std::nullopt;
  }
  if (PositionLess(*selection_anchor, position)) {
    return SelectionRange{*selection_anchor, position};
  }
  return SelectionRange{position, *selection_anchor};
}

inline TextPosition RangeEndExclusive(const SelectionRange& r) {
  return PositionLess(r.start, r.end) ? r.end : r.start;
}

inline bool ValidateRangeColumns(LineSpan lines, const SelectionRange& n) {
  if (n.start.line >= lines.size() || n.end.line >= lines.size()) {
    return false;
  }
  if (n.start.column > lines[n.start.line].size() ||
      n.end.column > lines[n.end.line].size()) {
    return false;
  }
  return true;
}

inline std::string TextBetweenLines(LineSpan lines, const SelectionRange& n) {
  const auto& a = n.start;
  const auto& b = n.end;
  if (a.line == b.line) {
    return std::string(lines[a.line].substr(a.column, b.column - a.column));
  }
  std::string out;
  out += lines[a.line].substr(a.column);
  out.push_back('\n');
  for (std::size_t i = a.line + 1; i < b.line; ++i) {
    out += lines[i];
    out.push_back('\n');
  }
  out += lines[b.line].substr(0, b.column);
  return out;
}

// Newline / trailing-column counts of a replacement string. Computed once per
// edit so the per-caret remap below is a branch-only update instead of
// re-scanning the replacement for every previously-placed caret (the multi-caret
// fan-out remaps O(k) prior carets per applied edit, so the rescan was
// O(k^2 * |replacement|)).
struct ReplacementShape {
  std::size_t inserted_newlines = 0;
  std::size_t last_segment_cols = 0;
};

inline ReplacementShape ComputeReplacementShape(std::string_view replacement) {
  // Must mirror util::NormalizeLineEndings, which every edit path (e.g.
  // BuildRangeHistoryEntry) applies to the replacement before splitting it into
  // lines: a lone '\r' and a '\r\n' each become a single newline. Counting only
  // literal '\n' would undercount line breaks for pasted lone-CR / reversed
  // content, desyncing the multi-caret remap so higher carets land on the wrong
  // line.
  ReplacementShape shape;
  for (std::size_t i = 0; i < replacement.size(); ++i) {
    const char ch = replacement[i];
    if (ch == '\n') {
      ++shape.inserted_newlines;
      shape.last_segment_cols = 0;
    } else if (ch == '\r') {
      ++shape.inserted_newlines;
      shape.last_segment_cols = 0;
      if (i + 1 < replacement.size() && replacement[i + 1] == '\n') {
        ++i;  // consume the paired '\n' so "\r\n" counts as one newline
      }
    } else {
      ++shape.last_segment_cols;
    }
  }
  return shape;
}

// Maps a caret position forward across one applied edit that replaced the
// (normalized) range [removed_start, removed_end) with a replacement whose
// newline/column shape is `shape`. The multi-caret pipelines walk carets
// high-to-low, so a caret recorded earlier always sits at or after a later
// (lower) edit; remapping keeps positions correct when several carets share a
// line (without it, the higher carets are left stale by the byte/line counts
// inserted below them).
inline TextPosition RemapPositionAfterReplace(TextPosition position,
                                              TextPosition removed_start,
                                              TextPosition removed_end,
                                              const ReplacementShape& shape) {
  // Positions strictly before the end of the removed range are unaffected.
  if (PositionLess(position, removed_end)) {
    return position;
  }

  if (position.line == removed_end.line) {
    TextPosition result;
    result.line = removed_start.line + shape.inserted_newlines;
    const std::size_t tail = position.column - removed_end.column;
    result.column = shape.inserted_newlines == 0
                        ? removed_start.column + shape.last_segment_cols + tail
                        : shape.last_segment_cols + tail;
    return result;
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(shape.inserted_newlines) -
      static_cast<std::ptrdiff_t>(removed_end.line - removed_start.line);
  TextPosition result = position;
  result.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(position.line) + line_delta);
  return result;
}

// One site produced by a multi-caret reverse-apply walk: the caret's own
// post-edit `landed` position (in the coordinate space *before* any lower /
// earlier-caret edit is folded in — exactly what the walk records right after
// applying this caret's edit and before remapping), plus that edit's removed
// range + replacement shape. `has_edit` is false for carets that produced no
// buffer edit (no-op backspace at doc start, auto-close skip, invalid range);
// they still get remapped by lower edits but shift nothing themselves.
struct MultiCaretRemapSite {
  TextPosition landed{};
  std::optional<TextPosition> anchor;  // companion position (selection anchor), remapped too
  bool has_edit = false;
  SelectionRange removed{};
  ReplacementShape shape{};
};

// Fold every lower-index site's edit into each site's `landed` (and `anchor`),
// reproducing the reverse-apply walk's incremental "remap every already-produced
// result after each applied edit" exactly, but without its O(sites^2) inner loop
// in the common case. Sites are in ascending caret order; edits are disjoint and
// sorted (the caller rejects overlaps first).
//
// When every edit removed a single-line range and no site carries an anchor, the
// net effect of all lower edits on a caret is a pure additive line delta plus a
// same-line column delta, computed in one forward pass (O(sites)). Multi-line
// removed ranges or anchors fall back to the exact O(sites^2) remap. Both paths
// yield identical results to the original per-edit remap.
inline void ResolveMultiCaretRemapSites(std::vector<MultiCaretRemapSite>& sites) {
  bool fast_eligible = true;
  for (const MultiCaretRemapSite& site : sites) {
    if (site.anchor.has_value() ||
        (site.has_edit && site.removed.start.line != site.removed.end.line)) {
      fast_eligible = false;
      break;
    }
  }

  if (!fast_eligible) {
    // Exact fallback: site i is remapped by edits i-1, i-2, ..., 0 in that order
    // (matching the reverse walk, which applies higher carets first and remaps
    // each lower edit into the already-recorded higher results).
    for (std::size_t i = 0; i < sites.size(); ++i) {
      for (std::size_t k = i; k-- > 0;) {
        if (!sites[k].has_edit) {
          continue;
        }
        const SelectionRange& removed = sites[k].removed;
        sites[i].landed =
            RemapPositionAfterReplace(sites[i].landed, removed.start, removed.end, sites[k].shape);
        if (sites[i].anchor.has_value()) {
          sites[i].anchor = RemapPositionAfterReplace(*sites[i].anchor, removed.start, removed.end,
                                                      sites[k].shape);
        }
      }
    }
    return;
  }

  // Fast path — single-line removed ranges only, so every edit's line delta is
  // non-negative and a caret's line never decreases across the fold. `acc_line`
  // accumulates every lower edit's line delta (both remap branches add it);
  // `acc_col` accumulates the same-original-line column shift, resetting to 0 at
  // each new original line and rebasing when an edit inserts newlines (its prefix
  // moves to a fresh tail line, so earlier same-line column growth no longer
  // applies to carets past it).
  std::ptrdiff_t acc_line = 0;
  std::ptrdiff_t acc_col = 0;
  std::size_t prev_orig_line = std::numeric_limits<std::size_t>::max();
  for (MultiCaretRemapSite& site : sites) {
    const std::size_t orig_line =
        site.has_edit ? site.removed.start.line : site.landed.line;
    if (orig_line != prev_orig_line) {
      acc_col = 0;
      prev_orig_line = orig_line;
    }
    const std::size_t final_line =
        static_cast<std::size_t>(static_cast<std::ptrdiff_t>(site.landed.line) + acc_line);
    std::size_t final_col = site.landed.column;
    // Apply the same-line column shift only when this caret still sits on its
    // original line — a caret whose own edit inserted newlines landed on a fresh
    // tail line that lower same-line edits do not touch.
    if (site.landed.line == orig_line) {
      final_col = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(final_col) + acc_col);
    }
    site.landed = TextPosition{final_line, final_col};

    if (site.has_edit) {
      acc_line += static_cast<std::ptrdiff_t>(site.shape.inserted_newlines);
      if (site.shape.inserted_newlines == 0) {
        acc_col += static_cast<std::ptrdiff_t>(site.shape.last_segment_cols) -
                   static_cast<std::ptrdiff_t>(site.removed.end.column - site.removed.start.column);
      } else {
        acc_col = static_cast<std::ptrdiff_t>(site.shape.last_segment_cols) -
                  static_cast<std::ptrdiff_t>(site.removed.end.column);
      }
    }
  }
}

// Collects the disjoint before/after images of a multi-caret edit while it is
// applied high-to-low, so the recorded undo entry is the SET of edited ranges
// instead of one range spanning the outermost carets (TD-2026-08-06-157).
//
// Eight carets 8,400 lines apart used to capture 8,401 lines for the before image
// and 8,401 for the after, and retain both, on every keystroke — the cost of one
// keystroke was the DISTANCE between the carets, not their number.
//
// Every capture happens at the moment its footprint is untouched: the walk is
// highest-first, so when a footprint opens, everything above it is already edited
// (and is above, so its indices do not move) and everything below is still
// original. That is what makes `before_start` the true pre-edit index.
class DisjointEditCapture {
 public:
  explicit DisjointEditCapture(const TextBuffer& buffer) : buffer_(&buffer) {}

  // Call immediately BEFORE applying the highest edit of a footprint.
  void BeginFootprint(std::size_t start, std::size_t end_exclusive) {
    open_start_ = std::min(start, buffer_->size());
    open_before_size_ = std::clamp(end_exclusive, open_start_, buffer_->size()) - open_start_;
    document_size_at_begin_ = buffer_->size();
    open_before_lines_ = buffer_->SliceLinesBlob(open_start_, open_start_ + open_before_size_);
    open_ = true;
  }

  // Call immediately AFTER applying the lowest edit of the footprint.
  void EndFootprint() {
    if (!open_) {
      return;
    }
    open_ = false;
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(buffer_->size()) -
                                 static_cast<std::ptrdiff_t>(document_size_at_begin_);
    const std::size_t after_size = static_cast<std::size_t>(std::max<std::ptrdiff_t>(
        0, static_cast<std::ptrdiff_t>(open_before_size_) + delta));
    const std::size_t after_end = std::min(buffer_->size(), open_start_ + after_size);
    parts_.push_back(Captured{
        .before_start = open_start_,
        .before_lines = std::move(open_before_lines_),
        .after_lines = buffer_->SliceLinesBlob(std::min(open_start_, after_end), after_end),
    });
  }

  bool empty() const { return parts_.empty(); }

  // Parts were pushed highest-first; this reverses them into the ascending order
  // the entry stores, trims each one's identical head/tail, and drops any that
  // turned out to be a no-op.
  TextViewportUndoHistory::Entry Build(
      const TextViewportUndoHistory::ViewState& before_state,
      const TextViewportUndoHistory::ViewState& after_state) {
    std::reverse(parts_.begin(), parts_.end());
    TextViewportUndoHistory::Entry aggregate;
    aggregate.before_state = before_state;
    aggregate.after_state = after_state;
    std::ptrdiff_t shift = 0;
    bool have_primary = false;
    for (Captured& part : parts_) {
      TextViewportUndoHistory::Entry trimmed =
          TextViewportUndoHistory::BuildEntryForDocumentChange(
              std::move(part.before_lines), before_state, std::move(part.after_lines), after_state);
      if (trimmed.before_lines.empty() && trimmed.after_lines.empty()) {
        continue;  // this footprint's edits cancelled out
      }
      const std::size_t before_start = part.before_start + trimmed.start_line;
      const std::ptrdiff_t part_delta = static_cast<std::ptrdiff_t>(trimmed.after_lines.size()) -
                                        static_cast<std::ptrdiff_t>(trimmed.before_lines.size());
      if (!have_primary) {
        aggregate.start_line = before_start;
        aggregate.before_lines = std::move(trimmed.before_lines);
        aggregate.after_lines = std::move(trimmed.after_lines);
        have_primary = true;
      } else {
        aggregate.extra_parts.push_back(TextViewportUndoHistory::DisjointPart{
            .before_start = before_start,
            .after_start = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(before_start) + shift),
            .before_lines = std::move(trimmed.before_lines),
            .after_lines = std::move(trimmed.after_lines),
        });
      }
      shift += part_delta;
    }
    return aggregate;
  }

 private:
  struct Captured {
    std::size_t before_start = 0;
    LineBlob before_lines;
    LineBlob after_lines;
  };

  const TextBuffer* buffer_;
  std::vector<Captured> parts_;
  LineBlob open_before_lines_;
  std::size_t open_start_ = 0;
  std::size_t open_before_size_ = 0;
  std::size_t document_size_at_begin_ = 0;
  bool open_ = false;
};

}  // namespace microide::editor::detail
