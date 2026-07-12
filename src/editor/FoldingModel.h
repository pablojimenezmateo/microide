#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"

namespace microide::editor {

class TextViewport;

enum class FoldSource : std::uint8_t {
  Bracket = 0,
  Indent = 1,
};

struct FoldRange {
  std::size_t opener_line = 0;
  std::size_t closer_line = 0;
  FoldSource source = FoldSource::Bracket;
};

// Lazy fold-region model owned by the active editor tab. Designed as a
// CPU-frugal lazy compute keyed on a coarse fingerprint (layout revision,
// tab size, language id) so it can be reused across consecutive frames when
// nothing actionable has changed.
class FoldingModel {
 public:
  struct Fingerprint {
    std::uint64_t layout_revision = 0;
    std::size_t tab_size = 4;
    std::string language_id;

    bool operator==(const Fingerprint& other) const {
      return layout_revision == other.layout_revision && tab_size == other.tab_size &&
             language_id == other.language_id;
    }
    bool operator!=(const Fingerprint& other) const { return !(*this == other); }
  };

  struct ComputeOptions {
    // Single-character bracket pairs (e.g. {/}, (/), [/]). Each entry is a
    // (open, close) pair encoded as a 2-character string for ergonomic use
    // in tests; either character can be supplied as the empty string to skip
    // that pair (no-op).
    std::vector<std::pair<char, char>> bracket_pairs;
    // When true, indent-driven block boundaries also yield fold ranges. They
    // are ignored on lines already covered by a balanced bracket range.
    bool use_indent_source = true;
    std::size_t tab_size = 4;
  };

  // Replace the stored ranges with a fresh scan. Returns true on completion.
  bool Compute(LineSpan lines, const ComputeOptions& options);

  // Same as `Compute` but stop scanning once `max_lines` of work is done; the
  // returned ranges are partial and `complete()` will report `false`. This is
  // the budgeted recompute described in the spec; the fold gutter renderer
  // paints whatever is resolved.
  // `incremental_resume_line` anchors bracket rescans after localized edits:
  // bracket folds with `closer_line < incremental_resume_line` are reused when
  // they match the previous model, and bracket balance is seeded from lines
  // `[0, incremental_resume_line)`. `std::numeric_limits<std::size_t>::max()`
  // forces a whole-file bracket scan.
  bool ComputeWithBudget(LineSpan lines,
                         const ComputeOptions& options,
                         std::size_t max_lines,
                         std::size_t incremental_resume_line = std::numeric_limits<std::size_t>::max(),
                         std::size_t target_end_exclusive = std::numeric_limits<std::size_t>::max(),
                         const TextViewport* syntax_viewport = nullptr);

  // Resolve folds only for the visible prefix needed by the current viewport
  // plus a bounded look-ahead. When the requested range is already resolved
  // and the model is not dirty, this is a no-op.
  bool EnsureFoldsForVisibleRange(
      LineSpan lines,
      const ComputeOptions& options,
      std::size_t visible_start_line,
      std::size_t visible_end_line,
      std::size_t max_lines,
      std::size_t incremental_resume_line = std::numeric_limits<std::size_t>::max(),
      const TextViewport* syntax_viewport = nullptr);

  // Toggle the collapsed state of the fold range whose opener matches
  // `opener_line`. Returns true if a matching range was found and toggled.
  bool ToggleFold(std::size_t opener_line);
  bool Collapse(std::size_t opener_line);
  bool Expand(std::size_t opener_line);
  void CollapseAll();
  void ExpandAll();

  // Returns true when the line participates in a collapsed fold body (i.e. it
  // is strictly between an opener and a closer of a collapsed range).
  bool IsLineHidden(std::size_t line) const;

  // Returns the fold range whose opener equals `line`, if any.
  std::optional<FoldRange> FoldStartingAt(std::size_t line) const;

