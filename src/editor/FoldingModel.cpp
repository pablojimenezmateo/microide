#include "editor/FoldingModel.h"

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

// Returned by MeasureIndent for a line whose every byte is a space or a tab
// (including an empty line). It doubles as the "blank or indent only" predicate:
// nothing else can produce it, so callers must not re-derive that separately.
constexpr std::size_t kSentinelIndent = static_cast<std::size_t>(-1);

// Cache slots for `line_indent_`. `kBlankIndent` is the stored form of
// `kSentinelIndent`; `kUnmeasuredIndent` marks a line whose width has not been
// measured since an edit touched it, so a read measures it then. Both sit above
// any width a real line can produce (a width is bounded by the line's byte count
// times the tab size, and lines are 32-bit-addressed).
constexpr std::uint32_t kBlankIndent = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kUnmeasuredIndent = kBlankIndent - 1;

// Block partition tuning.
//
// The obvious trade is the rebuild of the block an edit lands in (O(block))
// against the number of words a walk applies (O(document / block)), which would
// put the optimum near sqrt(document). But a rebuild reads cached per-line arrays
// rather than document bytes -- about 4 ns a line -- so it is nearly free at any
// size in this range, while each block costs a handful of ALLOCATIONS for its
// word lists the first time it is built. That is the binding cost: at 256 lines,
// first-touching a 50k-line file added ~750 allocations, enough to fail a perf
// gate on its own. Fewer, larger blocks buy that back for a few microseconds of
// extra line walking per refresh.
constexpr std::size_t kTargetBlockLines = 1024;

std::size_t MeasureIndent(std::string_view line, std::size_t tab_size) {
  if (tab_size == 0) tab_size = 1;
  // Count the leading spaces a word at a time. Each space used to contribute one
  // loop iteration and one branch; on deeply nested code that was the whole cost
  // of the scan (the 50k-line fixture averages 131 leading whitespace bytes per
  // line).
  const std::size_t spaces = util::LeadingByteRun(line, ' ');
  if (spaces == line.size()) {
    return kSentinelIndent;  // line is whitespace-only / blank
  }
  if (line[spaces] != '\t') {
    return spaces;  // ordinary space indent, which is almost every line
  }
  // A tab appears: fall back to the exact per-character rule from that point,
  // since a tab advances to the next stop rather than by one column.
  std::size_t indent = spaces;
  for (std::size_t i = spaces; i < line.size(); ++i) {
    const char c = line[i];
    if (c == ' ') {
      ++indent;
    } else if (c == '\t') {
      indent += tab_size - (indent % tab_size);
    } else {
      return indent;
    }
  }
  return kSentinelIndent;  // line is whitespace-only / blank
}

// Distinct bracket bytes the SWAR skip below can filter on. Real inputs ship 3-4
// pairs (6-8 bytes); past this the skip falls back to the scalar loop, which is
// correct for any set, just slower.
constexpr std::size_t kMaxSwarBracketBytes = 8;

// `byte` replicated into all eight lanes of a 64-bit word.
constexpr std::uint64_t LowOnesForByte(unsigned char byte) {
  return 0x0101010101010101ULL * static_cast<std::uint64_t>(byte);
}

// True when the bracket at `column` is inside a string or comment and must not
// take part in fold matching.
//
// When the syntax-highlight LRU is cold for a line, `tokens` is empty and the
// bracket is **not** suppressed. The alternative was forcing full syntax
// highlighting of every scanned line, which thrashed the 256-entry LRU on large
// documents (perf round-4 Finding 1).
inline bool IsSuppressedBracketAt(std::span<const SyntaxTokenKind> tokens, std::size_t column) {
  if (column >= tokens.size()) {
    return false;
  }
  const SyntaxTokenKind kind = tokens[column];
  return kind == SyntaxTokenKind::String || kind == SyntaxTokenKind::Comment;
}

}  // namespace

// 256-entry table: `KindFor(byte)` selects which pair a byte belongs to (1..31),
// or 0 when the byte is not a bracket at all. The bracket-scan inner loop runs
// once per source byte, so a linear scan of the pair vector would be paid per
// byte; the lookup makes the per-byte check a single load and compare.
class FoldingModel::BracketTable {
 public:
  explicit BracketTable(const std::vector<std::pair<char, char>>& pairs) {
    // Cap at 31 distinct pairs; real inputs ship 3-4.
    const std::size_t capped = std::min<std::size_t>(pairs.size(), 31);
    for (std::size_t i = 0; i < capped; ++i) {
      const std::uint8_t kind = static_cast<std::uint8_t>(i + 1);
      const auto& pair = pairs[i];
      const auto open_byte = static_cast<unsigned char>(pair.first);
      const auto close_byte = static_cast<unsigned char>(pair.second);
      if (kind_for_byte_[open_byte] == 0) {
        kind_for_byte_[open_byte] = kind;
      }
      if (kind_for_byte_[close_byte] == 0) {
        kind_for_byte_[close_byte] = kind;
      }
      pair_for_kind_[kind] = pair;
      any_bracket_ = true;
    }
    // Collect the distinct bracket bytes for the SWAR filter. `swar_covers_all_`
    // gates it: the filter is only sound when every bracket byte is represented,
    // so an oversized set disables the fast skip rather than dropping brackets.
    swar_covers_all_ = any_bracket_;
    for (std::size_t byte = 0; byte < kind_for_byte_.size(); ++byte) {
      if (kind_for_byte_[byte] == 0) {
        continue;
      }
      if (match_word_count_ >= kMaxSwarBracketBytes) {
        swar_covers_all_ = false;
        break;
      }
      match_words_[match_word_count_++] = LowOnesForByte(static_cast<unsigned char>(byte));
    }
  }

  bool any() const { return any_bracket_; }
  std::uint8_t KindFor(unsigned char byte) const { return kind_for_byte_[byte]; }
  const std::pair<char, char>& PairForKind(std::uint8_t kind) const {
    return pair_for_kind_[kind];
  }

  // Index of the first byte at/after `from` that could be a bracket, or
  // `line.size()`.
  //
  // Source is almost entirely non-bracket bytes, so filter eight at a time with
  // the has-zero-byte trick against each distinct bracket byte, then resolve
  // inside a flagged word with the exact table. The word test can flag a word
  // with no real bracket in it (borrow across byte lanes); it can never miss
  // one, and a false flag costs only the scalar re-scan of those eight bytes.
  std::size_t NextCandidate(std::string_view line, std::size_t from) const {
    constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
    constexpr std::uint64_t kLowOnes = 0x0101010101010101ULL;
    std::size_t index = from;
    if (swar_covers_all_) {
      while (index + sizeof(std::uint64_t) <= line.size()) {
        std::uint64_t word = 0;
        std::memcpy(&word, line.data() + index, sizeof(word));
        std::uint64_t hits = 0;
        for (std::size_t k = 0; k < match_word_count_; ++k) {
          const std::uint64_t marks = word ^ match_words_[k];
          hits |= (marks - kLowOnes) & ~marks & kHighBits;
        }
        if (hits != 0) {
          break;
        }
        index += sizeof(std::uint64_t);
      }
    }
    for (; index < line.size(); ++index) {
      if (kind_for_byte_[static_cast<unsigned char>(line[index])] != 0) {
        return index;
      }
    }
    return line.size();
  }

