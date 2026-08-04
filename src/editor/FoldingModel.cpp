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
// any width a real line can produce (a width is bounded by the line's byte
// count times the tab size, and lines are 32-bit-addressed).
constexpr std::uint32_t kBlankIndent = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kUnmeasuredIndent = kBlankIndent - 1;

std::size_t MeasureIndent(std::string_view line, std::size_t tab_size) {
  if (tab_size == 0) tab_size = 1;
  // Count the leading spaces a word at a time. This runs for every line of the
  // document on every fold recompute, and each space contributed one loop
  // iteration and one branch; on deeply nested code that is the whole cost of
  // the scan (the 50k-line fixture averages 131 leading whitespace bytes per
  // line, and this pass measured 2.3 ms per keystroke against 0.25 ms when only
  // the first byte was read).
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

// Single-line bracket scanner that emits balanced bracket fold ranges across
// multiple lines. The scanner walks character-by-character; for each open
// bracket it records the line of the opener; the corresponding close on a
// later line emits a fold range. Brackets balanced on the same line are
// ignored (no fold opportunity).
using StackEntry = FoldingModel::BracketStackEntry;

// 256-entry table: BracketKind[byte] selects which pair the byte belongs to (1..127), or 0 when the
// byte is not a bracket character at all. The bracket-scan inner loop runs once per source byte, so
// the previous std::vector<std::pair<char,char>> linear scan was paid for every byte. The lookup
// makes the per-byte check a single load + compare.
// Distinct bracket bytes the SWAR skip below can filter on. Real inputs ship
// 3-4 pairs (6-8 bytes); past this the skip falls back to the scalar loop, which
// is correct for any set, just slower.
constexpr std::size_t kMaxSwarBracketBytes = 8;

// `byte` replicated into all eight lanes of a 64-bit word.
constexpr std::uint64_t kLowOnesForByte(unsigned char byte) {
  return 0x0101010101010101ULL * static_cast<std::uint64_t>(byte);
}

struct BracketLookupTable {
  // For each input byte: 0 = not a bracket, otherwise index+1 into the source `pairs` vector.
  std::array<std::uint8_t, 256> kind_for_byte{};
  // Cached `(open, close)` for each used kind. Index 0 is unused.
  std::array<std::pair<char, char>, 32> pair_for_kind{};
  // `byte * 0x0101010101010101` for each distinct bracket byte, for the
  // eight-bytes-at-a-time skip in NextBracketCandidateWord.
  std::array<std::uint64_t, kMaxSwarBracketBytes> match_words{};
  std::size_t match_word_count = 0;
  bool swar_covers_all_bytes = false;
  bool any_bracket = false;
};

// Index of the first byte at/after `from` that could be a bracket, or
// `line.size()`.
//
// The bracket scan is the dominant cost of recomputing folds on a large file --
// a mid-file keystroke rescans every line after the edit, ~4 MB on the 50k-line
// fixture -- and it ran one 256-entry table lookup per byte. Source is almost
// entirely non-bracket bytes, so filter eight at a time with the has-zero-byte
// trick against each distinct bracket byte, then resolve inside a flagged word
// with the exact table. The word test can flag a word with no real bracket in it
// (borrow across byte lanes); it can never miss one, and a false flag costs only
// the scalar re-scan of those eight bytes.
std::size_t NextBracketCandidate(const BracketLookupTable& table,
                                 std::string_view line,
                                 std::size_t from) {
  constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
  constexpr std::uint64_t kLowOnes = 0x0101010101010101ULL;
  std::size_t index = from;
  if (table.swar_covers_all_bytes) {
    while (index + sizeof(std::uint64_t) <= line.size()) {
      std::uint64_t word = 0;
      std::memcpy(&word, line.data() + index, sizeof(word));
      std::uint64_t hits = 0;
      for (std::size_t k = 0; k < table.match_word_count; ++k) {
        const std::uint64_t marks = word ^ table.match_words[k];
        hits |= (marks - kLowOnes) & ~marks & kHighBits;
      }
      if (hits != 0) {
        break;
      }
      index += sizeof(std::uint64_t);
    }
  }
  for (; index < line.size(); ++index) {
    if (table.kind_for_byte[static_cast<unsigned char>(line[index])] != 0) {
      return index;
    }
  }
  return line.size();
}

BracketLookupTable BuildBracketLookupTable(const std::vector<std::pair<char, char>>& pairs) {
  BracketLookupTable table;
  // Cap at 31 distinct pairs; FoldingModel inputs ship 3-4 pairs in practice.
  const std::size_t capped = std::min<std::size_t>(pairs.size(), 31);
  for (std::size_t i = 0; i < capped; ++i) {
    const std::uint8_t kind = static_cast<std::uint8_t>(i + 1);
    const auto& pair = pairs[i];
    const auto open_byte = static_cast<unsigned char>(pair.first);
    const auto close_byte = static_cast<unsigned char>(pair.second);
    if (table.kind_for_byte[open_byte] == 0) {
      table.kind_for_byte[open_byte] = kind;
    }
    if (table.kind_for_byte[close_byte] == 0) {
      table.kind_for_byte[close_byte] = kind;
    }
    table.pair_for_kind[kind] = pair;
    table.any_bracket = true;
  }
  // Collect the distinct bracket bytes for the SWAR filter. `swar_covers_all_bytes`
  // gates it: the filter is only sound when every bracket byte is represented, so
  // an oversized set disables the fast skip rather than dropping brackets.
  table.swar_covers_all_bytes = table.any_bracket;
  for (std::size_t byte = 0; byte < table.kind_for_byte.size(); ++byte) {
    if (table.kind_for_byte[byte] == 0) {
      continue;
    }
    if (table.match_word_count >= kMaxSwarBracketBytes) {
      table.swar_covers_all_bytes = false;
      break;
    }
    table.match_words[table.match_word_count++] = kLowOnesForByte(static_cast<unsigned char>(byte));
  }
  return table;
}

// Per-line token span hoisted by the bracket scanners. When the syntax-highlight
// LRU is cold for this line, `tokens` is empty and the bracket is **not**
// suppressed — far-from-viewport scans then over-emit a few fold ranges for
// brackets inside strings/comments, which is cheap to live with. The
// alternative was a `viewport->HighlightedLineTokens(line_index)` call **per
// bracket byte**, which forced full syntax highlighting of every scanned line
// and thrashed the 256-entry LRU on large-document fold recomputes
// (perf round-4 Finding 1).
inline bool IsSuppressedBracketAt(std::span<const SyntaxTokenKind> tokens, std::size_t column) {
  if (column >= tokens.size()) {
    return false;
  }
  const SyntaxTokenKind kind = tokens[column];
  return kind == SyntaxTokenKind::String || kind == SyntaxTokenKind::Comment;
}

// Resolves "is the bracket at this column inside a string or comment?" without a
// hash probe per line.
//
// Only lines in the syntax highlighter's LRU (<= 256 entries) can be suppressed
// at all, and the scan walks lines in increasing order, so a sorted list of those
// line indices plus a cursor answers it in O(1) amortized. The probe used to be a
// map lookup for every line carrying a bracket, which is most lines in C-like
// source and was the largest per-line cost left once the byte scan got cheap.
class SuppressionCursor {
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

using CachedBracket = FoldingModel::CachedBracket;

// Append every bracket byte on `line` to `out`.
void ScanLineBrackets(const BracketLookupTable& table, std::string_view line,
                      std::vector<CachedBracket>& out) {
  for (std::size_t column = NextBracketCandidate(table, line, 0); column < line.size();
       column = NextBracketCandidate(table, line, column + 1)) {
    out.push_back(CachedBracket{static_cast<std::uint32_t>(column), line[column]});
  }
}

// Walk cached events for lines [begin_line, end_line_exclusive), maintaining the
// bracket stack and (when `out_ranges` is non-null) emitting a fold range for
// every pair that spans more than one line.
//
// `event_index` must be the flat-array index of the first event on `begin_line`.
void MatchBracketEvents(const std::vector<CachedBracket>& events,
                        const std::vector<std::uint32_t>& counts,
                        std::size_t event_index,
                        const BracketLookupTable& table,
                        std::size_t begin_line,
                        std::size_t end_line_exclusive,
                        std::vector<StackEntry>& stack,
                        std::vector<FoldRange>* out_ranges,
                        SuppressionCursor& suppression) {
  const std::size_t scan_end = std::min(end_line_exclusive, counts.size());
  for (std::size_t line_index = begin_line; line_index < scan_end; ++line_index) {
    const std::size_t count = counts[line_index];
    if (count == 0) {
      continue;
    }
    // Deferred exactly as the byte scan deferred it: only a line that actually
    // carries a bracket can be suppressed, so only those consult the tokens.
    const std::span<const SyntaxTokenKind> tokens = suppression.TokensFor(line_index);
    for (std::size_t i = 0; i < count; ++i) {
      const CachedBracket& event = events[event_index + i];
      const auto byte = static_cast<unsigned char>(event.byte);
      const std::uint8_t kind = table.kind_for_byte[byte];
      if (kind == 0) {
        continue;  // not a bracket under the current pair set
      }
      if (!tokens.empty() && IsSuppressedBracketAt(tokens, event.column)) {
        continue;
      }
      const auto& pair = table.pair_for_kind[kind];
      if (pair.first == pair.second) continue;
      const char c = event.byte;
      if (c == pair.first) {
        stack.push_back({pair.first, pair.second, line_index});
      } else if (c == pair.second && !stack.empty() && stack.back().close == c) {
        const StackEntry top = stack.back();
        stack.pop_back();
        if (out_ranges != nullptr && line_index > top.line) {
          out_ranges->push_back(FoldRange{top.line, line_index, FoldSource::Bracket});
        }
      }
    }
    event_index += count;
  }
}

// Flat-array index of the first event on `line`.
//
// O(line): a prefix sum from 0 over the per-line event counts. A mid-file edit
// resolves this at the edit line, so on a 50k-line file it is a 25,000-iteration
// walk, and the recompute wants it two or three times per keystroke. Scoped so
// its share of the fold resync is visible rather than hidden in the caller's self
// time -- it was the blind spot that made SyncLineBracketCache look like a scan
// cost when most of it is this walk.
std::size_t EventIndexForLine(const std::vector<std::uint32_t>& counts, std::size_t line) {
  util::PerformanceTrace::Scope perf_scope("FoldingModel::EventIndexForLine");
  const std::size_t end = std::min(line, counts.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorFoldEventIndexLinesWalked, end);
  std::size_t index = 0;
  for (std::size_t i = 0; i < end; ++i) {
    index += counts[i];
  }
  return index;
}

void ScanIndentRanges(LineSpan lines,
                      std::size_t end_line_exclusive,
                      std::size_t tab_size,
                      const std::vector<FoldRange>& bracket_ranges,
                      std::size_t max_lines,
                      std::vector<std::uint32_t>& indent_cache,
                      std::vector<FoldRange>& out_ranges,
                      bool& complete) {
  const std::size_t scan_end = std::min(end_line_exclusive, lines.size());
  // Build a sorted set of openers covered by bracket scans so we don't emit
  // an indent fold on the same opener line. Only indices [0, scan_end) are ever
  // read below, so sizing to scan_end (not the whole document) avoids a large
  // unused zero-init on budgeted recomputes of big files; the bounds check below
  // still guards openers that fall past the scan window.
  std::vector<bool> bracket_opener(scan_end, false);
  for (const auto& r : bracket_ranges) {
    if (r.opener_line < bracket_opener.size()) {
      bracket_opener[r.opener_line] = true;
    }
  }

  // Measure the lines whose cached width an edit invalidated. `indent_cache` is
  // kept in sync with the document by SyncLineIndentCache, so in the steady state
  // of typing this loop touches only the edited lines instead of remeasuring the
  // whole scan window (it was the second-largest cost of a keystroke on a large
  // file). The budget now counts real measurements rather than iterations, so a
  // warm cache also lets a budgeted recompute reach further than a cold one.
  std::size_t scanned = 0;
  {
    // Split from the emission pass below: this one is a pure per-line measure and
    // the other is the range walk, and they have very different fixes if either
    // dominates.
    util::PerformanceTrace::Scope perf_scope("FoldingModel::ScanIndentRanges::Measure");
    for (std::size_t i = 0; i < scan_end; ++i) {
      if (indent_cache[i] != kUnmeasuredIndent) {
        continue;
      }
      if (max_lines != 0 && scanned >= max_lines) {
        complete = false;
        break;
      }
      ++scanned;
      const std::size_t measured = MeasureIndent(lines[i], tab_size);
      // Clamp so a pathological width can never collide with a reserved slot.
      indent_cache[i] = measured == kSentinelIndent
                            ? kBlankIndent
                            : static_cast<std::uint32_t>(
                                  std::min<std::size_t>(measured, kUnmeasuredIndent - 1));
    }
  }
  // A line left unmeasured (the budget ran out before reaching it) reads as
  // blank, exactly as the old zero-initialized scratch array did.
  const auto indent_at = [&](std::size_t i) -> std::size_t {
    const std::uint32_t slot = indent_cache[i];
    return (slot == kBlankIndent || slot == kUnmeasuredIndent) ? kSentinelIndent
                                                               : static_cast<std::size_t>(slot);
  };

  // The emission pass only reads the precomputed `indents[]` array; it must not
  // share the measurement loop's budget counter. Because callers pass
  // `max_lines == work_budget == max(max_lines, scan_end)`, the measurement loop
  // above always consumes the full budget first, which would leave this loop
  // with zero remaining budget and silently drop every indent fold on files
  // whose scan window reaches the budget. Reset the counter so emission gets its
  // own budget of `max_lines` visits.
  scanned = 0;
  for (std::size_t i = 0; i < scan_end; ++i) {
    if (max_lines != 0 && scanned >= max_lines) {
      complete = false;
      break;
    }
    ++scanned;
    if (bracket_opener[i]) continue;
    const std::size_t opener_indent = indent_at(i);
    // `kSentinelIndent` IS "blank or indent only": MeasureIndent returns it
    // exactly when every byte of the line is a space or a tab, which is the same
    // predicate LineIsBlankOrIndentOnly computes. Re-asking cost a piece-tree
    // line lookup plus a byte scan for every non-blank line in the document, on
    // every fold recompute -- and with it gone this pass reads no line text at
    // all, only the measured indents.
    if (opener_indent == kSentinelIndent) continue;
    // Find the next non-blank line to determine if a deeper-indented body starts.
    std::size_t body_start = i + 1;
    while (body_start < scan_end && indent_at(body_start) == kSentinelIndent) {
      ++body_start;
    }
    if (body_start >= scan_end) break;
    if (indent_at(body_start) <= opener_indent) continue;
    // Walk forward until a genuine dedent (indent <= opener_indent) or the window
    // boundary.
    std::size_t closer = body_start;
    bool found_dedent = false;
    while (closer + 1 < scan_end) {
      const std::size_t next_indent = indent_at(closer + 1);
      if (next_indent != kSentinelIndent && next_indent <= opener_indent) {
        found_dedent = true;
        break;
      }
      ++closer;
    }
    // Only emit when the block genuinely terminates: either a dedent was found, or
    // the scan window already reaches end-of-document (so the block really ends at
    // EOF). If the window is budget-limited and no dedent was seen, the block is
    // truncated by the window — emitting here would produce a bogus short fold on a
    // deeply-indented body whose real end (or a bracket fold on the same opener)
    // lies past the budget, e.g. a `namespace {` whose `}` is thousands of lines
    // below. Defer it: mark incomplete so a wider scan on scroll resolves it.
    const bool window_reaches_eof = scan_end >= lines.size();
    if (found_dedent || window_reaches_eof) {
      if (closer > i) {
        out_ranges.push_back(FoldRange{i, closer, FoldSource::Indent});
      }
    } else {
      complete = false;
    }
  }
}

// Indexes only the collapsed previous openers. `(opener_line, closer_line, source)` matches the
// equality predicate the previous O(N·M) loop checked.
struct CollapsedOpenerKey {
  std::size_t opener_line;
  std::size_t closer_line;
  FoldSource source;
  bool operator==(const CollapsedOpenerKey& other) const noexcept {
    return opener_line == other.opener_line && closer_line == other.closer_line &&
           source == other.source;
  }
};

void RemapCollapsedFlags(const std::vector<FoldRange>& previous_ranges,
                         const std::vector<bool>& previous_collapsed,
                         std::size_t previous_collapsed_count,
                         const std::vector<FoldRange>& new_ranges,
                         std::vector<bool>& out_collapsed,
                         std::size_t& out_collapsed_count,
                         std::size_t edit_anchor_line,
                         std::ptrdiff_t line_delta) {
  out_collapsed.assign(new_ranges.size(), false);
  out_collapsed_count = 0;
  if (previous_collapsed_count == 0 || previous_ranges.empty() || new_ranges.empty()) {
    return;
  }

  // A line-count-changing edit at/after `edit_anchor_line` shifts every fold line
  // below the anchor by `line_delta`. Shifting the PREVIOUS collapsed opener/closer
  // by that delta before the equality match lets a collapsed fold survive such an
  // edit (its opener moved, so an absolute match would fail and re-expand it).
  const auto shift_line = [&](std::size_t line) -> std::size_t {
    if (line_delta == 0 || line < edit_anchor_line) {
      return line;
    }
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + line_delta;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  };

  // Collect just the collapsed previous openers (typically tiny: only ranges
  // the user explicitly toggled), sorted by the SHIFTED opener_line for a binary
  // search against the new ranges.
  std::vector<std::pair<std::size_t, std::size_t>> collapsed_index;
  collapsed_index.reserve(previous_collapsed_count);
  for (std::size_t j = 0; j < previous_ranges.size(); ++j) {
    if (previous_collapsed[j]) {
      collapsed_index.emplace_back(shift_line(previous_ranges[j].opener_line), j);
    }
  }
  std::sort(collapsed_index.begin(), collapsed_index.end());

  for (std::size_t i = 0; i < new_ranges.size(); ++i) {
    const auto& nr = new_ranges[i];
    auto it = std::lower_bound(
        collapsed_index.begin(), collapsed_index.end(),
        std::pair<std::size_t, std::size_t>{nr.opener_line, 0},
        [](const auto& a, const auto& b) { return a.first < b.first; });
    while (it != collapsed_index.end() && it->first == nr.opener_line) {
      const FoldRange& pr = previous_ranges[it->second];
      if (shift_line(pr.closer_line) == nr.closer_line && pr.source == nr.source) {
        out_collapsed[i] = true;
        ++out_collapsed_count;
        break;
      }
      ++it;
    }
  }
}

// Orders `ranges` ascending by opener line and keeps exactly one range per
// opener: the one with the lowest source, and among those the widest.
//
// This was a comparison sort plus std::unique. Every fold recompute runs it over
// every range in the document -- tens of thousands on a large file, on every
// keystroke -- and the key is a line index bounded by the document, so the sort
// was doing O(n log n) comparisons to order values that can be bucketed. Placing
// each range in a slot indexed by its opener and then compacting in slot order
// produces the identical sequence in O(n + line_count), and `scratch` is owned by
// the model so the document-sized slot array is allocated once rather than per
// keystroke.
constexpr std::uint32_t kNoRangeIndex = std::numeric_limits<std::uint32_t>::max();

// True when `candidate` beats `incumbent` for the same opener line: lower source
// first, then the wider range. Mirrors the old comparator's tie-breaks exactly.
bool RangeWinsForOpener(const FoldRange& candidate, const FoldRange& incumbent) {
  if (candidate.source != incumbent.source) {
    return static_cast<int>(candidate.source) < static_cast<int>(incumbent.source);
  }
  return candidate.closer_line > incumbent.closer_line;
}

void SortDedupeRangesByOpener(std::vector<FoldRange>& ranges,
                              std::vector<std::uint32_t>& scratch,
                              std::vector<FoldRange>& compact_scratch) {
  if (ranges.empty()) {
    return;
  }
  std::size_t max_opener = 0;
  for (const FoldRange& range : ranges) {
    max_opener = std::max(max_opener, range.opener_line);
  }
  // A range count that cannot be indexed by uint32 (or an opener line past what
  // a slot array can address) falls back to the comparison sort. Neither is
  // reachable from a real document, but the bucket path must not silently
  // truncate if one ever is.
  if (ranges.size() > kNoRangeIndex || max_opener >= kNoRangeIndex) {
    std::sort(ranges.begin(), ranges.end(), [](const FoldRange& a, const FoldRange& b) {
      if (a.opener_line != b.opener_line) {
        return a.opener_line < b.opener_line;
      }
      if (a.source != b.source) {
        return static_cast<int>(a.source) < static_cast<int>(b.source);
      }
      return a.closer_line > b.closer_line;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(),
                             [](const FoldRange& a, const FoldRange& b) {
                               return a.opener_line == b.opener_line;
                             }),
                 ranges.end());
    return;
  }

  scratch.assign(max_opener + 1, kNoRangeIndex);
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    std::uint32_t& slot = scratch[ranges[i].opener_line];
    if (slot == kNoRangeIndex || RangeWinsForOpener(ranges[i], ranges[slot])) {
      slot = static_cast<std::uint32_t>(i);
    }
  }
  compact_scratch.clear();
  compact_scratch.reserve(ranges.size());
  for (const std::uint32_t index : scratch) {
    if (index != kNoRangeIndex) {
      compact_scratch.push_back(ranges[index]);
    }
  }
  ranges.swap(compact_scratch);
}

}  // namespace

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

  // The span claims cache[cached_end, cache_size) and document[current_end,
  // line_count) are the same lines, so those two suffixes must be the same
  // length. If they are not, an edit reached the document without being reported
  // and nothing cached can be trusted -- rebuild rather than splice onto a lie.
  const bool splice_is_sound =
      !tab_size_changed && !edit_span.empty() && begin <= cached_end && begin <= line_indent_.size() &&
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

