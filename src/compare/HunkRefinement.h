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

namespace microide::compare {

inline constexpr std::uint32_t kNoHunkLine = std::numeric_limits<std::uint32_t>::max();

struct HunkRow {
  std::uint32_t left = kNoHunkLine;
  std::uint32_t right = kNoHunkLine;
};

struct HunkRefinement {
  std::vector<HunkRow> rows;
  std::vector<std::vector<CompareTextSpan>> left_spans;
  std::vector<std::vector<CompareTextSpan>> right_spans;
};

struct HunkRefinementStats {
  std::size_t structural_hunks = 0;
  std::size_t flat_fallback_hunks = 0;
  std::size_t graph_vertices = 0;
  std::size_t cached_hunks = 0;
};

void RefineHunk(std::span<const std::string_view> left_lines,
                std::span<const std::string_view> right_lines,
                const DiffSyntax& syntax,
                HunkRefinement& out,
                HunkRefinementStats* stats = nullptr,
                const DiffLexState& left_entry = {},
                const DiffLexState& right_entry = {});

class HunkRefinementView {
 public:
  HunkRefinementView() = default;
  HunkRefinementView(std::span<const HunkRow> rows, std::span<const CompareTextSpan> spans,
                     std::span<const std::uint32_t> line_starts, std::uint32_t left_lines);

  std::span<const HunkRow> rows() const { return rows_; }
  std::span<const CompareTextSpan> LeftSpans(std::uint32_t line) const;
  std::span<const CompareTextSpan> RightSpans(std::uint32_t line) const;

 private:
  std::span<const CompareTextSpan> LineSpans(std::uint32_t slot) const;

  std::span<const HunkRow> rows_;
  std::span<const CompareTextSpan> spans_;
  std::span<const std::uint32_t> line_starts_;
  std::uint32_t left_lines_ = 0;
};

class HunkRefinementCache {
 public:
  void BeginBuild(std::shared_ptr<const DiffSyntax> syntax);
  HunkRefinementView Refine(std::span<const std::string_view> left_lines,
                            std::span<const std::string_view> right_lines,
                            HunkRefinementStats* stats,
                            const DiffLexState& left_entry = {},
                            const DiffLexState& right_entry = {});
  void EndBuild();

  std::size_t size() const { return previous_.entries.size(); }

 private:
  struct Entry {
    std::uint64_t hash = 0;
    std::uint32_t text_begin = 0;
    std::uint32_t left_bytes = 0;
    std::uint32_t right_bytes = 0;
    std::uint32_t rows_begin = 0;
    std::uint32_t rows_count = 0;
    std::uint32_t lines_begin = 0;
    std::uint32_t left_lines = 0;
    std::uint32_t right_lines = 0;
    DiffLexState::Kind left_kind = DiffLexState::Kind::Code;
    DiffLexState::Kind right_kind = DiffLexState::Kind::Code;
    std::uint8_t left_index = 0;
    std::uint8_t right_index = 0;
    std::uint16_t left_delimiter = 0;
    std::uint16_t right_delimiter = 0;
  };
  struct Arena {
    std::shared_ptr<const DiffSyntax> syntax;
    std::string text;
    std::vector<HunkRow> rows;
    std::vector<CompareTextSpan> spans;
    std::vector<std::uint32_t> line_starts;
    std::vector<Entry> entries;
    std::vector<std::uint32_t> index;

    void Clear();
    std::uint32_t Find(std::uint64_t hash, std::span<const std::string_view> left,
                       std::span<const std::string_view> right, const DiffLexState& left_entry,
                       const DiffLexState& right_entry) const;
    void Index(std::uint32_t entry);
    HunkRefinementView View(std::uint32_t entry) const;
  };

  std::uint32_t CopyFrom(const Arena& source, std::uint32_t entry);
  std::uint32_t Append(std::uint64_t hash, std::span<const std::string_view> left,
                       std::span<const std::string_view> right, const DiffLexState& left_entry,
                       const DiffLexState& right_entry, const HunkRefinement& refinement);

  Arena current_;
  Arena previous_;
  HunkRefinement scratch_;
};

void ZipHunk(std::size_t left_count, std::size_t right_count, std::vector<HunkRow>& rows);

}