  // Append every bracket byte on `line` to `out`.
  void ScanLine(std::string_view line, std::vector<CachedBracket>& out) const {
    for (std::size_t column = NextCandidate(line, 0); column < line.size();
         column = NextCandidate(line, column + 1)) {
      out.push_back(CachedBracket{static_cast<std::uint32_t>(column), line[column]});
    }
  }

 private:
  std::array<std::uint8_t, 256> kind_for_byte_{};
  std::array<std::pair<char, char>, 32> pair_for_kind_{};
  std::array<std::uint64_t, kMaxSwarBracketBytes> match_words_{};
  std::size_t match_word_count_ = 0;
  bool swar_covers_all_ = false;
  bool any_bracket_ = false;
};

// Resolves "is the bracket at this column inside a string or comment?" without a
// hash probe per line.
//
// Only lines in the syntax highlighter's LRU (<= 256 entries) can be suppressed
// at all, and a walk visits lines in increasing order, so a sorted list of those
// line indices plus a cursor answers it in O(1) amortised.
class FoldingModel::SuppressionCursor {
 public:
  SuppressionCursor(const TextViewport* viewport, const std::vector<std::size_t>& sorted_lines)
      : viewport_(viewport), lines_(sorted_lines) {}

  // Tokens for `line_index` if it is one of the cached lines, else empty. Must be
  // called with non-decreasing `line_index`.
  std::span<const SyntaxTokenKind> TokensFor(std::size_t line_index) {
    if (viewport_ == nullptr) {
      return {};
    }
    while (cursor_ < lines_.size() && lines_[cursor_] < line_index) {
      ++cursor_;
    }
    if (cursor_ >= lines_.size() || lines_[cursor_] != line_index) {
      return {};
    }
    return viewport_->HighlightedLineTokensIfCached(line_index);
  }

  void Rewind() { cursor_ = 0; }

 private:
  const TextViewport* viewport_ = nullptr;
  const std::vector<std::size_t>& lines_;
  std::size_t cursor_ = 0;
};

// ---------------------------------------------------------------------------
// Per-line caches
// ---------------------------------------------------------------------------

void FoldingModel::SyncLineIndentCache(std::size_t line_count, std::size_t tab_size,
                                       const LineEditSpan& edit_span) {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::SyncLineIndentCache");
  const std::size_t effective_tab_size = tab_size == 0 ? 1 : tab_size;
  // Widths are measured against a tab size, so a change invalidates all of them.
  const bool tab_size_changed = line_indent_tab_size_ != effective_tab_size;
  line_indent_tab_size_ = effective_tab_size;

  const std::size_t cached_end = edit_span.ResolvedCachedEnd(line_indent_.size());
  const std::size_t current_end = edit_span.ResolvedCurrentEnd(line_count);
  const std::size_t begin = edit_span.begin();

  // The span claims cache[cached_end, size) and document[current_end, line_count)
  // are the same lines, so those two suffixes must be the same length. If they
  // are not, an edit reached the document without being reported and nothing
  // cached can be trusted -- rebuild rather than splice onto a lie.
  const bool splice_is_sound = !tab_size_changed && !edit_span.empty() &&
                               begin <= cached_end && begin <= line_indent_.size() &&
                               line_indent_.size() - cached_end == line_count - current_end;

  if (edit_span.empty() && !tab_size_changed && line_indent_.size() == line_count) {
    return;  // nothing changed since the last sync
  }
  if (!splice_is_sound) {
    line_indent_.assign(line_count, kUnmeasuredIndent);
    return;
  }

  const std::size_t removed = cached_end - begin;
  const std::size_t inserted = current_end - begin;
  if (removed == inserted) {
    // In-line edit: no suffix entry moves, so overwrite in place rather than
    // memmoving the tail twice. This is the dominant keystroke.
    std::fill(line_indent_.begin() + static_cast<std::ptrdiff_t>(begin),
              line_indent_.begin() + static_cast<std::ptrdiff_t>(cached_end), kUnmeasuredIndent);
  } else {
    line_indent_.erase(line_indent_.begin() + static_cast<std::ptrdiff_t>(begin),
                       line_indent_.begin() + static_cast<std::ptrdiff_t>(cached_end));
    line_indent_.insert(line_indent_.begin() + static_cast<std::ptrdiff_t>(begin), inserted,
                        kUnmeasuredIndent);
  }
}

std::uint32_t FoldingModel::IndentAt(LineSpan lines, std::size_t line, std::size_t tab_size,
                                     WorkBudget& budget) {
  std::uint32_t& slot = line_indent_[line];
  if (slot != kUnmeasuredIndent) {
    return slot;
  }
  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldIndentLinesMeasured);
  budget.Spend();
  const std::size_t measured = MeasureIndent(lines[line], tab_size);
  // Clamp so a pathological width can never collide with a reserved slot.
  slot = measured == kSentinelIndent
             ? kBlankIndent
             : static_cast<std::uint32_t>(std::min<std::size_t>(measured, kUnmeasuredIndent - 1));
  return slot;
}

std::size_t FoldingModel::EventIndexForLine(std::size_t line) const {
  // Blocks carry their own event totals, so this is a walk over ~200 blocks plus
  // the lines of one block rather than a prefix sum over every line of the
  // document (which a mid-file keystroke used to pay two or three times).
  EnsureBlockStarts();
  if (blocks_.empty()) {
    return 0;
  }
  const std::size_t block = BlockIndexForLine(line);
  std::size_t index = 0;
  for (std::size_t i = 0; i < block; ++i) {
    index += blocks_[i].event_count;
  }
  const std::size_t stop = std::min(line, line_bracket_count_.size());
  for (std::size_t i = block_start_line_[block]; i < stop; ++i) {
    index += line_bracket_count_[i];
  }
  return index;
}

