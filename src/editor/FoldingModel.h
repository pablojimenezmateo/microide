#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::editor {

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
  bool Compute(const std::vector<std::string>& lines, const ComputeOptions& options);

  // Same as `Compute` but stop scanning once `max_lines` of work is done; the
  // returned ranges are partial and `complete()` will report `false`. This is
  // the budgeted recompute described in the spec; the fold gutter renderer
  // paints whatever is resolved.
  // `incremental_resume_line` anchors bracket rescans after localized edits:
  // bracket folds with `closer_line < incremental_resume_line` are reused when
  // they match the previous model, and bracket balance is seeded from lines
  // `[0, incremental_resume_line)`. `std::numeric_limits<std::size_t>::max()`
  // forces a whole-file bracket scan.
  bool ComputeWithBudget(const std::vector<std::string>& lines,
                         const ComputeOptions& options,
                         std::size_t max_lines,
                         std::size_t incremental_resume_line = std::numeric_limits<std::size_t>::max());

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

  // True iff a fold range opens at `line` and is currently collapsed.
  bool IsCollapsedAtOpener(std::size_t line) const;

  void Clear();

  const std::vector<FoldRange>& ranges() const { return ranges_; }
  const std::vector<bool>& collapsed_flags() const { return collapsed_; }
  bool complete() const { return complete_; }
  std::size_t revision() const { return revision_; }

  bool IsFresh(const Fingerprint& fingerprint) const {
    return !dirty_ && fingerprint_ == fingerprint;
  }
  void MarkDirty() { dirty_ = true; }
  void SetFingerprint(Fingerprint fingerprint) {
    fingerprint_ = std::move(fingerprint);
    dirty_ = false;
  }
  const Fingerprint& fingerprint() const { return fingerprint_; }

 private:
  std::vector<FoldRange> ranges_;
  std::vector<bool> collapsed_;  // parallel to ranges_
  Fingerprint fingerprint_;
  bool complete_ = true;
  bool dirty_ = true;
  std::size_t revision_ = 0;
};

}  // namespace microide::editor
