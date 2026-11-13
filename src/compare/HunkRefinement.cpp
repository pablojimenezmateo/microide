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

std::uint64_t HashState(std::uint64_t hash, const DiffLexState& state) {
  hash = util::Fnv1aValue(hash, (static_cast<std::uint32_t>(state.kind) << 8) | state.index);
  return state.delimiter.empty() ? hash : util::Fnv1aBytes(hash, state.delimiter);
}

bool SideEquals(std::string_view stored, std::span<const std::string_view> lines) {
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

}  // namespace

HunkRefinementView::HunkRefinementView(const HunkRefinement& refinement)
    : refinement_(&refinement), rows_(refinement.rows) {}

HunkRefinementView::HunkRefinementView(std::span<const HunkRow> rows,
                                       std::span<const CompareTextSpan> spans,
                                       std::span<const std::uint32_t> line_starts,
                                       std::uint32_t left_lines)
    : rows_(rows), spans_(spans), line_starts_(line_starts), left_lines_(left_lines) {}

std::span<const CompareTextSpan> HunkRefinementView::LeftSpans(std::uint32_t line) const {
  if (refinement_ != nullptr) {
    return line < refinement_->left_spans.size()
               ? std::span<const CompareTextSpan>(refinement_->left_spans[line])
               : std::span<const CompareTextSpan>();
  }
  return line < left_lines_ ? LineSpans(line) : std::span<const CompareTextSpan>();
}

std::span<const CompareTextSpan> HunkRefinementView::RightSpans(std::uint32_t line) const {
  if (refinement_ != nullptr) {
    return line < refinement_->right_spans.size()
               ? std::span<const CompareTextSpan>(refinement_->right_spans[line])
               : std::span<const CompareTextSpan>();
  }
  const std::size_t slot = std::size_t{left_lines_} + line;
  return slot + 1 < line_starts_.size() ? LineSpans(static_cast<std::uint32_t>(slot))
                                        : std::span<const CompareTextSpan>();
}

std::span<const CompareTextSpan> HunkRefinementView::LineSpans(std::uint32_t slot) const {
  return spans_.subspan(line_starts_[slot], line_starts_[slot + 1] - line_starts_[slot]);
}

namespace {

std::uint64_t MixHash(std::uint64_t value) {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  return value;
}

}  // namespace

void HunkRefinementCache::Arena::Clear() {
  syntax.reset();
  text.clear();
  rows.clear();
  spans.clear();
  line_starts.clear();
  entries.clear();
  std::fill(index.begin(), index.end(), 0u);
}

std::uint32_t HunkRefinementCache::Arena::Find(std::uint64_t hash,
                                               std::span<const std::string_view> left,
                                               std::span<const std::string_view> right,
                                               const DiffLexState& left_entry,
                                               const DiffLexState& right_entry) const {
  if (index.empty()) {
    return kNoHunkLine;
  }
  const std::size_t mask = index.size() - 1;
  for (std::size_t slot = MixHash(hash) & mask; index[slot] != 0; slot = (slot + 1) & mask) {
    const Entry& entry = entries[index[slot] - 1];
    if (entry.hash == hash && entry.left_kind == left_entry.kind &&
        entry.right_kind == right_entry.kind && entry.left_index == left_entry.index &&
        entry.right_index == right_entry.index &&
        entry.left_delimiter == left_entry.delimiter.size() &&
        entry.right_delimiter == right_entry.delimiter.size()) {
      const std::string_view delimiters(text.data() + entry.text_begin,
                                        std::size_t{entry.left_delimiter} + entry.right_delimiter);
      const std::string_view stored(delimiters.data() + delimiters.size(),
                                    std::size_t{entry.left_bytes} + entry.right_bytes);
      if (delimiters.substr(0, entry.left_delimiter) == left_entry.delimiter &&
          delimiters.substr(entry.left_delimiter) == right_entry.delimiter &&
          SideEquals(stored.substr(0, entry.left_bytes), left) &&
          SideEquals(stored.substr(entry.left_bytes), right)) {
        return index[slot] - 1;
      }
    }
  }
  return kNoHunkLine;
}

void HunkRefinementCache::Arena::Index(std::uint32_t entry) {
  if ((entries.size() + 1) * 2 > index.size()) {
    // Grow and reinsert every entry; only while a cache is first filling.
    index.assign(std::max<std::size_t>(64, index.size() * 2), 0u);
    const std::size_t mask = index.size() - 1;
    for (std::uint32_t existing = 0; existing < entries.size(); ++existing) {
      std::size_t slot = MixHash(entries[existing].hash) & mask;
      while (index[slot] != 0) {
        slot = (slot + 1) & mask;
      }
      index[slot] = existing + 1;
    }
    return;
  }
  const std::size_t mask = index.size() - 1;
  std::size_t slot = MixHash(entries[entry].hash) & mask;
  while (index[slot] != 0) {
    slot = (slot + 1) & mask;
  }
  index[slot] = entry + 1;
}

HunkRefinementView HunkRefinementCache::Arena::View(std::uint32_t entry_index) const {
  const Entry& entry = entries[entry_index];
  return HunkRefinementView(
      std::span<const HunkRow>(rows).subspan(entry.rows_begin, entry.rows_count),
      std::span<const CompareTextSpan>(spans),
      std::span<const std::uint32_t>(line_starts)
          .subspan(entry.lines_begin, std::size_t{entry.left_lines} + entry.right_lines + 1),
      entry.left_lines);
}

void HunkRefinementCache::BeginBuild(std::shared_ptr<const DiffSyntax> syntax) {
  current_.Clear();
  current_.syntax = std::move(syntax);
}