void FoldingModel::SyncLineBracketCache(LineSpan lines, const BracketTable& table,
                                        const std::vector<std::pair<char, char>>& pairs,
                                        const LineEditSpan& edit_span, bool pairs_changed) {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::SyncLineBracketCache");
  const std::size_t line_count = lines.size();

  // A pair-set change (a language switch) changes which bytes are recorded, so
  // every cached column becomes suspect.
  if (pairs_changed) {
    bracket_cache_pairs_ = pairs;
    line_brackets_.clear();
    line_bracket_count_.clear();
    bracket_cache_valid_through_ = 0;
    return;
  }
  if (bracket_cache_valid_through_ > line_count) {
    // The document shrank past the cached prefix: keep only what it still covers.
    const std::size_t keep = line_count;
    std::size_t events = 0;
    for (std::size_t i = 0; i < keep; ++i) {
      events += line_bracket_count_[i];
    }
    line_bracket_count_.resize(keep);
    line_brackets_.resize(events);
    bracket_cache_valid_through_ = keep;
  }

  const std::size_t begin = edit_span.begin();
  if (edit_span.empty() || begin >= bracket_cache_valid_through_) {
    return;
  }
  const std::size_t cached_end = edit_span.cached_end();
  const std::size_t current_end = edit_span.current_end();
  if (cached_end == LineEditSpan::kToEnd || current_end == LineEditSpan::kToEnd ||
      cached_end > bracket_cache_valid_through_ || current_end > line_count) {
    // The changed window reaches past what is cached (or has no describable end),
    // so nothing below the edit can be carried across. Keep the prefix.
    const std::size_t keep = begin;
    std::size_t events = 0;
    for (std::size_t i = 0; i < keep; ++i) {
      events += line_bracket_count_[i];
    }
    line_bracket_count_.resize(keep);
    line_brackets_.resize(events);
    bracket_cache_valid_through_ = keep;
    return;
  }

  // Rescan just the replaced lines and splice both arrays. Counts plus one flat
  // event array means this is two memmoves and no offset fixup pass.
  const std::size_t first_event = EventIndexForLine(begin);
  // Derived from `first_event`, not walked from 0 again: both are prefix sums
  // over the same array and `begin <= cached_end`, so the second index is the
  // first plus the counts of the replaced window -- usually one or two lines.
  std::size_t last_event = first_event;
  for (std::size_t line = begin; line < cached_end; ++line) {
    last_event += line_bracket_count_[line];
  }
  bracket_event_scratch_.clear();
  bracket_count_scratch_.clear();
  for (std::size_t line = begin; line < current_end; ++line) {
    const std::size_t before = bracket_event_scratch_.size();
    table.ScanLine(lines[line], bracket_event_scratch_);
    bracket_count_scratch_.push_back(
        static_cast<std::uint32_t>(bracket_event_scratch_.size() - before));
  }
  line_brackets_.erase(line_brackets_.begin() + static_cast<std::ptrdiff_t>(first_event),
                       line_brackets_.begin() + static_cast<std::ptrdiff_t>(last_event));
  line_brackets_.insert(line_brackets_.begin() + static_cast<std::ptrdiff_t>(first_event),
                        bracket_event_scratch_.begin(), bracket_event_scratch_.end());
  line_bracket_count_.erase(line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(begin),
                            line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(cached_end));
  line_bracket_count_.insert(line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(begin),
                             bracket_count_scratch_.begin(), bracket_count_scratch_.end());
  bracket_cache_valid_through_ = line_bracket_count_.size();
}

bool FoldingModel::ExtendBracketCache(LineSpan lines, const BracketTable& table,
                                      std::size_t needed_end, WorkBudget& budget) {
  const std::size_t target = std::min(needed_end, lines.size());
  if (!table.any() || bracket_cache_valid_through_ >= target) {
    return true;
  }
  util::PerformanceTrace::Scope perf_scope("FoldingModel::ExtendBracketCache");
  EnsureBlockStarts();
  while (bracket_cache_valid_through_ < target) {
    const std::size_t block = BlockIndexForLine(bracket_cache_valid_through_);
    const std::size_t block_end =
        std::min(block_start_line_[block] + blocks_[block].line_count, target);
    std::uint32_t added = 0;
    bool ran_out = false;
    while (bracket_cache_valid_through_ < block_end) {
      if (!budget.available()) {
        ran_out = true;
        break;
      }
      const std::size_t before = line_brackets_.size();
      table.ScanLine(lines[bracket_cache_valid_through_], line_brackets_);
      const auto count = static_cast<std::uint32_t>(line_brackets_.size() - before);
      line_bracket_count_.push_back(count);
      added += count;
      ++bracket_cache_valid_through_;
      budget.Spend();
      util::AddPerformanceCounter(util::PerfCounterId::EditorFoldBracketLinesScanned);
    }
    blocks_[block].event_count += added;
    if (ran_out) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Block partition
// ---------------------------------------------------------------------------

void FoldingModel::EnsureBlockStarts() const {
  if (!block_starts_dirty_) {
    return;
  }
  block_start_line_.resize(blocks_.size());
  std::size_t start = 0;
  for (std::size_t i = 0; i < blocks_.size(); ++i) {
    block_start_line_[i] = start;
    start += blocks_[i].line_count;
  }
  block_starts_dirty_ = false;
}

std::size_t FoldingModel::BlockIndexForLine(std::size_t line) const {
  EnsureBlockStarts();
  if (blocks_.empty()) {
    return 0;
  }
  const auto it = std::upper_bound(block_start_line_.begin(), block_start_line_.end(), line);
  const std::size_t index = static_cast<std::size_t>(it - block_start_line_.begin());
  return index == 0 ? 0 : index - 1;
}

void FoldingModel::RecomputeBlockEventCount(std::size_t block_index) {
  EnsureBlockStarts();
  Block& block = blocks_[block_index];
  const std::size_t start = block_start_line_[block_index];
  const std::size_t end =
      std::min(start + block.line_count, line_bracket_count_.size());
  std::uint32_t total = 0;
  for (std::size_t line = start; line < end; ++line) {
    total += line_bracket_count_[line];
  }
  block.event_count = total;
}

void FoldingModel::RepartitionAll(std::size_t line_count) {
  const std::size_t block_count =
      line_count == 0 ? 0 : (line_count + kTargetBlockLines - 1) / kTargetBlockLines;
  blocks_.resize(block_count);
  std::size_t remaining = line_count;
  for (std::size_t i = 0; i < block_count; ++i) {
    Block& block = blocks_[i];
    block.line_count = static_cast<std::uint32_t>(std::min(remaining, kTargetBlockLines));
    remaining -= block.line_count;
    block.valid = false;
    block.event_count = 0;
  }
  block_starts_dirty_ = true;
  document_line_count_ = line_count;
  InvalidatePrefixFrom(0);
  for (std::size_t i = 0; i < blocks_.size(); ++i) {
    RecomputeBlockEventCount(i);
  }
}

void FoldingModel::SyncBlocks(std::size_t line_count, const LineEditSpan& edit_span,
                              bool reset_all) {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::SyncBlocks");
  if (reset_all || blocks_.empty()) {
    RepartitionAll(line_count);
    return;
  }
  if (edit_span.empty()) {
    if (document_line_count_ != line_count) {
      // The line count moved with no reported extent: nothing cached can be
      // trusted about where the change landed.
      RepartitionAll(line_count);
    }
    return;
  }

  const std::size_t begin = edit_span.begin();
  const std::size_t cached_end = edit_span.ResolvedCachedEnd(document_line_count_);
  const std::size_t current_end = edit_span.ResolvedCurrentEnd(line_count);
  if (begin > cached_end || begin > document_line_count_ || cached_end > document_line_count_ ||
      current_end > line_count ||
      document_line_count_ - cached_end != line_count - current_end) {
    RepartitionAll(line_count);
    return;
  }

  EnsureBlockStarts();
  const std::size_t first_block = BlockIndexForLine(begin);
  const std::size_t last_block =
      cached_end > begin ? BlockIndexForLine(cached_end - 1) : first_block;
  std::size_t region_old_lines = 0;
  for (std::size_t i = first_block; i <= last_block; ++i) {
    region_old_lines += blocks_[i].line_count;
  }
  const std::ptrdiff_t delta =
      static_cast<std::ptrdiff_t>(current_end) - static_cast<std::ptrdiff_t>(cached_end);
  const std::ptrdiff_t region_new_signed =
      static_cast<std::ptrdiff_t>(region_old_lines) + delta;
  if (region_new_signed < 0) {
    RepartitionAll(line_count);
    return;
  }
  const auto region_new_lines = static_cast<std::size_t>(region_new_signed);
  const std::size_t old_block_count = last_block - first_block + 1;
  const std::size_t new_block_count =
      region_new_lines == 0 ? 0 : (region_new_lines + kTargetBlockLines - 1) / kTargetBlockLines;

  if (new_block_count != old_block_count) {
    // The edited region no longer partitions into the same number of blocks.
    // Splicing the block vector is a memmove over ~200 small records, which is
    // still nothing next to re-deriving every word.
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(first_block),
                  blocks_.begin() + static_cast<std::ptrdiff_t>(last_block + 1));
    blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(first_block), new_block_count,
                   Block{});
  }
  std::size_t remaining = region_new_lines;
  for (std::size_t i = 0; i < new_block_count; ++i) {
    Block& block = blocks_[first_block + i];
    block.line_count = static_cast<std::uint32_t>(std::min(remaining, kTargetBlockLines));
    remaining -= block.line_count;
    block.valid = false;
    block.event_count = 0;
  }
  block_starts_dirty_ = true;
  document_line_count_ = line_count;
  // Everything before the edited region is untouched, so the memoised state at
  // the START of `first_block` still holds; the next boundary does not.
  InvalidatePrefixFrom(first_block + 1);
  EnsureBlockStarts();
  for (std::size_t i = 0; i < new_block_count; ++i) {
    RecomputeBlockEventCount(first_block + i);
  }
}