bool FoldingModel::Compute(LineSpan lines,
                           const ComputeOptions& options) {
  return ComputeWithBudget(lines, options, /*max_lines=*/0, LineEditSpan::FullRebuild(),
                           std::numeric_limits<std::size_t>::max(), nullptr);
}

bool FoldingModel::ComputeWithBudget(LineSpan lines,
                                     const ComputeOptions& options,
                                     std::size_t max_lines,
                                     const LineEditSpan& edit_span,
                                     std::size_t target_end_exclusive,
                                     const TextViewport* syntax_viewport) {
  const std::size_t line_count = lines.size();
  constexpr std::size_t kNoResume = std::numeric_limits<std::size_t>::max();

  // Resync the per-line indent widths before anything reads them; this is what
  // turns the indent pass from a whole-document remeasure into an edited-lines
  // remeasure.
  SyncLineIndentCache(line_count, options.tab_size, edit_span);

  // The bracket reuse works off the first changed line. An empty span means no
  // edit landed since the last compute (a scroll-driven extension), which reuses
  // everything before the scan window rather than nothing.
  const std::size_t incremental_resume_line =
      edit_span.empty() ? kNoResume : edit_span.begin();

  // The incremental-resume path reuses the pre-edit bracket ranges whose closer is
  // before the edit anchor, then rescans the tail — cheap for a localized edit on a
  // large file. It needs a snapshot of `ranges_`, so snapshot when a resume will be
  // attempted, not only when a fold is collapsed (the collapse remap has its own
  // gate below). Without this the reuse path was dead whenever nothing was
  // collapsed — the common case — forcing a full rescan on every edit.
  const bool resume_valid = incremental_resume_line != kNoResume &&
                            incremental_resume_line > 0 && incremental_resume_line < line_count &&
                            !options.bracket_pairs.empty();

  // Skip the expensive previous-state copies unless the remap or the resume needs
  // them; the collapse remap is a no-op (all-false) when nothing was collapsed.
  const std::size_t previous_collapsed_count = collapsed_count_;
  // Net line-count change since the previous compute (whose ranges we are about to
  // remap collapse flags from). Combined with the edit anchor it lets the remap
  // shift collapsed openers so a fold survives a line-count-changing edit above it.
  const std::ptrdiff_t collapse_line_delta =
      static_cast<std::ptrdiff_t>(line_count) - static_cast<std::ptrdiff_t>(computed_line_count_);
  const std::size_t collapse_edit_anchor = incremental_resume_line;  // kNoResume => no shift
  std::vector<FoldRange> previous_ranges;
  std::vector<bool> previous_collapsed;
  if (previous_collapsed_count > 0 || resume_valid) {
    util::PerformanceTrace::Scope scope("FoldingModel::CopyPreviousRanges");
    previous_ranges = ranges_;
  }
  if (previous_collapsed_count > 0) {
    previous_collapsed = collapsed_;
  }
  const std::size_t scan_end =
      std::min(target_end_exclusive == kNoResume ? line_count : target_end_exclusive, line_count);

  ranges_.clear();
  collapsed_.clear();
  collapsed_count_ = 0;
  complete_ = scan_end >= line_count;

  auto merge_indent_and_finish = [&](std::vector<FoldRange> bracket_ranges,
                                     bool bracket_scan_complete) -> bool {
    std::vector<FoldRange> indent_ranges;
    if (options.use_indent_source) {
      util::PerformanceTrace::Scope si("FoldingModel::ScanIndentRanges");
      ScanIndentRanges(lines, scan_end, options.tab_size, bracket_ranges, max_lines,
                       line_indent_, indent_ranges, complete_);
    }

    util::PerformanceTrace::Scope sm("FoldingModel::MergeRanges");
    ranges_.reserve(bracket_ranges.size() + indent_ranges.size());
    ranges_.insert(ranges_.end(), bracket_ranges.begin(), bracket_ranges.end());
    ranges_.insert(ranges_.end(), indent_ranges.begin(), indent_ranges.end());
    SortDedupeRangesByOpener(ranges_, merge_by_opener_scratch_, merge_compact_scratch_);
    if (previous_collapsed_count > 0) {
      RemapCollapsedFlags(previous_ranges, previous_collapsed, previous_collapsed_count,
                          ranges_, collapsed_, collapsed_count_, collapse_edit_anchor,
                          collapse_line_delta);
    } else {
      collapsed_.assign(ranges_.size(), false);
      collapsed_count_ = 0;
    }
    // Record the line count these ranges were computed against for the next remap.
    computed_line_count_ = line_count;
    complete_ = complete_ && bracket_scan_complete && scan_end >= line_count;
    // Record how far the scan actually resolved. When the budget cut the scan
    // short (`!complete_`, only possible for a finite `max_lines`) the scan may
    // not have visited every line in `[0, scan_end)`, so cap the claim at the
    // budget; overstating would let the visible-range fast path skip a rescan
    // that is still needed. When complete the full `scan_end` is resolved.
    resolved_prefix_line_count_ = complete_ ? scan_end : std::min(scan_end, max_lines);
    ++revision_;
    return complete_;
  };

  bool kept_prefix_exists = false;
  if (resume_valid) {
    for (const FoldRange& r : previous_ranges) {
      if (r.source == FoldSource::Bracket && r.closer_line < incremental_resume_line) {
        kept_prefix_exists = true;
        break;
      }
    }
  }

  std::vector<FoldRange> bracket_ranges;
  bool bracket_scan_complete = true;
  if (!options.bracket_pairs.empty()) {
    const BracketLookupTable bracket_table = BuildBracketLookupTable(options.bracket_pairs);
    {
      util::PerformanceTrace::Scope sc("FoldingModel::SyncLineBracketCache");
      SyncLineBracketCache(lines, options.bracket_pairs, edit_span, scan_end, max_lines,
                           bracket_scan_complete);
    }

    // The lines whose highlight tokens are cached, once per recompute, so the
    // match walk resolves string/comment suppression with a cursor instead of a
    // hash probe per bracket-carrying line.
    suppression_lines_scratch_.clear();
    if (syntax_viewport != nullptr) {
      syntax_viewport->AppendCachedHighlightedLines(suppression_lines_scratch_);
    }
    SuppressionCursor suppression(syntax_viewport, suppression_lines_scratch_);

    const std::size_t match_end = std::min(scan_end, bracket_cache_valid_through_);
    std::vector<StackEntry> stack;
    std::size_t match_begin = 0;

    if (resume_valid && kept_prefix_exists && incremental_resume_line <= match_end) {
      // Reuse the pre-edit bracket folds that close before the edit, and seed the
      // stack at the edit rather than re-deriving it from line 0.
      if (prefix_stack_valid_ && prefix_stack_line_ == incremental_resume_line) {
        // Same resume line as the last recompute, so every line before it is what
        // it was then (the span's start is the minimum line touched since, so an
        // earlier edit would have lowered it). The stack it produced still holds.
        stack = prefix_stack_;
      } else {
        util::PerformanceTrace::Scope sp("FoldingModel::MatchBracketPrefix");
        MatchBracketEvents(line_brackets_, line_bracket_count_, 0, bracket_table, 0,
                           incremental_resume_line, stack, /*out_ranges=*/nullptr, suppression);
        prefix_stack_ = stack;
        prefix_stack_line_ = incremental_resume_line;
        prefix_stack_valid_ = true;
      }
      for (const FoldRange& r : previous_ranges) {
        if (r.source == FoldSource::Bracket && r.closer_line < incremental_resume_line) {
          bracket_ranges.push_back(r);
        }
      }
      match_begin = incremental_resume_line;
    } else {
      // Whole-window match. Drop the prefix-stack memo: this is the path a freshly
      // loaded or reset document takes, and the memo records which LINE it was
      // built for, not which document.
      prefix_stack_valid_ = false;
    }

    {
      util::PerformanceTrace::Scope st("FoldingModel::MatchBracketRanges");
      MatchBracketEvents(line_brackets_, line_bracket_count_,
                         EventIndexForLine(line_bracket_count_, match_begin), bracket_table,
                         match_begin, match_end, stack, &bracket_ranges, suppression);
    }
  }
  return merge_indent_and_finish(std::move(bracket_ranges), bracket_scan_complete);
}

