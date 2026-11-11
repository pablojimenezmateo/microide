#include "compare/HunkRefinement.h"

#include <algorithm>
#include <tuple>

#include "compare/StructuralDiff.h"
#include "util/Fnv1a.h"

namespace microide::compare {

namespace {

bool IsBlank(char byte) { return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\f'; }

bool IsWordByte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80;
}

struct LinePair {
  std::uint32_t left = 0;
  std::uint32_t right = 0;
  std::uint64_t weight = 0;
};

struct RefineScratch {
  DiffContentInterner interner;
  DiffSyntaxTree lhs;
  DiffSyntaxTree rhs;
  StructuralMatch match;
  std::vector<std::string_view> words_lhs;
  std::vector<std::string_view> words_rhs;
  std::vector<LinePair> pairs;
  std::vector<std::uint64_t> best;
  std::vector<std::uint32_t> pred;
  // Fenwick tree over right lines: the best chain ending at or before a line.
  std::vector<std::uint64_t> fenwick_value;
  std::vector<std::uint32_t> fenwick_pair;
  std::vector<std::uint32_t> chain;

  // A huge hunk's trees and match are not worth keeping for the next, ordinary,
  // hunk on this thread.
  void ReleaseIfLarge() {
    constexpr std::size_t kRetainTokens = 1u << 16;
    if (lhs.tokens.capacity() + rhs.tokens.capacity() > kRetainTokens) {
      lhs = DiffSyntaxTree();
      rhs = DiffSyntaxTree();
      match = StructuralMatch();
      std::vector<LinePair>().swap(pairs);
      std::vector<std::uint64_t>().swap(best);
      std::vector<std::uint32_t>().swap(pred);
      interner = DiffContentInterner();
    }
  }
};

RefineScratch& Scratch() {
  thread_local RefineScratch scratch;
  return scratch;
}

void ResetSpans(std::vector<std::vector<CompareTextSpan>>& spans, std::size_t lines) {
  spans.resize(lines);
  for (std::vector<CompareTextSpan>& line : spans) {
    line.clear();
  }
}

// Appends [start, end), trimmed of surrounding blanks, to a line's spans.
void AddSpan(std::vector<CompareTextSpan>& spans, std::string_view line, std::size_t start,
             std::size_t end) {
  while (start < end && IsBlank(line[start])) {
    ++start;
  }
  while (end > start && IsBlank(line[end - 1])) {
    --end;
  }
  if (start < end) {
    spans.push_back(CompareTextSpan{start, end});
  }
}

// Sorts a line's spans and joins those that touch or are separated only by
// blanks: two novel tokens in a row read as one change.
void NormalizeSpans(std::vector<CompareTextSpan>& spans, std::string_view line) {
  if (spans.size() < 2) {
    return;
  }
  std::sort(spans.begin(), spans.end(),
            [](const CompareTextSpan& a, const CompareTextSpan& b) { return a.start < b.start; });
  std::size_t kept = 0;
  for (std::size_t index = 1; index < spans.size(); ++index) {
    CompareTextSpan& last = spans[kept];
    const CompareTextSpan& next = spans[index];
    const bool joins =
        next.start <= last.end ||
        std::all_of(line.begin() + static_cast<std::ptrdiff_t>(last.end),
                    line.begin() + static_cast<std::ptrdiff_t>(next.start), IsBlank);
    if (joins) {
      last.end = std::max(last.end, next.end);
    } else {
      spans[++kept] = next;
    }
  }
  spans.resize(kept + 1);
}

// Marks the bytes [begin, end) of a region — given as pointers into its lines,
// starting the search at `line` — novel, line by line.
void AddRange(std::span<const std::string_view> lines, std::uint32_t line, const char* begin,
              const char* end, std::vector<std::vector<CompareTextSpan>>& spans) {
  const auto address = [](const char* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); };
  for (; line < lines.size() && address(begin) < address(end); ++line) {
    const std::string_view text = lines[line];
    const std::uintptr_t line_begin = address(text.data());
    const std::uintptr_t line_end = line_begin + text.size();
    if (address(begin) > line_end) {
      continue;  // the range starts on a later line
    }
    const std::size_t start = address(begin) > line_begin ? address(begin) - line_begin : 0;
    const std::size_t stop = std::min<std::uintptr_t>(address(end), line_end) - line_begin;
    if (stop > start) {
      AddSpan(spans[line], text, start, stop);
    }
    if (address(end) <= line_end) {
      return;
    }
    begin = lines[line].data() + text.size();
  }
}

void AddToken(std::span<const std::string_view> lines, const DiffToken& token,
              std::vector<std::vector<CompareTextSpan>>& spans) {
  if (token.line == token.end_line) {
    AddSpan(spans[token.line], lines[token.line], token.column, token.end_column);
    return;
  }
  AddRange(lines, token.line, token.text.data(), token.text.data() + token.text.size(), spans);
}

void SplitWords(std::string_view text, std::vector<std::string_view>& words) {
  words.clear();
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (IsBlank(static_cast<char>(byte)) || byte == '\n') {
      ++index;
      continue;
    }
    const std::size_t start = index;
    if (IsWordByte(byte)) {
      while (index < text.size() && IsWordByte(static_cast<unsigned char>(text[index]))) {
        ++index;
      }
    } else {
      ++index;
    }
    words.push_back(text.substr(start, index - start));
  }
}