// ---------------------------------------------------------------------------
// Block summaries
// ---------------------------------------------------------------------------

bool FoldingModel::EnsureBlockSummary(LineSpan lines, const ComputeOptions& options,
                                      const BracketTable& table, std::size_t block_index,
                                      std::size_t event_index, WorkBudget& budget) {
  Block& block = blocks_[block_index];
  if (block.valid) {
    return true;
  }
  if (!budget.available()) {
    return false;
  }
  EnsureBlockStarts();
  const std::size_t start = block_start_line_[block_index];
  const std::size_t end = std::min(start + block.line_count, lines.size());
  if (!ExtendBracketCache(lines, table, end, budget)) {
    return false;
  }
  util::PerformanceTrace::Scope perf_scope("FoldingModel::BuildBlockSummary");
  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldBlockSummariesBuilt);
  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldBlockSummaryLines, end - start);

  // Built into reused scratch and `assign`ed in below, so a block costs one
  // allocation per non-empty list rather than a push_back growth series -- ~2900
  // extra allocations across a 50k-line file, which is a perf gate on its own.
  build_bracket_closers_.clear();
  build_bracket_openers_.clear();
  build_indent_dedents_.clear();
  build_indent_openers_.clear();
  block.last_nonblank = kNoLine;

  if (table.any()) {
    // Reduce the block's bracket events to (closers reaching before it, openers
    // still open at its end). Pairs matched INSIDE the block are deliberately
    // dropped: a fold whose opener and closer both sit in a block outside the
    // resolved window is not something any query can ask for, and the window
    // itself is walked line by line.
    //
    // Suppression (string/comment brackets) is deliberately NOT applied here.
    // The block word is cached across frames while the syntax-highlight LRU it
    // would consult changes as the user *scrolls*, so baking a suppression
    // decision into the word would pin whatever happened to be highlighted when
    // the block was first built. The window walk, which is redone per resolve,
    // applies suppression to the lines that actually matter.
    std::vector<StackBracket>& stack = build_bracket_stack_;
    stack.clear();
    std::size_t event = event_index;
    for (std::size_t line = start; line < end; ++line) {
      const std::size_t count = line_bracket_count_[line];
      for (std::size_t i = 0; i < count; ++i) {
        const CachedBracket& bracket = line_brackets_[event + i];
        const std::uint8_t kind = table.KindFor(static_cast<unsigned char>(bracket.byte));
        if (kind == 0) {
          continue;  // not a bracket under the current pair set
        }
        const auto& pair = table.PairForKind(kind);
        if (pair.first == pair.second) {
          continue;
        }
        const auto rel = static_cast<std::uint32_t>(line - start);
        if (bracket.byte == pair.first) {
          stack.push_back(StackBracket{pair.second, rel});
        } else if (!stack.empty()) {
          if (stack.back().close == bracket.byte) {
            stack.pop_back();
          }
          // A closer that does not match the top is dropped, exactly as the
          // line-by-line scanner drops it.
        } else {
          build_bracket_closers_.push_back(WordCloser{bracket.byte, rel});
        }
      }
      event += count;
    }
    build_bracket_openers_.reserve(stack.size());
    for (const StackBracket& entry : stack) {
      build_bracket_openers_.push_back(
          WordOpener{entry.close, static_cast<std::uint32_t>(entry.line)});
    }
  }

  if (options.use_indent_source) {
    const std::size_t tab_size = options.tab_size == 0 ? 1 : options.tab_size;
    std::vector<StackIndent>& stack = build_indent_stack_;
    stack.clear();
    // Lowest dedent threshold that has already reached before this block. Only a
    // strictly lower one can pop anything more, so the recorded sequence is the
    // running minimum -- which is what keeps the word short.
    std::uint32_t min_threshold = kNoLine;
    std::uint32_t last_nonblank_rel = kNoLine;
    for (std::size_t line = start; line < end; ++line) {
      const std::uint32_t indent = IndentAt(lines, line, tab_size, budget);
      if (indent == kBlankIndent) {
        continue;  // blank lines neither open nor close a block
      }
      while (!stack.empty() && stack.back().level >= indent) {
        stack.pop_back();
      }
      if (stack.empty() && indent < min_threshold) {
        build_indent_dedents_.push_back(WordDedent{indent, last_nonblank_rel});
        min_threshold = indent;
      }
      const auto rel = static_cast<std::uint32_t>(line - start);
      stack.push_back(StackIndent{indent, rel});
      last_nonblank_rel = rel;
    }
    build_indent_openers_.reserve(stack.size());
    for (const StackIndent& entry : stack) {
      build_indent_openers_.push_back(
          WordIndent{entry.level, static_cast<std::uint32_t>(entry.line)});
    }
    block.last_nonblank = last_nonblank_rel;
  }

  block.bracket_closers.assign(build_bracket_closers_.begin(), build_bracket_closers_.end());
  block.bracket_openers.assign(build_bracket_openers_.begin(), build_bracket_openers_.end());
  block.indent_dedents.assign(build_indent_dedents_.begin(), build_indent_dedents_.end());
  block.indent_openers.assign(build_indent_openers_.begin(), build_indent_openers_.end());
  block.valid = true;
  return true;
}