std::size_t FoldingModel::SyncLineBracketCache(LineSpan lines,
                                               const std::vector<std::pair<char, char>>& pairs,
                                               const LineEditSpan& edit_span,
                                               std::size_t needed_end,
                                               std::size_t max_new_line_scans,
                                               bool& complete) {
  const BracketLookupTable table = BuildBracketLookupTable(pairs);
  const std::size_t line_count = lines.size();

  // A pair-set change (a language switch) changes which bytes are recorded, so
  // every cached column becomes suspect.
  if (bracket_cache_pairs_ != pairs) {
    bracket_cache_pairs_ = pairs;
    line_brackets_.clear();
    line_bracket_count_.clear();
    bracket_cache_valid_through_ = 0;
  }
  if (bracket_cache_valid_through_ > line_count) {
    TruncateLineBracketCache(line_count);
  }

  const std::size_t begin = edit_span.begin();
  if (!edit_span.empty() && begin < bracket_cache_valid_through_) {
    const std::size_t cached_end = edit_span.cached_end();
    const std::size_t current_end = edit_span.current_end();
    if (cached_end == LineEditSpan::kToEnd || current_end == LineEditSpan::kToEnd ||
        cached_end > bracket_cache_valid_through_ || current_end > line_count) {
      // The changed window reaches past what is cached (or has no describable
      // end), so nothing below the edit can be carried across. Keep the prefix.
      TruncateLineBracketCache(begin);
    } else {
      // Rescan just the replaced lines and splice both arrays. Counts + a flat
      // event array means this is two memmoves and no offset fixup.
      const std::size_t first_event = EventIndexForLine(line_bracket_count_, begin);
      const std::size_t last_event = EventIndexForLine(line_bracket_count_, cached_end);
      bracket_event_scratch_.clear();
      bracket_count_scratch_.clear();
      for (std::size_t line = begin; line < current_end; ++line) {
        const std::size_t before = bracket_event_scratch_.size();
        ScanLineBrackets(table, lines[line], bracket_event_scratch_);
        bracket_count_scratch_.push_back(
            static_cast<std::uint32_t>(bracket_event_scratch_.size() - before));
      }
      line_brackets_.erase(line_brackets_.begin() + static_cast<std::ptrdiff_t>(first_event),
                           line_brackets_.begin() + static_cast<std::ptrdiff_t>(last_event));
      line_brackets_.insert(line_brackets_.begin() + static_cast<std::ptrdiff_t>(first_event),
                            bracket_event_scratch_.begin(), bracket_event_scratch_.end());
      line_bracket_count_.erase(
          line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(begin),
          line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(cached_end));
      line_bracket_count_.insert(
          line_bracket_count_.begin() + static_cast<std::ptrdiff_t>(begin),
          bracket_count_scratch_.begin(), bracket_count_scratch_.end());
      bracket_cache_valid_through_ = line_bracket_count_.size();
    }
  }

  // Extend the cached prefix to cover the scan window, scanning only what is
  // missing and stopping at the budget rather than pretending to be complete.
  std::size_t scanned = 0;
  const std::size_t target = std::min(needed_end, line_count);
  while (bracket_cache_valid_through_ < target) {
    if (max_new_line_scans != 0 && scanned >= max_new_line_scans) {
      complete = false;
      break;
    }
    const std::size_t before = line_brackets_.size();
    ScanLineBrackets(table, lines[bracket_cache_valid_through_], line_brackets_);
    line_bracket_count_.push_back(static_cast<std::uint32_t>(line_brackets_.size() - before));
    ++bracket_cache_valid_through_;
    ++scanned;
  }
  return bracket_cache_valid_through_;
}

