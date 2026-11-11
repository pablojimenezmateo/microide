#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
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

// One hunk's refinement as the model build reads it: the rows, and the novel
// spans of each line. A view into storage it does not own (a HunkRefinement or
// a HunkRefinementCache arena); valid until that storage next changes.
class HunkRefinementView {
 public:
  HunkRefinementView() = default;
  explicit HunkRefinementView(const HunkRefinement& refinement);
  HunkRefinementView(std::span<const HunkRow> rows, std::span<const CompareTextSpan> spans,
                     std::span<const std::uint32_t> line_starts, std::uint32_t left_lines);

  std::span<const HunkRow> rows() const { return rows_; }
  // Novel spans of the hunk's `line`-th left / right line (empty when none).
  std::span<const CompareTextSpan> LeftSpans(std::uint32_t line) const;
  std::span<const CompareTextSpan> RightSpans(std::uint32_t line) const;

 private:
  std::span<const CompareTextSpan> LineSpans(std::uint32_t slot) const;

  const HunkRefinement* refinement_ = nullptr;
  std::span<const HunkRow> rows_;
  std::span<const CompareTextSpan> spans_;
  std::span<const std::uint32_t> line_starts_;  // left lines, then right, then the end
  std::uint32_t left_lines_ = 0;
};

// Memoizes RefineHunk across rebuilds of one compare model, keyed on the exact
// text of the hunk's two sides and the syntax of the build.
//
// An editable compare pane rebuilds its model on every keystroke, and a
// keystroke changes one hunk: without this, every other hunk of the file was
// lexed, matched and aligned again, so a keystroke cost the whole diff's
// refinement instead of one hunk's. A hit is verified against the stored text,
// never trusted on the hash alone. The cache holds the hunks of the last
// finished build only.
//
// Storage is two flat arenas, the previous build's and the one being built,
// swapped at the end of a build: a hit copies its entry across, a miss refines
// into retained scratch and appends. After the first build or two every buffer
// has its capacity, so a rebuild allocates nothing here -- a per-entry heap
// layout doubled the allocations of a keystroke.
class HunkRefinementCache {
 public:
  // Starts a build whose hunks are all read with `syntax`.
  void BeginBuild(std::shared_ptr<const DiffSyntax> syntax);
  // The refinement of this hunk: cached, or refined now. The view is valid
  // until the next Refine or EndBuild.
  HunkRefinementView Refine(std::span<const std::string_view> left_lines,
                            std::span<const std::string_view> right_lines,
                            HunkRefinementStats* stats);
  void EndBuild();

  // Hunks held from the last finished build.
  std::size_t size() const { return previous_.entries.size(); }

 private:
  struct Entry {
    std::uint64_t hash = 0;
    std::uint32_t text_begin = 0;
    std::uint32_t left_bytes = 0;   // the left lines, each followed by '\n'
    std::uint32_t right_bytes = 0;  // then the right lines, likewise
    std::uint32_t rows_begin = 0;
    std::uint32_t rows_count = 0;
    std::uint32_t lines_begin = 0;  // into line_starts: left + right + 1 offsets
    std::uint32_t left_lines = 0;
    std::uint32_t right_lines = 0;
  };
  struct Arena {
    // Shared, not raw: a freed syntax's address can be reused by a different
    // one, which must not read as the same key.
    std::shared_ptr<const DiffSyntax> syntax;
    std::string text;
    std::vector<HunkRow> rows;
    std::vector<CompareTextSpan> spans;
    std::vector<std::uint32_t> line_starts;
    std::vector<Entry> entries;
    std::vector<std::uint32_t> index;  // open addressing: entry + 1, 0 = empty

    void Clear();
    std::uint32_t Find(std::uint64_t hash, std::span<const std::string_view> left,
                       std::span<const std::string_view> right) const;
    void Index(std::uint32_t entry);
    HunkRefinementView View(std::uint32_t entry) const;
  };

  std::uint32_t CopyFrom(const Arena& source, std::uint32_t entry);
  std::uint32_t Append(std::uint64_t hash, std::span<const std::string_view> left,
                       std::span<const std::string_view> right,
                       const HunkRefinement& refinement);

  Arena current_;
  Arena previous_;
  HunkRefinement scratch_;
};

// Rows for a hunk without refinement: left and right lines side by side in
// order, the longer side's tail alone. No spans.
void ZipHunk(std::size_t left_count, std::size_t right_count, std::vector<HunkRow>& rows);

}  // namespace microide::compare