void FoldingModel::LoadPrefixState(std::size_t boundary, WalkState& out) const {
  out.Clear();
  if (boundary >= prefix_states_.size()) {
    return;
  }
  const PrefixState& state = prefix_states_[boundary];
  out.brackets.assign(prefix_bracket_pool_.begin() + state.bracket_offset,
                      prefix_bracket_pool_.begin() + state.bracket_offset + state.bracket_count);
  out.indents.assign(prefix_indent_pool_.begin() + state.indent_offset,
                     prefix_indent_pool_.begin() + state.indent_offset + state.indent_count);
  out.last_nonblank = state.last_nonblank;
}

void FoldingModel::StorePrefixState(std::size_t boundary, const WalkState& state,
                                    std::size_t event_index) {
  if (boundary != prefix_states_.size()) {
    return;  // boundaries are only ever appended in order
  }
  PrefixState entry;
  entry.bracket_offset = static_cast<std::uint32_t>(prefix_bracket_pool_.size());
  entry.bracket_count = static_cast<std::uint32_t>(state.brackets.size());
  entry.indent_offset = static_cast<std::uint32_t>(prefix_indent_pool_.size());
  entry.indent_count = static_cast<std::uint32_t>(state.indents.size());
  entry.event_index = event_index;
  entry.last_nonblank = state.last_nonblank;
  prefix_bracket_pool_.insert(prefix_bracket_pool_.end(), state.brackets.begin(),
                              state.brackets.end());
  prefix_indent_pool_.insert(prefix_indent_pool_.end(), state.indents.begin(),
                             state.indents.end());
  prefix_states_.push_back(entry);
}

void FoldingModel::InvalidatePrefixFrom(std::size_t boundary) {
  if (boundary >= prefix_states_.size()) {
    return;
  }
  const PrefixState& entry = prefix_states_[boundary];
  prefix_bracket_pool_.resize(entry.bracket_offset);
  prefix_indent_pool_.resize(entry.indent_offset);
  prefix_states_.resize(boundary);
}

std::size_t FoldingModel::EnsurePrefixState(LineSpan lines, const ComputeOptions& options,
                                            const BracketTable& table, std::size_t boundary,
                                            WorkBudget& budget) {
  if (blocks_.empty()) {
    return 0;
  }
  if (prefix_states_.empty()) {
    WalkState empty;
    StorePrefixState(0, empty, 0);
  }
  if (boundary < prefix_states_.size()) {
    return boundary;
  }
  util::PerformanceTrace::Scope perf_scope("FoldingModel::EnsurePrefixState");
  EnsureBlockStarts();
  WalkState& state = walk_;
  LoadPrefixState(prefix_states_.size() - 1, state);
  std::size_t event_index = prefix_states_.back().event_index;
  while (prefix_states_.size() <= boundary) {
    const std::size_t block_index = prefix_states_.size() - 1;
    if (block_index >= blocks_.size()) {
      break;
    }
    if (!EnsureBlockSummary(lines, options, table, block_index, event_index, budget)) {
      break;
    }
    util::AddPerformanceCounter(util::PerfCounterId::EditorFoldBlockWordsApplied);
    ApplyBlockWord(blocks_[block_index], block_start_line_[block_index], state, nullptr, 0);
    event_index += blocks_[block_index].event_count;
    StorePrefixState(block_index + 1, state, event_index);
  }
  return prefix_states_.size() - 1;
}

// ---------------------------------------------------------------------------
// Walking
// ---------------------------------------------------------------------------

void FoldingModel::ApplyBlockWord(const Block& block, std::size_t block_start, WalkState& state,
                                  std::vector<FoldRange>* out,
                                  std::size_t emit_opener_limit) const {
  for (const WordCloser& closer : block.bracket_closers) {
    if (state.brackets.empty() || state.brackets.back().close != closer.close) {
      continue;  // unmatched or mismatched: dropped, as the scanner drops it
    }
    const StackBracket top = state.brackets.back();
    state.brackets.pop_back();
    state.NoteBracketPop(top.line);
    const std::size_t closer_line = block_start + closer.line;
    if (out != nullptr && closer_line > top.line && top.line <= emit_opener_limit) {
      out->push_back(FoldRange{top.line, closer_line, FoldSource::Bracket});
    }
  }
  for (const WordDedent& dedent : block.indent_dedents) {
    const std::size_t closer_line = dedent.closer_line == kNoLine
                                        ? state.last_nonblank
                                        : block_start + dedent.closer_line;
    while (!state.indents.empty() && state.indents.back().level >= dedent.threshold) {
      const StackIndent top = state.indents.back();
      state.indents.pop_back();
      state.NoteIndentPop(top.line);
      if (out != nullptr && closer_line != kNoLineIndex && closer_line > top.line &&
          top.line <= emit_opener_limit) {
        out->push_back(FoldRange{top.line, closer_line, FoldSource::Indent});
      }
    }
  }
  for (const WordOpener& opener : block.bracket_openers) {
    state.PushBracket(StackBracket{opener.close, block_start + opener.line});
  }
  for (const WordIndent& opener : block.indent_openers) {
    state.PushIndent(StackIndent{opener.level, block_start + opener.line});
  }
  if (block.last_nonblank != kNoLine) {
    state.last_nonblank = block_start + block.last_nonblank;
  }
}

void FoldingModel::WalkLines(LineSpan lines, const ComputeOptions& options,
                             const BracketTable& table, SuppressionCursor& suppression,
                             std::size_t begin, std::size_t end, std::size_t& event_index,
                             WalkState& state, std::vector<FoldRange>* out,
                             std::size_t emit_opener_limit, WorkBudget& budget) {
  if (begin >= end) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("FoldingModel::WalkLines");
  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldWindowLinesWalked, end - begin);
  const bool do_brackets = table.any();
  const bool do_indent = options.use_indent_source;
  const std::size_t tab_size = options.tab_size == 0 ? 1 : options.tab_size;
  for (std::size_t line = begin; line < end; ++line) {
    if (do_brackets) {
      const std::size_t count =
          line < line_bracket_count_.size() ? line_bracket_count_[line] : 0;
      if (count != 0) {
        // Deferred exactly as the byte scan deferred it: only a line that
        // actually carries a bracket can be suppressed, so only those lines
        // consult the syntax tokens.
        const std::span<const SyntaxTokenKind> tokens = suppression.TokensFor(line);
        for (std::size_t i = 0; i < count; ++i) {
          const CachedBracket& bracket = line_brackets_[event_index + i];
          const std::uint8_t kind = table.KindFor(static_cast<unsigned char>(bracket.byte));
          if (kind == 0) {
            continue;
          }
          if (!tokens.empty() && IsSuppressedBracketAt(tokens, bracket.column)) {
            continue;
          }
          const auto& pair = table.PairForKind(kind);
          if (pair.first == pair.second) {
            continue;
          }
          if (bracket.byte == pair.first) {
            state.PushBracket(StackBracket{pair.second, line});
          } else if (!state.brackets.empty() && state.brackets.back().close == bracket.byte) {
            const StackBracket top = state.brackets.back();
            state.brackets.pop_back();
            state.NoteBracketPop(top.line);
            if (out != nullptr && line > top.line && top.line <= emit_opener_limit) {
              out->push_back(FoldRange{top.line, line, FoldSource::Bracket});
            }
          }
        }
        event_index += count;
      }
    }
    if (do_indent) {
      const std::uint32_t indent = IndentAt(lines, line, tab_size, budget);
      if (indent == kBlankIndent) {
        continue;
      }
      while (!state.indents.empty() && state.indents.back().level >= indent) {
        const StackIndent top = state.indents.back();
        state.indents.pop_back();
        state.NoteIndentPop(top.line);
        if (out != nullptr && state.last_nonblank != kNoLineIndex &&
            state.last_nonblank > top.line && top.line <= emit_opener_limit) {
          out->push_back(FoldRange{top.line, state.last_nonblank, FoldSource::Indent});
        }
      }
      state.PushIndent(StackIndent{indent, line});
      state.last_nonblank = line;
    }
  }
}