void FoldingModel::TruncateLineBracketCache(std::size_t line_count) {
  const std::size_t keep = std::min(line_count, line_bracket_count_.size());
  const std::size_t events = EventIndexForLine(line_bracket_count_, keep);
  line_bracket_count_.resize(keep);
  line_brackets_.resize(events);
  bracket_cache_valid_through_ = keep;
}

bool FoldingModel::EnsureFoldsForVisibleRange(
    LineSpan lines,
    const ComputeOptions& options,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    std::size_t max_lines,
    const LineEditSpan& edit_span,
    const TextViewport* syntax_viewport) {
  const std::size_t line_count = lines.size();
  if (line_count == 0) {
    Clear();
    dirty_ = false;
    resolved_prefix_line_count_ = 0;
    return true;
  }

  const std::size_t target_end =
      std::min(line_count, std::max(visible_start_line, visible_end_line) + kVisibleLookAhead + 1);
  if (!dirty_ && resolved_prefix_line_count_ >= target_end) {
    return true;
  }

  // The bracket scanner only emits a fold once it reaches the matching closer,
  // so an opener on a visible line whose `}` sits past the look-ahead would
  // never get a marker. Extend the scan past the look-ahead to a full budget
  // window so any construct whose closer is within `max_lines` of the scan
  // origin resolves immediately. The work budget always covers `scan_end` (as
  // the old `max(max_lines, target_end)` did) so a deep viewport still reaches
  // its own region; on a huge file the look-ahead bound keeps the per-frame
  // span finite and far-below openers keep resolving on scroll.
  // `resolved_prefix_line_count_` is owned by ComputeWithBudget.
  const std::size_t budget = max_lines == 0 ? line_count : max_lines;
  // Grow the resolved prefix GEOMETRICALLY, not by the visible window.
  //
  // ComputeWithBudget always rescans `[0, scan_end)` from the start -- its
  // incremental path only serves a localized edit, not a forward extension -- so
  // extending the prefix by one look-ahead window at a time makes scrolling a
  // large file quadratic: every extension re-walks the whole prefix. Scrolling a
  // 50k-line file measured 108 extensions averaging 2.35 ms each (216 ms, over
  // half of the whole editor_sticky_scroll_scroll scenario), all of it on the
  // shell thread.
  //
  // Doubling makes the number of extensions O(log n) and the total scan work
  // O(n). It does NOT raise the worst-case single hitch: the largest scan under
  // either policy is the one that reaches the end of the file, and the first
  // extension is still bounded by `budget` (the per-frame compute budget), so
  // first paint keeps its bound.
  const std::size_t doubled_prefix = resolved_prefix_line_count_ >= line_count / 2
                                         ? line_count
                                         : resolved_prefix_line_count_ * 2;
  const std::size_t scan_end =
      std::min(line_count, std::max({target_end, budget, doubled_prefix}));
  const std::size_t work_budget = max_lines == 0 ? line_count : std::max(max_lines, scan_end);
  ComputeWithBudget(lines, options, work_budget, edit_span, scan_end, syntax_viewport);
  dirty_ = false;
  return true;
}