  // Returns the innermost fold covering `line` (maximal opener_line among ranges
  // with opener_line <= line <= closer_line). Empty optional when unknown.
  std::optional<FoldRange> InnermostFoldContaining(std::size_t line) const;

  // Append every fold range whose `[opener_line, closer_line]` interval covers
  // `line`, in increasing-opener order (outermost first). Uses the indexed
  // prefix-max-closer cache so callers do not need to scan `ranges()` linearly.
  void AppendFoldsContaining(std::size_t line, std::vector<FoldRange>* out) const;

  // True iff a fold range opens at `line` and is currently collapsed.
  bool IsCollapsedAtOpener(std::size_t line) const;

  void Clear();

  const std::vector<FoldRange>& ranges() const { return ranges_; }
  const std::vector<bool>& collapsed_flags() const { return collapsed_; }
  // O(1) probe for "is any fold collapsed". Replaces the linear scan of
  // `collapsed_flags()` that the wrapped-row layout build used to do every edit.
  bool has_any_collapsed_fold() const { return collapsed_count_ > 0; }
  bool complete() const { return complete_; }
  std::size_t revision() const { return revision_; }

  // Look-ahead the visible-range resolve extends past the last visible line so an
  // opener on-screen whose closer sits just below still gets a marker. Shared with
  // EnsureFoldsForVisibleRange and IsVisibleRangeResolved so the refresh gate and
  // the scan use the same window.
  static constexpr std::size_t kVisibleLookAhead = 32;

  bool IsFresh(const Fingerprint& fingerprint) const {
    return !dirty_ && fingerprint_ == fingerprint;
  }
  // True when the folds covering [0, visible_end + look-ahead] are already resolved,
  // so an EnsureFoldsForVisibleRange call for that range would no-op. A budgeted
  // compute on a huge file resolves only a prefix, so IsFresh (content-only) is not
  // sufficient to skip the refresh — the scan must still extend as the user scrolls.
  bool IsVisibleRangeResolved(std::size_t visible_end_line) const {
    return !dirty_ &&
           (complete_ || resolved_prefix_line_count_ >= visible_end_line + kVisibleLookAhead + 1);
  }
  void MarkDirty() { dirty_ = true; }
  void SetFingerprint(Fingerprint fingerprint) {
    fingerprint_ = std::move(fingerprint);
    dirty_ = false;
  }
  const Fingerprint& fingerprint() const { return fingerprint_; }

 private:
  struct CollapsedInterval {
    std::size_t lo = 0;  // first hidden line (opener + 1)
    std::size_t hi = 0;  // last hidden line (closer, inclusive)
  };

  // Build the revision-keyed lookup tables: sorted collapsed-interval list with
  // prefix `hi` running-max, plus a per-range prefix running-max of `closer_line`
  // for InnermostFoldContaining. Cheap when ranges_/collapsed_ are unchanged.
  void EnsureLookupCache() const;

  std::vector<FoldRange> ranges_;
  std::vector<bool> collapsed_;  // parallel to ranges_
  // Maintained alongside `collapsed_` so `has_any_collapsed_fold()` is O(1).
  std::size_t collapsed_count_ = 0;
  Fingerprint fingerprint_;
  bool complete_ = true;
  bool dirty_ = true;
  std::size_t revision_ = 0;
  std::size_t resolved_prefix_line_count_ = 0;
  // Line count of the buffer at the last compute. The difference against the
  // current line count gives the net edit delta used to shift previous collapsed
  // fold openers so a collapsed fold survives a line-count-changing edit above it
  // (VS Code shift-preserves; matching by absolute opener/closer alone dropped it).
  std::size_t computed_line_count_ = 0;

  mutable std::size_t cached_revision_ = std::numeric_limits<std::size_t>::max();
  mutable std::vector<CollapsedInterval> cached_collapsed_intervals_;
  mutable std::vector<std::size_t> cached_collapsed_hi_prefix_max_;
  mutable std::vector<std::size_t> cached_range_closer_prefix_max_;
};

}  // namespace microide::editor