void FoldingModel::FlushIndentsAtEof(WalkState& state, std::vector<FoldRange>* out,
                                     std::size_t emit_opener_limit) const {
  while (!state.indents.empty()) {
    const StackIndent top = state.indents.back();
    state.indents.pop_back();
    if (out != nullptr && state.last_nonblank != kNoLineIndex && state.last_nonblank > top.line &&
        top.line <= emit_opener_limit) {
      out->push_back(FoldRange{top.line, state.last_nonblank, FoldSource::Indent});
    }
  }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

bool FoldingModel::Refresh(LineSpan lines, const ComputeOptions& options, std::size_t first_line,
                           std::size_t last_line, std::size_t max_lines,
                           const LineEditSpan& edit_span, const TextViewport* syntax_viewport) {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::Refresh");
  const std::size_t line_count = lines.size();

  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldRefreshCalls);
  const BracketTable table(options.bracket_pairs);
  const bool pairs_changed = bracket_cache_pairs_ != options.bracket_pairs;
  const std::size_t tab_size = options.tab_size == 0 ? 1 : options.tab_size;
  const bool tab_size_changed = line_indent_tab_size_ != 0 && line_indent_tab_size_ != tab_size;
  const bool indent_source_changed = indent_source_enabled_ != options.use_indent_source;
  // Each of these invalidates every block word: the pair set changes which bytes
  // are brackets, the tab size changes every measured indent width, and turning
  // the indent source off leaves indent entries in words that would still be
  // applied.
  const bool reset_all = pairs_changed || tab_size_changed || indent_source_changed;
  indent_source_enabled_ = options.use_indent_source;

  if (line_count == 0) {
    Clear();
    dirty_ = false;
    resolved_ = true;
    resolved_first_line_ = 0;
    resolved_last_line_ = 0;
    complete_ = true;
    return true;
  }

  // ---- resync the incremental caches ------------------------------------
  //
  // Order matters twice over: the collapsed shift reads the PREVIOUS line count,
  // which `SyncBlocks` overwrites, and the bracket splice resolves its flat event
  // index through the PREVIOUS block partition, which `SyncBlocks` replaces.
  ShiftCollapsedRanges(edit_span, line_count);
  SyncLineBracketCache(lines, table, options.bracket_pairs, edit_span, pairs_changed);
  SyncLineIndentCache(line_count, tab_size, edit_span);
  SyncBlocks(line_count, edit_span, reset_all);

  // ---- window ------------------------------------------------------------
  const std::size_t clamped_first = std::min(first_line, line_count - 1);
  const std::size_t clamped_last =
      last_line == kAllLines ? line_count - 1 : std::min(last_line, line_count - 1);
  const std::size_t window_first =
      clamped_first > kWindowPadLines ? clamped_first - kWindowPadLines : 0;
  const std::size_t window_last =
      std::min(line_count - 1, std::max(clamped_first, clamped_last) + kWindowPadLines);

  WorkBudget budget;
  if (max_lines != 0) {
    // The multiplier doubles while the model is still catching up, so a jump into
    // a huge file converges in O(log n) frames instead of O(n / max_lines) --
    // and it resets the moment a resolve completes, so steady-state typing keeps
    // the caller's bound.
    budget.remaining = max_lines > WorkBudget::kUnlimited / budget_multiplier_
                           ? WorkBudget::kUnlimited - 1
                           : max_lines * budget_multiplier_;
  }

  suppression_lines_scratch_.clear();
  if (syntax_viewport != nullptr) {
    syntax_viewport->AppendCachedHighlightedLines(suppression_lines_scratch_);
  }
  SuppressionCursor suppression(syntax_viewport, suppression_lines_scratch_);

  complete_ = true;
  range_scratch_.clear();

  // Reach the state at the window's first block boundary, then walk the rest of
  // that block by line so the window's own resolve starts from an exact state.
  EnsureBlockStarts();
  const std::size_t window_block = BlockIndexForLine(window_first);
  const std::size_t reached = EnsurePrefixState(lines, options, table, window_block, budget);
  if (reached < window_block) {
    complete_ = false;
  }
  const std::size_t scan_from = block_start_line_[reached];
  LoadPrefixState(reached, walk_);
  std::size_t event_index = prefix_states_[reached].event_index;

  // The bracket events for everything we are about to walk by line must be
  // cached; a budget shortfall here only makes the resolve partial. Stopping the
  // line walk at the cached prefix is what keeps a partial resolve honest --
  // walking past it would silently read "this line has no brackets".
  std::size_t line_walk_end = window_last + 1;
  if (!ExtendBracketCache(lines, table, line_walk_end, budget)) {
    complete_ = false;
    if (table.any()) {
      line_walk_end = std::max(scan_from, bracket_cache_valid_through_);
    }
  }

  const std::size_t emit_from = std::clamp(window_first, scan_from, line_walk_end);
  WalkLines(lines, options, table, suppression, scan_from, emit_from, event_index, walk_, nullptr,
            0, budget);
  WalkLines(lines, options, table, suppression, emit_from, line_walk_end, event_index, walk_,
            &range_scratch_, window_last, budget);

  // ---- forward walk: close whatever the window left open ------------------
  //
  // Every stack entry still open at `window_last` has its opener at or before
  // the window, so it is a fold the window needs. Consuming whole block words
  // rather than lines is what makes a top-level `{` closing 50k lines below cost
  // ~200 word applications instead of 50k line visits. The walk stops as soon as
  // the last of those entries is closed (or falls off the depth cap); anything
  // still open below that belongs to somebody else's window.
  walk_.ArmPending(window_last);
  std::size_t line = line_walk_end;
  const std::size_t forward_limit =
      line_count - line_walk_end > kMaxForwardResolveLines ? line_walk_end + kMaxForwardResolveLines
                                                           : line_count;
  if (line < line_count && !walk_.pending_resolved()) {
    util::PerformanceTrace::Scope forward_scope("FoldingModel::ForwardWalk");
    std::size_t block_index = BlockIndexForLine(line);
    const std::size_t partial_end =
        std::min(block_start_line_[block_index] + blocks_[block_index].line_count, line_count);
    if (ExtendBracketCache(lines, table, partial_end, budget)) {
      // Finish the block the window ended inside by line, then take whole words.
      WalkLines(lines, options, table, suppression, line, partial_end, event_index, walk_,
                &range_scratch_, window_last, budget);
      line = partial_end;
      ++block_index;
      while (line < forward_limit && !walk_.pending_resolved()) {
        if (!EnsureBlockSummary(lines, options, table, block_index, event_index, budget)) {
          complete_ = false;
          break;
        }
        util::AddPerformanceCounter(util::PerfCounterId::EditorFoldBlockWordsApplied);
        ApplyBlockWord(blocks_[block_index], block_start_line_[block_index], walk_,
                       &range_scratch_, window_last);
        event_index += blocks_[block_index].event_count;
        line = block_start_line_[block_index] + blocks_[block_index].line_count;
        ++block_index;
      }
    } else {
      complete_ = false;
    }
  }
  if (line >= line_count) {
    // End of document: every indent level still open genuinely ends here.
    FlushIndentsAtEof(walk_, &range_scratch_, window_last);
  }

  FinishRanges();
  // A partial resolve must not claim its window, or the refresh gate would never
  // come back for the rest of it.
  resolved_ = complete_;
  resolved_first_line_ = window_first;
  resolved_last_line_ = window_last;
  RevalidateCollapsedInWindow();
  dirty_ = false;
  ++revision_;
  if (!complete_) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorFoldPartialResolves);
  }
  budget_multiplier_ = complete_ ? 1 : std::min<std::size_t>(budget_multiplier_ * 2, 4096);
  return complete_;
}