std::uint32_t HunkRefinementCache::CopyFrom(const Arena& source, std::uint32_t entry_index) {
  const Entry& from = source.entries[entry_index];
  Entry entry = from;
  entry.text_begin = static_cast<std::uint32_t>(current_.text.size());
  current_.text.append(source.text, from.text_begin,
                       std::size_t{from.left_delimiter} + from.right_delimiter + from.left_bytes +
                           from.right_bytes);
  entry.rows_begin = static_cast<std::uint32_t>(current_.rows.size());
  current_.rows.insert(current_.rows.end(), source.rows.begin() + from.rows_begin,
                       source.rows.begin() + from.rows_begin + from.rows_count);
  // Line starts are offsets into the arena's span list, so rebase them.
  const std::uint32_t lines = from.left_lines + from.right_lines;
  const std::uint32_t old_base = source.line_starts[from.lines_begin];
  const std::uint32_t old_end = source.line_starts[from.lines_begin + lines];
  const auto new_base = static_cast<std::uint32_t>(current_.spans.size());
  current_.spans.insert(current_.spans.end(), source.spans.begin() + old_base,
                        source.spans.begin() + old_end);
  entry.lines_begin = static_cast<std::uint32_t>(current_.line_starts.size());
  for (std::uint32_t line = 0; line <= lines; ++line) {
    current_.line_starts.push_back(source.line_starts[from.lines_begin + line] - old_base +
                                   new_base);
  }
  current_.entries.push_back(entry);
  const auto index = static_cast<std::uint32_t>(current_.entries.size() - 1);
  current_.Index(index);
  return index;
}

std::uint32_t HunkRefinementCache::Append(std::uint64_t hash,
                                          std::span<const std::string_view> left,
                                          std::span<const std::string_view> right,
                                          const DiffLexState& left_entry,
                                          const DiffLexState& right_entry,
                                          const HunkRefinement& refinement) {
  Entry entry;
  entry.hash = hash;
  entry.text_begin = static_cast<std::uint32_t>(current_.text.size());
  entry.left_kind = left_entry.kind;
  entry.right_kind = right_entry.kind;
  entry.left_index = left_entry.index;
  entry.right_index = right_entry.index;
  entry.left_delimiter = static_cast<std::uint16_t>(left_entry.delimiter.size());
  entry.right_delimiter = static_cast<std::uint16_t>(right_entry.delimiter.size());
  current_.text.append(left_entry.delimiter);
  current_.text.append(right_entry.delimiter);
  const std::size_t lines_begin = current_.text.size();
  for (const std::string_view line : left) {
    current_.text.append(line);
    current_.text.push_back('\n');
  }
  entry.left_bytes = static_cast<std::uint32_t>(current_.text.size() - lines_begin);
  for (const std::string_view line : right) {
    current_.text.append(line);
    current_.text.push_back('\n');
  }
  entry.right_bytes =
      static_cast<std::uint32_t>(current_.text.size() - lines_begin - entry.left_bytes);
  entry.rows_begin = static_cast<std::uint32_t>(current_.rows.size());
  entry.rows_count = static_cast<std::uint32_t>(refinement.rows.size());
  current_.rows.insert(current_.rows.end(), refinement.rows.begin(), refinement.rows.end());
  entry.left_lines = static_cast<std::uint32_t>(left.size());
  entry.right_lines = static_cast<std::uint32_t>(right.size());
  entry.lines_begin = static_cast<std::uint32_t>(current_.line_starts.size());
  const auto append_side = [&](const std::vector<std::vector<CompareTextSpan>>& side,
                               std::size_t lines) {
    for (std::size_t line = 0; line < lines; ++line) {
      current_.line_starts.push_back(static_cast<std::uint32_t>(current_.spans.size()));
      if (line < side.size()) {
        current_.spans.insert(current_.spans.end(), side[line].begin(), side[line].end());
      }
    }
  };
  append_side(refinement.left_spans, left.size());
  append_side(refinement.right_spans, right.size());
  current_.line_starts.push_back(static_cast<std::uint32_t>(current_.spans.size()));
  current_.entries.push_back(entry);
  const auto index = static_cast<std::uint32_t>(current_.entries.size() - 1);
  current_.Index(index);
  return index;
}

HunkRefinementView HunkRefinementCache::Refine(std::span<const std::string_view> left_lines,
                                               std::span<const std::string_view> right_lines,
                                               HunkRefinementStats* stats,
                                               const DiffLexState& left_entry,
                                               const DiffLexState& right_entry) {
  const std::uint64_t hash = HashState(
      HashState(HashSide(HashSide(util::kFnv1aOffsetBasis, left_lines), right_lines), left_entry),
      right_entry);
  std::uint32_t entry = current_.Find(hash, left_lines, right_lines, left_entry, right_entry);
  if (entry == kNoHunkLine && previous_.syntax == current_.syntax) {
    if (const std::uint32_t old = previous_.Find(hash, left_lines, right_lines, left_entry, right_entry);
        old != kNoHunkLine) {
      entry = CopyFrom(previous_, old);
    }
  }
  if (entry != kNoHunkLine) {
    if (stats != nullptr) {
      ++stats->cached_hunks;
    }
    return current_.View(entry);
  }
  RefineHunk(left_lines, right_lines, *current_.syntax, scratch_, stats, left_entry, right_entry);
  return current_.View(Append(hash, left_lines, right_lines, left_entry, right_entry, scratch_));
}

void HunkRefinementCache::EndBuild() {
  std::swap(current_, previous_);
  current_.Clear();
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
                HunkRefinementStats* stats,
                const DiffLexState& left_entry,
                const DiffLexState& right_entry) {
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
  BuildDiffSyntaxTree(left_lines, syntax, scratch.interner, scratch.lhs, left_entry);
  BuildDiffSyntaxTree(right_lines, syntax, scratch.interner, scratch.rhs, right_entry);
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