namespace {

// ranges_ is kept sorted by `opener_line` (and de-duplicated to one entry per
// opener) after `SortDedupeRangesByOpener`. Resolve an opener via binary search.
std::ptrdiff_t IndexOfOpener(const std::vector<FoldRange>& ranges,
                             std::size_t opener_line) {
  const auto it = std::lower_bound(
      ranges.begin(), ranges.end(), opener_line,
      [](const FoldRange& r, std::size_t v) { return r.opener_line < v; });
  if (it == ranges.end() || it->opener_line != opener_line) {
    return -1;
  }
  return static_cast<std::ptrdiff_t>(it - ranges.begin());
}

}  // namespace

void FoldingModel::EnsureLookupCache() const {
  if (cached_revision_ == revision_) {
    return;
  }
  cached_collapsed_intervals_.clear();
  cached_collapsed_hi_prefix_max_.clear();
  cached_range_closer_prefix_max_.clear();
  cached_collapsed_intervals_.reserve(ranges_.size());
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    if (i < collapsed_.size() && collapsed_[i] &&
        ranges_[i].closer_line > ranges_[i].opener_line) {
      cached_collapsed_intervals_.push_back(
          CollapsedInterval{ranges_[i].opener_line + 1, ranges_[i].closer_line});
    }
  }
  // Collapsed intervals are emitted in opener order (ranges_ is sorted by opener),
  // but interleave with non-collapsed entries we skipped, so the result is still
  // sorted by `lo`. Build the prefix running-max of `hi` for IsLineHidden.
  cached_collapsed_hi_prefix_max_.resize(cached_collapsed_intervals_.size());
  {
    std::size_t running = 0;
    for (std::size_t i = 0; i < cached_collapsed_intervals_.size(); ++i) {
      running = std::max(running, cached_collapsed_intervals_[i].hi);
      cached_collapsed_hi_prefix_max_[i] = running;
    }
  }
  // Per-range prefix running-max of `closer_line` for InnermostFoldContaining.
  cached_range_closer_prefix_max_.resize(ranges_.size());
  {
    std::size_t running = 0;
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
      running = std::max(running, ranges_[i].closer_line);
      cached_range_closer_prefix_max_[i] = running;
    }
  }
  cached_revision_ = revision_;
}