bool FoldingModel::ResolveAllFolds(LineSpan lines, const TextViewport* syntax_viewport) {
  ComputeOptions options;
  options.bracket_pairs = bracket_cache_pairs_;
  options.use_indent_source = indent_source_enabled_;
  options.tab_size = line_indent_tab_size_ == 0 ? 4 : line_indent_tab_size_;
  // An empty edit span: the caller has just refreshed, so every per-line cache
  // and block word is current and only the WINDOW needs widening.
  return Refresh(lines, options, 0, kAllLines, /*max_lines=*/0, LineEditSpan{}, syntax_viewport);
}

void FoldingModel::FinishRanges() {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::FinishRanges");
  std::sort(range_scratch_.begin(), range_scratch_.end(),
            [](const FoldRange& a, const FoldRange& b) {
              if (a.opener_line != b.opener_line) {
                return a.opener_line < b.opener_line;
              }
              if (a.source != b.source) {
                return static_cast<int>(a.source) < static_cast<int>(b.source);
              }
              return a.closer_line > b.closer_line;
            });
  // The sort already orders each opener's candidates best-first (bracket before
  // indent, then widest), so keeping the first entry per opener is the dedupe.
  ranges_.clear();
  for (const FoldRange& range : range_scratch_) {
    if (!ranges_.empty() && ranges_.back().opener_line == range.opener_line) {
      continue;
    }
    ranges_.push_back(range);
  }
  range_closer_prefix_max_.resize(ranges_.size());
  std::size_t running = 0;
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    running = std::max(running, ranges_[i].closer_line);
    range_closer_prefix_max_[i] = running;
  }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

namespace {

std::ptrdiff_t IndexOfOpener(const std::vector<FoldRange>& ranges, std::size_t opener_line) {
  const auto it = std::lower_bound(
      ranges.begin(), ranges.end(), opener_line,
      [](const FoldRange& range, std::size_t value) { return range.opener_line < value; });
  if (it == ranges.end() || it->opener_line != opener_line) {
    return -1;
  }
  return static_cast<std::ptrdiff_t>(it - ranges.begin());
}

}  // namespace

std::optional<FoldRange> FoldingModel::FoldStartingAt(std::size_t line) const {
  const auto index = IndexOfOpener(ranges_, line);
  if (index < 0) {
    return std::nullopt;
  }
  return ranges_[static_cast<std::size_t>(index)];
}

std::optional<FoldRange> FoldingModel::InnermostFoldContaining(std::size_t line) const {
  if (ranges_.empty()) {
    return std::nullopt;
  }
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t value, const FoldRange& range) { return value < range.opener_line; });
  if (it == ranges_.begin()) {
    return std::nullopt;
  }
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const auto index = static_cast<std::size_t>(i);
    if (range_closer_prefix_max_[index] < line) {
      return std::nullopt;
    }
    if (ranges_[index].closer_line >= line) {
      return ranges_[index];
    }
  }
  return std::nullopt;
}

void FoldingModel::AppendFoldsContaining(std::size_t line, std::vector<FoldRange>* out) const {
  if (out == nullptr || ranges_.empty()) {
    return;
  }
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t value, const FoldRange& range) { return value < range.opener_line; });
  if (it == ranges_.begin()) {
    return;
  }
  const std::size_t first_out = out->size();
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const auto index = static_cast<std::size_t>(i);
    if (range_closer_prefix_max_[index] < line) {
      break;
    }
    if (ranges_[index].closer_line >= line) {
      out->push_back(ranges_[index]);
    }
  }
  // Collected innermost-first; the contract is outermost-first.
  std::reverse(out->begin() + static_cast<std::ptrdiff_t>(first_out), out->end());
}

// ---------------------------------------------------------------------------
// Collapsed state
// ---------------------------------------------------------------------------

void FoldingModel::BumpCollapseRevision() {
  ++revision_;
  ++layout_revision_;
  collapsed_index_dirty_ = true;
}

void FoldingModel::EnsureCollapsedIndex() const {
  if (!collapsed_index_dirty_) {
    return;
  }
  collapsed_hi_prefix_max_.resize(collapsed_.size());
  std::size_t running = 0;
  for (std::size_t i = 0; i < collapsed_.size(); ++i) {
    running = std::max(running, collapsed_[i].closer_line);
    collapsed_hi_prefix_max_[i] = running;
  }
  collapsed_index_dirty_ = false;
}

bool FoldingModel::CollapseRange(FoldRange range) {
  if (range.closer_line <= range.opener_line) {
    return false;
  }
  const auto it = std::lower_bound(
      collapsed_.begin(), collapsed_.end(), range.opener_line,
      [](const FoldRange& entry, std::size_t value) { return entry.opener_line < value; });
  if (it != collapsed_.end() && it->opener_line == range.opener_line) {
    if (it->closer_line == range.closer_line && it->source == range.source) {
      return false;  // already collapsed at this opener
    }
    *it = range;
    BumpCollapseRevision();
    return true;
  }
  collapsed_.insert(it, range);
  BumpCollapseRevision();
  return true;
}

bool FoldingModel::Collapse(std::size_t opener_line) {
  const auto index = IndexOfOpener(ranges_, opener_line);
  if (index < 0) {
    return false;
  }
  return CollapseRange(ranges_[static_cast<std::size_t>(index)]);
}

