#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/DiffSyntax.h"

// Turns one changed hunk — the run of left lines a line diff replaced with a run
// of right lines — into what the compare view shows: which tokens on each line
// are NOVEL (the structural diff's verdict) and which left line sits beside
// which right line.
//
// The line diff stays the authority on WHICH lines changed (it is what git
// sees, stages and applies); this only refines how a hunk it found is laid out
// and highlighted. So a line can be in a hunk and still carry no novel token —
// it changed only in whitespace or line breaks — and the view shows exactly
// that.
namespace microide::compare {

inline constexpr std::uint32_t kNoHunkLine = std::numeric_limits<std::uint32_t>::max();

struct HunkRow {
  std::uint32_t left = kNoHunkLine;   // index into the hunk's left lines
  std::uint32_t right = kNoHunkLine;  // index into the hunk's right lines
};

struct HunkRefinement {
  std::vector<HunkRow> rows;
  // Novel byte spans per hunk line, sorted and non-overlapping.
  std::vector<std::vector<CompareTextSpan>> left_spans;
  std::vector<std::vector<CompareTextSpan>> right_spans;
};

struct HunkRefinementStats {
  std::size_t structural_hunks = 0;
  std::size_t flat_fallback_hunks = 0;
  std::size_t graph_vertices = 0;
  // Hunks answered by a HunkRefinementCache without refining.
  std::size_t cached_hunks = 0;
};

// Refines one hunk. `left_lines`/`right_lines` are consecutive lines of their
// buffers. Reuses `out`'s storage.
void RefineHunk(std::span<const std::string_view> left_lines,
                std::span<const std::string_view> right_lines,
                const DiffSyntax& syntax,
                HunkRefinement& out,
                HunkRefinementStats* stats = nullptr);

// Memoizes RefineHunk across rebuilds of one compare model, keyed on the exact
// text of the hunk's two sides and the syntax it was read with.
//
// An editable compare pane rebuilds its model on every keystroke, and a
// keystroke changes one hunk: without this, every other hunk of the file was
// lexed, matched and aligned again, so a keystroke cost the whole diff's
// refinement instead of one hunk's. A hit is verified against the stored text,
// never trusted on the hash alone. Entries a build did not use are dropped when
// it ends, so the cache holds exactly the current model's hunks.
class HunkRefinementCache {
 public:
  void BeginBuild();
  // The refinement of this hunk: the cached one, or a fresh RefineHunk.
  const HunkRefinement& Refine(std::span<const std::string_view> left_lines,
                               std::span<const std::string_view> right_lines,
                               const std::shared_ptr<const DiffSyntax>& syntax,
                               HunkRefinementStats* stats);
  void EndBuild();

  std::size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::uint64_t hash = 0;
    // Shared, not a raw pointer: a freed syntax's address can be reused by a
    // different one, which must not read as the same key.
    std::shared_ptr<const DiffSyntax> syntax;
    std::string left;   // the side's lines, each followed by '\n'
    std::string right;
    HunkRefinement refinement;
    std::uint64_t used_in_build = 0;
  };

  std::vector<std::unique_ptr<Entry>> entries_;
  std::unordered_multimap<std::uint64_t, Entry*> by_hash_;
  std::uint64_t build_ = 0;
};

// Rows for a hunk without refinement: left and right lines side by side in
// order, the longer side's tail alone. No spans.
void ZipHunk(std::size_t left_count, std::size_t right_count, std::vector<HunkRow>& rows);

}  // namespace microide::compare