// A replaced string or comment: diff its words, and mark only the words that
// changed. A literal that was merely re-indented or re-wrapped marks nothing.
void AddReplacedWords(std::span<const std::string_view> left_lines,
                      std::span<const std::string_view> right_lines, const DiffToken& lhs,
                      const DiffToken& rhs, HunkRefinement& out, RefineScratch& scratch) {
  SplitWords(lhs.text, scratch.words_lhs);
  SplitWords(rhs.text, scratch.words_rhs);
  const std::vector<DiffOp> ops = BuildLineDiffOps(scratch.words_lhs, scratch.words_rhs);
  for (const DiffOp& op : ops) {
    if (op.kind == DiffOpKind::Delete) {
      AddRange(left_lines, lhs.line, op.text.data(), op.text.data() + op.text.size(),
               out.left_spans);
    } else if (op.kind == DiffOpKind::Insert) {
      AddRange(right_lines, rhs.line, op.text.data(), op.text.data() + op.text.size(),
               out.right_spans);
    }
  }
}

// The rows of a hunk: the heaviest chain of (left line, right line) pairs that
// share matched tokens, strictly increasing on both sides, with the lines
// between consecutive anchors zipped side by side.
void AlignRows(std::size_t left_count, std::size_t right_count, RefineScratch& scratch,
               std::vector<HunkRow>& rows) {
  std::vector<LinePair>& pairs = scratch.pairs;
  std::sort(pairs.begin(), pairs.end(), [](const LinePair& a, const LinePair& b) {
    return std::tie(a.left, a.right) < std::tie(b.left, b.right);
  });
  std::size_t merged = 0;
  for (std::size_t index = 0; index < pairs.size(); ++index) {
    if (merged > 0 && pairs[merged - 1].left == pairs[index].left &&
        pairs[merged - 1].right == pairs[index].right) {
      pairs[merged - 1].weight += pairs[index].weight;
    } else {
      pairs[merged++] = pairs[index];
    }
  }
  pairs.resize(merged);

  // Weighted longest increasing chain: pairs grouped by left line; a pair may
  // extend any chain ending at a strictly smaller left AND right line, which
  // the Fenwick prefix-max over right lines answers once a whole left-line
  // group has been scored.
  const std::size_t size = right_count + 1;
  scratch.fenwick_value.assign(size, 0);
  scratch.fenwick_pair.assign(size, kNoHunkLine);
  scratch.best.assign(pairs.size(), 0);
  scratch.pred.assign(pairs.size(), kNoHunkLine);
  const auto query = [&](std::size_t right_exclusive, std::uint64_t& value) {
    value = 0;
    std::uint32_t at = kNoHunkLine;
    for (std::size_t i = right_exclusive; i > 0; i -= i & (~i + 1)) {
      if (scratch.fenwick_value[i] > value) {
        value = scratch.fenwick_value[i];
        at = scratch.fenwick_pair[i];
      }
    }
    return at;
  };
  const auto update = [&](std::size_t right, std::uint64_t value, std::uint32_t pair) {
    for (std::size_t i = right + 1; i < size; i += i & (~i + 1)) {
      if (value > scratch.fenwick_value[i]) {
        scratch.fenwick_value[i] = value;
        scratch.fenwick_pair[i] = pair;
      }
    }
  };
  std::uint64_t best_value = 0;
  std::uint32_t best_pair = kNoHunkLine;
  for (std::size_t group = 0; group < pairs.size();) {
    std::size_t group_end = group;
    while (group_end < pairs.size() && pairs[group_end].left == pairs[group].left) {
      ++group_end;
    }
    for (std::size_t index = group; index < group_end; ++index) {
      std::uint64_t prior = 0;
      scratch.pred[index] = query(pairs[index].right, prior);
      scratch.best[index] = prior + pairs[index].weight;
      if (scratch.best[index] > best_value) {
        best_value = scratch.best[index];
        best_pair = static_cast<std::uint32_t>(index);
      }
    }
    for (std::size_t index = group; index < group_end; ++index) {
      update(pairs[index].right, scratch.best[index], static_cast<std::uint32_t>(index));
    }
    group = group_end;
  }
  std::vector<std::uint32_t>& chain = scratch.chain;
  chain.clear();
  for (std::uint32_t at = best_pair; at != kNoHunkLine; at = scratch.pred[at]) {
    chain.push_back(at);
  }
  std::reverse(chain.begin(), chain.end());

  rows.clear();
  std::uint32_t left = 0;
  std::uint32_t right = 0;
  const auto zip_until = [&](std::uint32_t left_stop, std::uint32_t right_stop) {
    while (left < left_stop && right < right_stop) {
      rows.push_back(HunkRow{left++, right++});
    }
    while (left < left_stop) {
      rows.push_back(HunkRow{left++, kNoHunkLine});
    }
    while (right < right_stop) {
      rows.push_back(HunkRow{kNoHunkLine, right++});
    }
  };
  for (const std::uint32_t index : chain) {
    zip_until(pairs[index].left, pairs[index].right);
    rows.push_back(HunkRow{left++, right++});
  }
  zip_until(static_cast<std::uint32_t>(left_count), static_cast<std::uint32_t>(right_count));
}

}  // namespace