bool FoldingModel::Expand(std::size_t opener_line) {
  const auto it = std::lower_bound(
      collapsed_.begin(), collapsed_.end(), opener_line,
      [](const FoldRange& entry, std::size_t value) { return entry.opener_line < value; });
  if (it == collapsed_.end() || it->opener_line != opener_line) {
    return false;
  }
  collapsed_.erase(it);
  BumpCollapseRevision();
  return true;
}

bool FoldingModel::ToggleFold(std::size_t opener_line) {
  if (IsCollapsedAtOpener(opener_line)) {
    return Expand(opener_line);
  }
  return Collapse(opener_line);
}

bool FoldingModel::CollapseAllResolved() {
  if (ranges_.empty()) {
    return false;
  }
  // `ranges_` is already sorted by opener with unique openers, so a merge against
  // the existing collapsed list beats inserting one at a time.
  std::vector<FoldRange> merged;
  merged.reserve(collapsed_.size() + ranges_.size());
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < collapsed_.size() || j < ranges_.size()) {
    if (j >= ranges_.size()) {
      merged.push_back(collapsed_[i++]);
    } else if (i >= collapsed_.size() || ranges_[j].opener_line < collapsed_[i].opener_line) {
      merged.push_back(ranges_[j++]);
    } else if (collapsed_[i].opener_line < ranges_[j].opener_line) {
      merged.push_back(collapsed_[i++]);
    } else {
      // Same opener: the freshly resolved range is the authoritative one.
      merged.push_back(ranges_[j++]);
      ++i;
    }
  }
  const bool changed = merged.size() != collapsed_.size() ||
                       !std::equal(merged.begin(), merged.end(), collapsed_.begin(),
                                   [](const FoldRange& a, const FoldRange& b) {
                                     return a.opener_line == b.opener_line &&
                                            a.closer_line == b.closer_line && a.source == b.source;
                                   });
  if (!changed) {
    return false;
  }
  collapsed_.swap(merged);
  BumpCollapseRevision();
  return true;
}

void FoldingModel::ExpandAll() {
  if (collapsed_.empty()) {
    return;
  }
  collapsed_.clear();
  BumpCollapseRevision();
}

bool FoldingModel::IsCollapsedAtOpener(std::size_t line) const {
  const auto it = std::lower_bound(
      collapsed_.begin(), collapsed_.end(), line,
      [](const FoldRange& entry, std::size_t value) { return entry.opener_line < value; });
  return it != collapsed_.end() && it->opener_line == line;
}

bool FoldingModel::IsLineHidden(std::size_t line) const {
  if (collapsed_.empty()) {
    return false;
  }
  EnsureCollapsedIndex();
  // Largest entry index whose body starts at or before `line`. Bodies start at
  // opener + 1, and `collapsed_` is sorted by opener, so this is a binary search
  // plus the prefix running max of the closers.
  const auto it = std::upper_bound(
      collapsed_.begin(), collapsed_.end(), line,
      [](std::size_t value, const FoldRange& entry) { return value < entry.opener_line + 1; });
  if (it == collapsed_.begin()) {
    return false;
  }
  const auto index = static_cast<std::size_t>(it - collapsed_.begin()) - 1;
  return collapsed_hi_prefix_max_[index] >= line;
}

void FoldingModel::ShiftCollapsedRanges(const LineEditSpan& edit_span, std::size_t line_count) {
  if (collapsed_.empty() || edit_span.empty()) {
    return;
  }
  const std::size_t begin = edit_span.begin();
  const std::size_t cached_end = edit_span.ResolvedCachedEnd(document_line_count_);
  const std::size_t current_end = edit_span.ResolvedCurrentEnd(line_count);
  const std::ptrdiff_t delta =
      static_cast<std::ptrdiff_t>(current_end) - static_cast<std::ptrdiff_t>(cached_end);
  const auto shift = [&](std::size_t value) -> std::size_t {
    if (delta == 0 || value < begin) {
      return value;
    }
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(value) + delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };
  std::size_t out = 0;
  bool changed = false;
  for (std::size_t i = 0; i < collapsed_.size(); ++i) {
    FoldRange range = collapsed_[i];
    range.opener_line = shift(range.opener_line);
    range.closer_line = shift(range.closer_line);
    if (range.closer_line <= range.opener_line || range.opener_line >= line_count) {
      changed = true;
      continue;  // the edit swallowed the fold
    }
    range.closer_line = std::min(range.closer_line, line_count - 1);
    changed = changed || range.opener_line != collapsed_[i].opener_line ||
              range.closer_line != collapsed_[i].closer_line;
    collapsed_[out++] = range;
  }
  if (out != collapsed_.size()) {
    collapsed_.resize(out);
  }
  // Only a real move bumps the revision: the wrapped-row layout cache keys on it,
  // so bumping on every keystroke that touched nothing would rebuild the whole
  // document's row layout per keystroke whenever any fold is collapsed.
  if (changed) {
    BumpCollapseRevision();
  }
}

void FoldingModel::RevalidateCollapsedInWindow() {
  if (collapsed_.empty() || !resolved_) {
    return;
  }
  // A collapsed fold whose opener falls inside the resolved window must still be
  // a fold there; if the edit dissolved it, drop the collapse rather than keep
  // hiding lines behind a fold that no longer exists. Openers outside the window
  // are not observable from here and are left alone.
  std::size_t out = 0;
  bool changed = false;
  for (std::size_t i = 0; i < collapsed_.size(); ++i) {
    const FoldRange& entry = collapsed_[i];
    if (entry.opener_line >= resolved_first_line_ && entry.opener_line <= resolved_last_line_) {
      const auto index = IndexOfOpener(ranges_, entry.opener_line);
      if (index < 0) {
        changed = true;
        continue;
      }
      const FoldRange& current = ranges_[static_cast<std::size_t>(index)];
      if (current.closer_line != entry.closer_line || current.source != entry.source) {
        collapsed_[out++] = current;
        changed = true;
        continue;
      }
    }
    collapsed_[out++] = entry;
  }
  if (out != collapsed_.size()) {
    collapsed_.resize(out);
  }
  if (changed) {
    BumpCollapseRevision();
  }
}

// ---------------------------------------------------------------------------

void FoldingModel::Clear() {
  line_brackets_.clear();
  line_bracket_count_.clear();
  bracket_cache_valid_through_ = 0;
  bracket_cache_pairs_.clear();
  line_indent_.clear();
  line_indent_tab_size_ = 0;
  blocks_.clear();
  block_start_line_.clear();
  block_starts_dirty_ = true;
  prefix_states_.clear();
  prefix_bracket_pool_.clear();
  prefix_indent_pool_.clear();
  ranges_.clear();
  range_scratch_.clear();
  range_closer_prefix_max_.clear();
  resolved_ = false;
  resolved_first_line_ = 0;
  resolved_last_line_ = 0;
  collapsed_.clear();
  collapsed_hi_prefix_max_.clear();
  collapsed_index_dirty_ = true;
  walk_.Clear();
  document_line_count_ = 0;
  budget_multiplier_ = 1;
  complete_ = true;
  dirty_ = true;
  ++revision_;
  ++layout_revision_;
}

}  // namespace microide::editor