bool FoldingModel::ToggleFold(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  const std::size_t pos = static_cast<std::size_t>(idx);
  const bool was_collapsed = collapsed_[pos];
  collapsed_[pos] = !was_collapsed;
  collapsed_count_ += was_collapsed ? std::size_t{0} : std::size_t{1};
  collapsed_count_ -= was_collapsed ? std::size_t{1} : std::size_t{0};
  ++revision_;
  return true;
}

bool FoldingModel::Collapse(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = true;
  ++collapsed_count_;
  ++revision_;
  return true;
}

bool FoldingModel::Expand(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (!collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = false;
  --collapsed_count_;
  ++revision_;
  return true;
}

void FoldingModel::CollapseAll() {
  if (collapsed_count_ == collapsed_.size()) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), true);
  collapsed_count_ = collapsed_.size();
  ++revision_;
}

void FoldingModel::ExpandAll() {
  if (collapsed_count_ == 0) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), false);
  collapsed_count_ = 0;
  ++revision_;
}

bool FoldingModel::IsLineHidden(std::size_t line) const {
  EnsureLookupCache();
  if (cached_collapsed_intervals_.empty()) {
    return false;
  }
  // Find the largest interval index whose `lo` is <= line.
  const auto it = std::upper_bound(
      cached_collapsed_intervals_.begin(), cached_collapsed_intervals_.end(), line,
      [](std::size_t v, const CollapsedInterval& a) { return v < a.lo; });
  if (it == cached_collapsed_intervals_.begin()) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(it - cached_collapsed_intervals_.begin()) - 1;
  // The running-max of `hi` over [0..idx] tells us whether any prefix interval
  // covers `line`; since intervals are sorted by `lo`, lo <= line holds for the
  // whole prefix.
  return cached_collapsed_hi_prefix_max_[idx] >= line;
}