namespace {

std::uint64_t HashSide(std::uint64_t hash, std::span<const std::string_view> lines) {
  hash = util::Fnv1aValue(hash, lines.size());
  for (const std::string_view line : lines) {
    hash = util::Fnv1aByte(util::Fnv1aBytes(hash, line), '\n');
  }
  return hash;
}

bool SideEquals(const std::string& stored, std::span<const std::string_view> lines) {
  std::size_t at = 0;
  for (const std::string_view line : lines) {
    if (stored.size() - at < line.size() + 1 || stored.compare(at, line.size(), line) != 0 ||
        stored[at + line.size()] != '\n') {
      return false;
    }
    at += line.size() + 1;
  }
  return at == stored.size();
}

void StoreSide(std::string& stored, std::span<const std::string_view> lines) {
  stored.clear();
  for (const std::string_view line : lines) {
    stored.append(line);
    stored.push_back('\n');
  }
}

}  // namespace

void HunkRefinementCache::BeginBuild() { ++build_; }

const HunkRefinement& HunkRefinementCache::Refine(std::span<const std::string_view> left_lines,
                                                  std::span<const std::string_view> right_lines,
                                                  const std::shared_ptr<const DiffSyntax>& syntax,
                                                  HunkRefinementStats* stats) {
  const std::uint64_t hash =
      HashSide(HashSide(util::kFnv1aOffsetBasis, left_lines), right_lines);
  for (auto [it, end] = by_hash_.equal_range(hash); it != end; ++it) {
    Entry* entry = it->second;
    if (entry->syntax == syntax && SideEquals(entry->left, left_lines) &&
        SideEquals(entry->right, right_lines)) {
      entry->used_in_build = build_;
      if (stats != nullptr) {
        ++stats->cached_hunks;
      }
      return entry->refinement;
    }
  }
  auto entry = std::make_unique<Entry>();
  entry->hash = hash;
  entry->syntax = syntax;
  StoreSide(entry->left, left_lines);
  StoreSide(entry->right, right_lines);
  RefineHunk(left_lines, right_lines, *syntax, entry->refinement, stats);
  entry->used_in_build = build_;
  by_hash_.emplace(hash, entry.get());
  entries_.push_back(std::move(entry));
  return entries_.back()->refinement;
}