std::optional<FoldRange> FoldingModel::FoldStartingAt(std::size_t line) const {
  const auto idx = IndexOfOpener(ranges_, line);
  if (idx < 0) {
    return std::nullopt;
  }
  return ranges_[static_cast<std::size_t>(idx)];
}

std::optional<FoldRange> FoldingModel::InnermostFoldContaining(std::size_t line) const {
  if (ranges_.empty()) {
    return std::nullopt;
  }
  // Find the rightmost range whose opener_line <= line.
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t v, const FoldRange& r) { return v < r.opener_line; });
  if (it == ranges_.begin()) {
    return std::nullopt;
  }
  EnsureLookupCache();
  // Walk left checking closer_line >= line. The prefix running-max of closer
  // lets us early-exit: if max closer in [0..i] is < line, no fold in that
  // prefix can contain `line`.
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const std::size_t idx = static_cast<std::size_t>(i);
    if (cached_range_closer_prefix_max_[idx] < line) {
      return std::nullopt;
    }
    if (ranges_[idx].closer_line >= line) {
      return ranges_[idx];
    }
  }
  return std::nullopt;
}

void FoldingModel::AppendFoldsContaining(std::size_t line,
                                          std::vector<FoldRange>* out) const {
  if (out == nullptr || ranges_.empty()) {
    return;
  }
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t v, const FoldRange& r) { return v < r.opener_line; });
  if (it == ranges_.begin()) {
    return;
  }
  EnsureLookupCache();
  // Collect into a local buffer first so we can reverse to outermost-first.
  std::vector<FoldRange> ancestors;
  ancestors.reserve(8);
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const std::size_t idx = static_cast<std::size_t>(i);
    if (cached_range_closer_prefix_max_[idx] < line) {
      break;
    }
    if (ranges_[idx].closer_line >= line) {
      ancestors.push_back(ranges_[idx]);
    }
  }
  // ancestors is in decreasing opener_line order; reverse to outer-first.
  for (auto rit = ancestors.rbegin(); rit != ancestors.rend(); ++rit) {
    out->push_back(*rit);
  }
}

bool FoldingModel::IsCollapsedAtOpener(std::size_t line) const {
  const auto idx = IndexOfOpener(ranges_, line);
  if (idx < 0) {
    return false;
  }
  return collapsed_[static_cast<std::size_t>(idx)];
}

void FoldingModel::Clear() {
  line_brackets_.clear();
  line_bracket_count_.clear();
  bracket_cache_valid_through_ = 0;
  bracket_cache_pairs_.clear();
  line_indent_.clear();
  line_indent_tab_size_ = 0;
  ranges_.clear();
  collapsed_.clear();
  prefix_stack_.clear();
  prefix_stack_valid_ = false;
  collapsed_count_ = 0;
  complete_ = true;
  dirty_ = true;
  resolved_prefix_line_count_ = 0;
  computed_line_count_ = 0;
  ++revision_;
}

}  // namespace microide::editor