void HunkRefinementCache::EndBuild() {
  const std::size_t before = entries_.size();
  std::erase_if(entries_, [&](const std::unique_ptr<Entry>& entry) {
    return entry->used_in_build != build_;
  });
  if (entries_.size() != before) {
    by_hash_.clear();
    for (const std::unique_ptr<Entry>& entry : entries_) {
      by_hash_.emplace(entry->hash, entry.get());
    }
  }
}

void ZipHunk(std::size_t left_count, std::size_t right_count, std::vector<HunkRow>& rows) {
  rows.clear();
  const std::size_t count = std::max(left_count, right_count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(HunkRow{
        index < left_count ? static_cast<std::uint32_t>(index) : kNoHunkLine,
        index < right_count ? static_cast<std::uint32_t>(index) : kNoHunkLine,
    });
  }
}

void RefineHunk(std::span<const std::string_view> left_lines,
                std::span<const std::string_view> right_lines,
                const DiffSyntax& syntax,
                HunkRefinement& out,
                HunkRefinementStats* stats) {
  ResetSpans(out.left_spans, left_lines.size());
  ResetSpans(out.right_spans, right_lines.size());

  // A one-sided hunk is novel through and through: no tree to compare.
  if (left_lines.empty() || right_lines.empty()) {
    for (std::size_t line = 0; line < left_lines.size(); ++line) {
      AddSpan(out.left_spans[line], left_lines[line], 0, left_lines[line].size());
    }
    for (std::size_t line = 0; line < right_lines.size(); ++line) {
      AddSpan(out.right_spans[line], right_lines[line], 0, right_lines[line].size());
    }
    ZipHunk(left_lines.size(), right_lines.size(), out.rows);
    return;
  }

  RefineScratch& scratch = Scratch();
  scratch.interner.Reset();
  BuildDiffSyntaxTree(left_lines, syntax, scratch.interner, scratch.lhs);
  BuildDiffSyntaxTree(right_lines, syntax, scratch.interner, scratch.rhs);
  StructuralDiffStats diff_stats;
  MatchSyntaxTrees(scratch.lhs, scratch.rhs, kStructuralDiffVertexBudget, scratch.match,
                   &diff_stats);
  if (stats != nullptr) {
    ++(diff_stats.fell_back ? stats->flat_fallback_hunks : stats->structural_hunks);
    stats->graph_vertices += diff_stats.vertices;
  }

  const StructuralMatch& match = scratch.match;
  scratch.pairs.clear();
  for (std::uint32_t index = 0; index < scratch.lhs.tokens.size(); ++index) {
    const DiffToken& token = scratch.lhs.tokens[index];
    const std::uint32_t partner = match.lhs_partner[index];
    if (partner == kNoDiffNode) {
      AddToken(left_lines, token, out.left_spans);
      continue;
    }
    const DiffToken& other = scratch.rhs.tokens[partner];
    if (match.lhs_replaced[index] != 0) {
      AddReplacedWords(left_lines, right_lines, token, other, out, scratch);
    }
    scratch.pairs.push_back(LinePair{token.line, other.line, token.text.size()});
  }
  for (std::uint32_t index = 0; index < scratch.rhs.tokens.size(); ++index) {
    if (match.rhs_partner[index] == kNoDiffNode) {
      AddToken(right_lines, scratch.rhs.tokens[index], out.right_spans);
    }
  }
  for (std::size_t line = 0; line < left_lines.size(); ++line) {
    NormalizeSpans(out.left_spans[line], left_lines[line]);
  }
  for (std::size_t line = 0; line < right_lines.size(); ++line) {
    NormalizeSpans(out.right_spans[line], right_lines[line]);
  }
  AlignRows(left_lines.size(), right_lines.size(), scratch, out.rows);
  scratch.ReleaseIfLarge();
}

}  // namespace microide::compare
