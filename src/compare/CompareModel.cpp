#include "compare/CompareModel.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "util/FlatDedupSet.h"
#include "util/Fnv1a.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/SaturatingMath.h"
#include "util/StringUtil.h"

namespace microide::compare {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxLineLcsMatrixCells = 250'000;
constexpr std::size_t kMaxHunkAlignmentMatrixCells = 65'536;
constexpr std::size_t kMaxIntralineLcsMatrixCells = 65'536;

// Cumulative cap on intra-line DP cells spent refining the Modified rows of a
// SINGLE hunk. The oversized-hunk positional fallback (see AlignHunkLines) emits
// min(left,right) Modified pairs with NO bound on that count, and each pair can
// otherwise cost up to kMaxIntralineLcsMatrixCells — so a huge hunk of long
// modified lines multiplies out to billions of comparisons on the UI thread. Once
// this per-hunk budget is spent, the remaining Modified rows fall back to
// whole-line-changed (no character-level refinement). Sized so every realistic
// hunk keeps full refinement (~128 worst-case-long modified pairs) while a
// pathological hunk stays bounded.
constexpr std::size_t kMaxHunkIntralineTotalCells = 8'388'608;

// Above this per-side byte length we skip intra-line span refinement for a
// modified row and mark the whole line changed. The intra-line helpers allocate
// O(n) codepoint-offset and token vectors per line; on a single very long line (a
// minified bundle or a binary blob from `git show`) that is a hundreds-of-MB
// spike on the UI thread. Character-level highlighting on such a line is not
// meaningful anyway, so the whole-line fallback loses nothing a user would see.
constexpr std::size_t kMaxIntralineTextBytes = 64 * 1024;

enum class LineTokenKind {
  Word,
  Whitespace,
  Symbol,
};

struct LineToken {
  std::size_t start = 0;
  std::size_t end = 0;
  LineTokenKind kind = LineTokenKind::Word;
};

struct TokenizedLine {
  std::string_view text;
  std::vector<LineToken> tokens;
  std::vector<std::size_t> significant_token_indices;
  std::size_t significant_token_bytes = 0;
};

enum class HunkAlignmentKind {
  Pair,
  Delete,
  Insert,
};

bool ProductExceeds(std::size_t left, std::size_t right, std::size_t limit) {
  if (left == 0 || right == 0) {
    return false;
  }
  return left > limit / right;
}

std::uint64_t DurationNs(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

LineTokenKind ClassifyCodepoint(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return LineTokenKind::Symbol;
  }

  const unsigned char byte = static_cast<unsigned char>(text[offset]);
  if (byte > 0x7F) {
    return LineTokenKind::Word;
  }
  if (util::IsAsciiSpace(static_cast<unsigned char>(byte)) != 0) {
    return LineTokenKind::Whitespace;
  }
  if (util::IsAsciiAlnum(static_cast<unsigned char>(byte)) != 0 || byte == '_') {
    return LineTokenKind::Word;
  }
  return LineTokenKind::Symbol;
}

void BuildUtf8OffsetsInto(std::string_view text, std::vector<std::size_t>& offsets) {
  offsets.clear();
  offsets.reserve(text.size() + 1);
  for (std::size_t offset = 0; offset < text.size();) {
    offsets.push_back(offset);
    offset += util::Utf8SequenceLength(text, offset);
  }
  offsets.push_back(text.size());
}

void TokenizeLineInto(std::string_view text, TokenizedLine& tokenized) {
  tokenized.tokens.clear();
  tokenized.significant_token_indices.clear();
  tokenized.significant_token_bytes = 0;
  tokenized.text = text;
  // Tokens are grouped runs (only symbols are per-codepoint), so the count is
  // well below the byte length. Reserving one token per byte over-allocates
  // badly on long/multibyte lines; a half-length heuristic fits the common case
  // without repeated growth on symbol-dense lines.
  tokenized.tokens.reserve(text.size() / 2 + 8);
  for (std::size_t offset = 0; offset < text.size();) {
    const LineTokenKind kind = ClassifyCodepoint(text, offset);
    const std::size_t start = offset;
    offset += util::Utf8SequenceLength(text, offset);
    if (kind != LineTokenKind::Symbol) {
      while (offset < text.size() && ClassifyCodepoint(text, offset) == kind) {
        offset += util::Utf8SequenceLength(text, offset);
      }
    }

    tokenized.tokens.push_back(LineToken{
        .start = start,
        .end = offset,
        .kind = kind,
    });
    if (kind != LineTokenKind::Whitespace) {
      tokenized.significant_token_indices.push_back(tokenized.tokens.size() - 1);
      tokenized.significant_token_bytes += offset - start;
    }
  }
}

// Per-thread scratch for the intraline (per-Modified-row) refinement. Each of
// these buffers used to be a fresh heap vector per row — token vectors, changed
// flags, the LCS DP matrix (up to kMaxIntralineLcsMatrixCells entries), and the
// codepoint offset tables — roughly a dozen allocations for every modified line
// of every diff. Refinement is strictly per-row and never re-entrant, so one
// arena per thread reuses the capacity across the whole file.
//
// The changed-flag vectors are `char`, not `vector<bool>`: the bit-packed
// specialization cannot be memset-filled and costs a shift/mask on every access
// in the DP backtrack.
struct IntralineScratch {
  TokenizedLine left_tokens;
  TokenizedLine right_tokens;
  std::vector<std::size_t> left_offsets;
  std::vector<std::size_t> right_offsets;
  std::vector<char> left_changed;
  std::vector<char> right_changed;
  std::vector<std::size_t> token_dp;
  std::vector<int> codepoint_dp;
};

IntralineScratch& Scratch() {
  thread_local IntralineScratch scratch;
  return scratch;
}

// Per-thread scratch for hunk alignment, the other per-hunk allocation cluster:
// two TokenizedLine-per-line vectors (two heap blocks *each*), the lazily-filled
// similarity matrix, and the DP cost/choice matrices. Disjoint from
// IntralineScratch above — alignment finishes before row refinement starts, but
// keeping them separate means neither can ever clobber the other.
//
// The tokenized vectors are only ever GROWN (never resized down), so each
// element keeps the token-vector capacity it reached on an earlier hunk; only
// the first `count` entries of each are live in any one call.
struct HunkAlignScratch {
  std::vector<std::string_view> deleted_lines;
  std::vector<std::string_view> inserted_lines;
  std::vector<HunkAlignmentKind> alignment;
  std::vector<TokenizedLine> left_tokenized;
  std::vector<TokenizedLine> right_tokenized;
  std::vector<double> similarity;
  std::vector<double> dp;
  std::vector<HunkAlignmentKind> choice;
  std::vector<std::size_t> common_tokens;
};

HunkAlignScratch& HunkScratch() {
  thread_local HunkAlignScratch scratch;
  return scratch;
}

// Grow-only tokenization into a reused vector: entries past `count` keep their
// (now stale) contents and, crucially, their heap capacity for the next hunk.
void TokenizeLinesInto(const std::vector<std::string_view>& lines,
                       std::vector<TokenizedLine>& out) {
  if (out.size() < lines.size()) {
    out.resize(lines.size());
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    TokenizeLineInto(lines[i], out[i]);
  }
}

// Sum of significant (non-whitespace) token bytes for `text`, matching
// TokenizedLine::significant_token_bytes without allocating the token vectors.
// Used by the line-match weight where only this scalar is needed.
std::size_t SignificantTokenBytes(std::string_view text) {
  std::size_t total = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    const LineTokenKind kind = ClassifyCodepoint(text, offset);
    const std::size_t start = offset;
    offset += util::Utf8SequenceLength(text, offset);
    if (kind != LineTokenKind::Symbol) {
      while (offset < text.size() && ClassifyCodepoint(text, offset) == kind) {
        offset += util::Utf8SequenceLength(text, offset);
      }
    }
    if (kind != LineTokenKind::Whitespace) {
      total += offset - start;
    }
  }
  return total;
}

bool TokenEquals(std::string_view left,
                 const LineToken& left_token,
                 std::string_view right,
                 const LineToken& right_token) {
  return left_token.kind == right_token.kind &&
         left.substr(left_token.start, left_token.end - left_token.start) ==
             right.substr(right_token.start, right_token.end - right_token.start);
}

std::size_t TokenMatchWeight(const LineToken& token) {
  const std::size_t token_bytes = token.end - token.start;
  switch (token.kind) {
    case LineTokenKind::Word:
      return token_bytes + 4;
    case LineTokenKind::Whitespace:
    case LineTokenKind::Symbol:
    default:
      return token_bytes > 0 ? 1 : 0;
  }
}

std::size_t CommonSignificantTokenBytes(const TokenizedLine& left, const TokenizedLine& right,
                                        std::vector<std::size_t>& dp) {
  const std::size_t left_count = left.significant_token_indices.size();
  const std::size_t right_count = right.significant_token_indices.size();
  if (left_count == 0 || right_count == 0) {
    return 0;
  }

  // Cap the token-LCS DP exactly as the intraline-span path (PopulateTokenChangedSpans)
  // does. AlignHunkLines only gates on the number of LINES, so a 1x1 hunk of a single
  // enormous line (a minified bundle: one ~MB line, one byte changed) sails past that
  // gate and reaches here, where an ungated `(left_count+1)*(right_count+1)` DP is
  // O(tokens^2) — hundreds of GB / an OOM crash on the synchronous UI-thread compare
  // build. Over budget, report no common tokens: LineSimilarity maps that to 0.0, so
  // the pair falls back to the same correct Delete+Insert alignment the coarse path
  // already produces for oversized hunks.
  if (ProductExceeds(left_count + 1, right_count + 1, kMaxIntralineLcsMatrixCells)) {
    return 0;
  }

  // Only at(0, 0) is ever read — there is no backtrack — and every cell depends
  // solely on the row below it, so ONE rolling row suffices. That turns the
  // working set from O(left * right) into O(right) and, more importantly, shrinks
  // the per-pair re-zero from up to kMaxIntralineLcsMatrixCells words to
  // right_count + 1. This runs up to left_lines * right_lines times per hunk, so
  // the full matrix was memsetting tens of MB across a dense diff.
  //
  // dp[j] holds at(i + 1, j) on entry to row i and at(i, j) on exit. Walking j
  // downward means dp[j + 1] has already been overwritten with at(i, j + 1), so
  // the diagonal at(i + 1, j + 1) is carried in `diagonal` instead of read back.
  dp.assign(right_count + 1, 0);

  for (std::size_t i = left_count; i-- > 0;) {
    const LineToken& left_token = left.tokens[left.significant_token_indices[i]];
    const std::size_t left_length = left_token.end - left_token.start;
    const std::string_view left_text = left.text.substr(left_token.start, left_length);
    std::size_t diagonal = 0;  // at(i + 1, right_count) is always 0
    for (std::size_t j = right_count; j-- > 0;) {
      const LineToken& right_token = right.tokens[right.significant_token_indices[j]];
      const std::size_t below = dp[j];  // at(i + 1, j), not yet overwritten
      const std::size_t length = right_token.end - right_token.start;
      const std::size_t value =
          (length == left_length &&
           right.text.compare(right_token.start, length, left_text) == 0)
              ? left_length + diagonal
              : std::max(below, dp[j + 1]);  // dp[j + 1] is already at(i, j + 1)
      diagonal = below;
      dp[j] = value;
    }
  }

  return dp[0];
}

std::size_t LineMatchWeight(std::string_view text,
                            std::size_t significant_token_bytes,
                            std::size_t occurrences) {
  const std::size_t informative_bytes =
      significant_token_bytes > 0 ? significant_token_bytes : (text.empty() ? 1 : std::size_t{2});
  const std::size_t rarity = std::max<std::size_t>(1, occurrences);
  return std::max<std::size_t>(1, (informative_bytes + 8) / rarity);
}

double LineSimilarity(const TokenizedLine& left, const TokenizedLine& right,
                      std::vector<std::size_t>& token_scratch) {
  if (left.text == right.text) {
    return 1.0;
  }
  if (left.significant_token_indices.empty() && right.significant_token_indices.empty()) {
    return 1.0;
  }
  if (left.significant_token_bytes == 0 || right.significant_token_bytes == 0) {
    return 0.0;
  }

  const std::size_t common_bytes = CommonSignificantTokenBytes(left, right, token_scratch);
  if (common_bytes == 0) {
    return 0.0;
  }
  return std::clamp(
      (2.0 * static_cast<double>(common_bytes)) /
          static_cast<double>(left.significant_token_bytes + right.significant_token_bytes),
      0.0, 1.0);
}

double AlignmentPairCost(double similarity) {
  const double clamped_similarity = std::clamp(similarity, 0.0, 1.0);
  return 1.15 - (0.85 * clamped_similarity);
}

bool CanPairAlignedLines(double similarity,
                         std::size_t left_count,
                         std::size_t right_count) {
  if (left_count == 1 && right_count == 1) {
    // A 1-del/1-add hunk always pairs as a single Modified row, even at ~0
    // similarity: a single-line change is by-design shown in place with an
    // intra-line diff (pinned by TestCompareManyTokenLineBoundsAlignmentDp). Do not
    // gate this on similarity.
    return true;
  }
  if ((left_count == 1 && right_count > 1) || (right_count == 1 && left_count > 1)) {
    return false;
  }
  return similarity >= 0.35;
}

void AlignHunkLinesInto(const std::vector<std::string_view>& deleted_lines,
                        const std::vector<std::string_view>& inserted_lines,
                        CompareBuildProfile* profile,
                        std::vector<HunkAlignmentKind>& alignment) {
  const std::size_t left_count = deleted_lines.size();
  const std::size_t right_count = inserted_lines.size();
  HunkAlignScratch& scratch = HunkScratch();
  alignment.clear();
  if (left_count == 0) {
    alignment.assign(right_count, HunkAlignmentKind::Insert);
    return;
  }
  if (right_count == 0) {
    alignment.assign(left_count, HunkAlignmentKind::Delete);
    return;
  }
  if (ProductExceeds(left_count + 1, right_count + 1, kMaxHunkAlignmentMatrixCells)) {
    if (profile != nullptr) {
      ++profile->fallback_hunk_alignment_calls;
    }
    // Positional fallback for a hunk too large for the DP: pair the first
    // min(left,right) rows as Modified. A similarity gate here (rendering
    // low-token-similarity positional pairs as delete+insert) was evaluated and
    // rejected — it changes the pinned fallback contract and degrades the common
    // systematic-rename case (e.g. `left-N` -> `right-N`) from a readable
    // side-by-side Modified row into split delete/insert rows. Kept as positional
    // pairing by design; see the won't-do note in known-tech-debt.md.
    const std::size_t paired = std::min(left_count, right_count);
    alignment.reserve(left_count + right_count - paired);
    alignment.insert(alignment.end(), paired, HunkAlignmentKind::Pair);
    alignment.insert(alignment.end(), left_count - paired, HunkAlignmentKind::Delete);
    alignment.insert(alignment.end(), right_count - paired, HunkAlignmentKind::Insert);
    return;
  }
  if (profile != nullptr) {
    ++profile->exact_hunk_alignment_calls;
  }

  std::vector<TokenizedLine>& left_tokenized = scratch.left_tokenized;
  std::vector<TokenizedLine>& right_tokenized = scratch.right_tokenized;
  TokenizeLinesInto(deleted_lines, left_tokenized);
  TokenizeLinesInto(inserted_lines, right_tokenized);

  // Whether a Pair is reachable at all for this hunk shape. CanPairAlignedLines
  // rejects 1-vs-many and many-vs-1 purely on the counts, so for those hunks the
  // similarity of every cell is computed and then discarded — an entire hunk of
  // O(tokens^2) token-LCS runs for nothing. Hoist that decision out of the loop.
  const bool pairing_possible = !((left_count == 1 && right_count > 1) ||
                                  (right_count == 1 && left_count > 1));
  // The 1x1 hunk always pairs and feeds the exact similarity into
  // AlignmentPairCost, so the cheap-bound skip below must not apply to it.
  const bool similarity_gates_pairing = !(left_count == 1 && right_count == 1);

  // Compute similarities lazily: -1 means "not yet computed".
  std::vector<double>& similarity = scratch.similarity;
  similarity.assign(left_count * right_count, -1.0);
  std::vector<std::size_t>& common_token_scratch = scratch.common_tokens;
  auto similarity_at = [&](std::size_t i, std::size_t j) -> double {
    double& val = similarity[i * right_count + j];
    if (val < 0.0) {
      const TokenizedLine& left_line = left_tokenized[i];
      const TokenizedLine& right_line = right_tokenized[j];
      // common_bytes can never exceed min(left_bytes, right_bytes), so
      //   similarity <= 2 * min / (left + right).
      // When that ceiling cannot reach the 0.35 pairing gate the exact value is
      // never read (the Pair branch is skipped), so the O(tokens^2) LCS below is
      // pure waste. Lines of very different length — the common shape in a real
      // hunk — are rejected here in a few instructions.
      const std::size_t left_bytes = left_line.significant_token_bytes;
      const std::size_t right_bytes = right_line.significant_token_bytes;
      const std::size_t total_bytes = left_bytes + right_bytes;
      if (similarity_gates_pairing && total_bytes > 0) {
        const double ceiling = (2.0 * static_cast<double>(std::min(left_bytes, right_bytes))) /
                               static_cast<double>(total_bytes);
        if (ceiling < 0.35) {
          val = ceiling;
          return val;
        }
      }
      val = LineSimilarity(left_line, right_line, common_token_scratch);
    }
    return val;
  };

  std::vector<double>& dp = scratch.dp;
  std::vector<HunkAlignmentKind>& choice = scratch.choice;
  dp.assign((left_count + 1) * (right_count + 1), 0.0);
  choice.assign((left_count + 1) * (right_count + 1), HunkAlignmentKind::Pair);
  auto at = [&](std::size_t i, std::size_t j) -> double& {
    return dp[i * (right_count + 1) + j];
  };
  auto choice_at = [&](std::size_t i, std::size_t j) -> HunkAlignmentKind& {
    return choice[i * (right_count + 1) + j];
  };

  for (std::size_t i = left_count + 1; i-- > 0;) {
    if (i < left_count) {
      at(i, right_count) = static_cast<double>(left_count - i);
      choice_at(i, right_count) = HunkAlignmentKind::Delete;
    }
  }
  for (std::size_t j = right_count + 1; j-- > 0;) {
    if (j < right_count) {
      at(left_count, j) = static_cast<double>(right_count - j);
      choice_at(left_count, j) = HunkAlignmentKind::Insert;
    }
  }

  for (std::size_t i = left_count; i-- > 0;) {
    for (std::size_t j = right_count; j-- > 0;) {
      double best_cost = std::numeric_limits<double>::infinity();
      HunkAlignmentKind best_kind = HunkAlignmentKind::Delete;
      double best_similarity = -1.0;
      const auto consider = [&](double candidate_cost,
                                HunkAlignmentKind candidate_kind,
                                double candidate_similarity = -1.0) {
        constexpr double kEpsilon = 1e-6;
        if (candidate_cost + kEpsilon < best_cost) {
          best_cost = candidate_cost;
          best_kind = candidate_kind;
          best_similarity = candidate_similarity;
          return;
        }
        if (std::fabs(candidate_cost - best_cost) > kEpsilon) {
          return;
        }
        if (candidate_kind == HunkAlignmentKind::Pair &&
            (best_kind != HunkAlignmentKind::Pair ||
             candidate_similarity > best_similarity + kEpsilon)) {
          best_kind = candidate_kind;
          best_similarity = candidate_similarity;
        }
      };

      const double pair_similarity = pairing_possible ? similarity_at(i, j) : 0.0;
      if (pairing_possible && CanPairAlignedLines(pair_similarity, left_count, right_count)) {
        consider(AlignmentPairCost(pair_similarity) + at(i + 1, j + 1), HunkAlignmentKind::Pair,
                 pair_similarity);
      }
      consider(1.0 + at(i + 1, j), HunkAlignmentKind::Delete);
      consider(1.0 + at(i, j + 1), HunkAlignmentKind::Insert);

      at(i, j) = best_cost;
      choice_at(i, j) = best_kind;
    }
  }

  alignment.reserve(left_count + right_count);
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_count || j < right_count) {
    if (i >= left_count) {
      alignment.push_back(HunkAlignmentKind::Insert);
      ++j;
      continue;
    }
    if (j >= right_count) {
      alignment.push_back(HunkAlignmentKind::Delete);
      ++i;
      continue;
    }

    const HunkAlignmentKind kind = choice_at(i, j);
    alignment.push_back(kind);
    switch (kind) {
      case HunkAlignmentKind::Pair:
        ++i;
        ++j;
        break;
      case HunkAlignmentKind::Delete:
        ++i;
        break;
      case HunkAlignmentKind::Insert:
        ++j;
        break;
    }
  }
}

bool Utf8CodepointEquals(std::string_view left,
                         const std::vector<std::size_t>& left_offsets,
                         std::size_t left_index,
                         std::string_view right,
                         const std::vector<std::size_t>& right_offsets,
                         std::size_t right_index) {
  const std::size_t left_start = left_offsets[left_index];
  const std::size_t left_end = left_offsets[left_index + 1];
  const std::size_t right_start = right_offsets[right_index];
  const std::size_t right_end = right_offsets[right_index + 1];
  const std::size_t left_length = left_end - left_start;
  return left_length == (right_end - right_start) &&
         left.substr(left_start, left_length) == right.substr(right_start, left_length);
}

void BuildSpansFromChangedCodepointsInto(const std::vector<std::size_t>& offsets,
                                         const std::vector<char>& changed,
                                         std::vector<CompareTextSpan>& spans) {
  spans.clear();
  if (offsets.size() < 2 || changed.empty()) {
    return;
  }

  std::size_t index = 0;
  while (index < changed.size()) {
    if (!changed[index]) {
      ++index;
      continue;
    }
    const std::size_t start = index;
    while (index < changed.size() && changed[index]) {
      ++index;
    }
    spans.push_back(CompareTextSpan{
        .start = offsets[start],
        .end = offsets[index],
    });
  }
}

void BuildSpansFromChangedTokensInto(const TokenizedLine& line,
                                     const std::vector<char>& changed,
                                     std::vector<CompareTextSpan>& spans) {
  spans.clear();
  if (line.tokens.empty() || changed.empty()) {
    return;
  }

  std::size_t index = 0;
  while (index < changed.size()) {
    if (!changed[index]) {
      ++index;
      continue;
    }
    const std::size_t start = index;
    while (index < changed.size() && changed[index]) {
      ++index;
    }
    spans.push_back(CompareTextSpan{
        .start = line.tokens[start].start,
        .end = line.tokens[index - 1].end,
    });
  }
}

// Clips in place: the trimmed set is a subsequence of the input (each surviving
// span keeps or shrinks its bounds, never splits), so a single forward compaction
// pass replaces the allocate-a-new-vector-per-row shape.
void TrimSpansToByteWindowInPlace(std::vector<CompareTextSpan>& spans, std::size_t start,
                                  std::size_t end) {
  std::size_t out = 0;
  for (const CompareTextSpan& span : spans) {
    if (span.end <= start || span.start >= end) {
      continue;
    }
    const std::size_t clipped_start = std::max(span.start, start);
    const std::size_t clipped_end = std::min(span.end, end);
    if (clipped_end > clipped_start) {
      spans[out++] = CompareTextSpan{clipped_start, clipped_end};
    }
  }
  spans.resize(out);
}

void TrimChangedSpansToSharedEdges(CompareRow& row) {
  if (row.kind != CompareRowKind::Modified) {
    return;
  }

  IntralineScratch& scratch = Scratch();
  std::vector<std::size_t>& left_offsets = scratch.left_offsets;
  std::vector<std::size_t>& right_offsets = scratch.right_offsets;
  BuildUtf8OffsetsInto(row.left_text, left_offsets);
  BuildUtf8OffsetsInto(row.right_text, right_offsets);
  const std::size_t left_count = left_offsets.size() - 1;
  const std::size_t right_count = right_offsets.size() - 1;

  std::size_t prefix = 0;
  while (prefix < left_count && prefix < right_count &&
         Utf8CodepointEquals(row.left_text, left_offsets, prefix, row.right_text, right_offsets,
                             prefix)) {
    ++prefix;
  }

  std::size_t left_suffix = left_count;
  std::size_t right_suffix = right_count;
  while (left_suffix > prefix && right_suffix > prefix &&
         Utf8CodepointEquals(row.left_text, left_offsets, left_suffix - 1, row.right_text,
                             right_offsets, right_suffix - 1)) {
    --left_suffix;
    --right_suffix;
  }

  TrimSpansToByteWindowInPlace(row.left_changed_spans, left_offsets[prefix],
                               left_offsets[left_suffix]);
  TrimSpansToByteWindowInPlace(row.right_changed_spans, right_offsets[prefix],
                               right_offsets[right_suffix]);
}

bool PopulateTokenChangedSpans(CompareRow& row) {
  IntralineScratch& scratch = Scratch();
  TokenizedLine& left = scratch.left_tokens;
  TokenizedLine& right = scratch.right_tokens;
  TokenizeLineInto(row.left_text, left);
  TokenizeLineInto(row.right_text, right);
  if (left.tokens.size() < 2 || right.tokens.size() < 2 ||
      ProductExceeds(left.tokens.size() + 1, right.tokens.size() + 1, kMaxIntralineLcsMatrixCells)) {
    return false;
  }

  std::vector<char>& left_changed = scratch.left_changed;
  std::vector<char>& right_changed = scratch.right_changed;
  left_changed.assign(left.tokens.size(), 1);
  right_changed.assign(right.tokens.size(), 1);

  std::size_t prefix = 0;
  while (prefix < left.tokens.size() && prefix < right.tokens.size() &&
         TokenEquals(left.text, left.tokens[prefix], right.text, right.tokens[prefix])) {
    left_changed[prefix] = 0;
    right_changed[prefix] = 0;
    ++prefix;
  }

  std::size_t left_suffix = left.tokens.size();
  std::size_t right_suffix = right.tokens.size();
  while (left_suffix > prefix && right_suffix > prefix &&
         TokenEquals(left.text, left.tokens[left_suffix - 1], right.text,
                     right.tokens[right_suffix - 1])) {
    --left_suffix;
    --right_suffix;
    left_changed[left_suffix] = 0;
    right_changed[right_suffix] = 0;
  }

  const std::size_t left_middle_count = left_suffix - prefix;
  const std::size_t right_middle_count = right_suffix - prefix;
  if (left_middle_count == 0 && right_middle_count == 0) {
    row.left_changed_spans.clear();
    row.right_changed_spans.clear();
    return true;
  }
  if (left_middle_count == 0 || right_middle_count == 0) {
    BuildSpansFromChangedTokensInto(left, left_changed, row.left_changed_spans);
    BuildSpansFromChangedTokensInto(right, right_changed, row.right_changed_spans);
    return true;
  }

  std::vector<std::size_t>& dp = scratch.token_dp;
  dp.assign((left_middle_count + 1) * (right_middle_count + 1), 0);
  auto at = [&](std::size_t i, std::size_t j) -> std::size_t& {
    return dp[i * (right_middle_count + 1) + j];
  };

  for (std::size_t i = left_middle_count; i-- > 0;) {
    for (std::size_t j = right_middle_count; j-- > 0;) {
      std::size_t best = std::max(at(i + 1, j), at(i, j + 1));
      if (TokenEquals(left.text, left.tokens[prefix + i], right.text, right.tokens[prefix + j])) {
        best =
            std::max(best, TokenMatchWeight(left.tokens[prefix + i]) + at(i + 1, j + 1));
      }
      at(i, j) = best;
    }
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_middle_count && j < right_middle_count) {
    const std::size_t left_index = prefix + i;
    const std::size_t right_index = prefix + j;
    const std::size_t skip_left = at(i + 1, j);
    const std::size_t skip_right = at(i, j + 1);
    if (TokenEquals(left.text, left.tokens[left_index], right.text, right.tokens[right_index])) {
      const std::size_t diagonal =
          TokenMatchWeight(left.tokens[left_index]) + at(i + 1, j + 1);
      if (diagonal >= skip_left && diagonal >= skip_right && at(i, j) == diagonal) {
        left_changed[left_index] = 0;
        right_changed[right_index] = 0;
        ++i;
        ++j;
        continue;
      }
    }

    if (skip_left >= skip_right) {
      ++i;
    } else {
      ++j;
    }
  }

  BuildSpansFromChangedTokensInto(left, left_changed, row.left_changed_spans);
  BuildSpansFromChangedTokensInto(right, right_changed, row.right_changed_spans);
  return !(row.left_changed_spans.empty() && row.right_changed_spans.empty());
}

void PopulateCodepointChangedSpans(CompareRow& row) {
  row.left_changed_spans.clear();
  row.right_changed_spans.clear();

  IntralineScratch& scratch = Scratch();
  std::vector<std::size_t>& left_offsets = scratch.left_offsets;
  std::vector<std::size_t>& right_offsets = scratch.right_offsets;
  BuildUtf8OffsetsInto(row.left_text, left_offsets);
  BuildUtf8OffsetsInto(row.right_text, right_offsets);
  const std::size_t left_count = left_offsets.size() - 1;
  const std::size_t right_count = right_offsets.size() - 1;
  std::vector<char>& left_changed = scratch.left_changed;
  std::vector<char>& right_changed = scratch.right_changed;
  left_changed.assign(left_count, 1);
  right_changed.assign(right_count, 1);

  std::size_t prefix = 0;
  while (prefix < left_count && prefix < right_count &&
         Utf8CodepointEquals(row.left_text, left_offsets, prefix, row.right_text, right_offsets,
                             prefix)) {
    left_changed[prefix] = 0;
    right_changed[prefix] = 0;
    ++prefix;
  }

  std::size_t left_suffix = left_count;
  std::size_t right_suffix = right_count;
  while (left_suffix > prefix && right_suffix > prefix &&
         Utf8CodepointEquals(row.left_text, left_offsets, left_suffix - 1, row.right_text,
                             right_offsets, right_suffix - 1)) {
    --left_suffix;
    --right_suffix;
    left_changed[left_suffix] = 0;
    right_changed[right_suffix] = 0;
  }

  const std::size_t left_middle_count = left_suffix - prefix;
  const std::size_t right_middle_count = right_suffix - prefix;
  if (left_middle_count > 0 && right_middle_count > 0 &&
      !ProductExceeds(left_middle_count + 1, right_middle_count + 1,
                      kMaxIntralineLcsMatrixCells)) {
    std::vector<int>& dp = scratch.codepoint_dp;
    dp.assign((left_middle_count + 1) * (right_middle_count + 1), 0);
    auto at = [&](std::size_t i, std::size_t j) -> int& {
      return dp[i * (right_middle_count + 1) + j];
    };

    for (std::size_t i = left_middle_count; i-- > 0;) {
      for (std::size_t j = right_middle_count; j-- > 0;) {
        if (Utf8CodepointEquals(row.left_text, left_offsets, prefix + i, row.right_text,
                                right_offsets, prefix + j)) {
          at(i, j) = at(i + 1, j + 1) + 1;
        } else {
          at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
        }
      }
    }

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left_middle_count && j < right_middle_count) {
      if (Utf8CodepointEquals(row.left_text, left_offsets, prefix + i, row.right_text,
                              right_offsets, prefix + j)) {
        left_changed[prefix + i] = 0;
        right_changed[prefix + j] = 0;
        ++i;
        ++j;
      } else if (at(i + 1, j) >= at(i, j + 1)) {
        ++i;
      } else {
        ++j;
      }
    }
  }

  BuildSpansFromChangedCodepointsInto(left_offsets, left_changed, row.left_changed_spans);
  BuildSpansFromChangedCodepointsInto(right_offsets, right_changed, row.right_changed_spans);
}

void PopulateChangedSpans(CompareRow& row, CompareBuildProfile* profile,
                          std::size_t* intraline_cells_budget) {
  row.left_changed_spans.clear();
  row.right_changed_spans.clear();

  if (row.kind == CompareRowKind::Unchanged) {
    return;
  }

  if (row.kind == CompareRowKind::Deleted) {
    if (!row.left_text.empty()) {
      row.left_changed_spans.push_back(CompareTextSpan{0, row.left_text.size()});
    }
    return;
  }

  if (row.kind == CompareRowKind::Added) {
    if (!row.right_text.empty()) {
      row.right_changed_spans.push_back(CompareTextSpan{0, row.right_text.size()});
    }
    return;
  }

  // Guard: a very long line on either side would make the per-codepoint/token
  // intra-line diff allocate proportional scratch and stall the UI thread. Fall
  // back to marking the entire line changed on both sides.
  if (row.left_text.size() > kMaxIntralineTextBytes ||
      row.right_text.size() > kMaxIntralineTextBytes) {
    if (!row.left_text.empty()) {
      row.left_changed_spans.push_back(CompareTextSpan{0, row.left_text.size()});
    }
    if (!row.right_text.empty()) {
      row.right_changed_spans.push_back(CompareTextSpan{0, row.right_text.size()});
    }
    return;
  }

  // Cumulative per-hunk intra-line budget: once spent, stop character-level
  // refinement and mark the whole (modified) line changed on both sides, bounding
  // total DP work for a pathological many-Modified-row hunk.
  if (intraline_cells_budget != nullptr) {
    if (*intraline_cells_budget == 0) {
      if (!row.left_text.empty()) {
        row.left_changed_spans.push_back(CompareTextSpan{0, row.left_text.size()});
      }
      if (!row.right_text.empty()) {
        row.right_changed_spans.push_back(CompareTextSpan{0, row.right_text.size()});
      }
      return;
    }
    // Charge this pair its worst-case DP size (capped at the per-pair limit). The
    // line-byte guard above bounds the operands, so the product cannot overflow.
    const std::size_t left_dim = row.left_text.size() + 1;
    const std::size_t right_dim = row.right_text.size() + 1;
    const std::size_t pair_cost =
        ProductExceeds(left_dim, right_dim, kMaxIntralineLcsMatrixCells)
            ? kMaxIntralineLcsMatrixCells
            : left_dim * right_dim;
    *intraline_cells_budget =
        *intraline_cells_budget > pair_cost ? *intraline_cells_budget - pair_cost : 0;
  }

  if (!PopulateTokenChangedSpans(row)) {
    if (profile != nullptr) {
      ++profile->codepoint_intraline_calls;
    }
    PopulateCodepointChangedSpans(row);
  } else if (profile != nullptr) {
    ++profile->token_intraline_calls;
  }
  TrimChangedSpansToSharedEdges(row);
}

void AppendDeleteOps(std::span<const std::string_view> lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Delete, lines[i]});
  }
}

void AppendInsertOps(std::span<const std::string_view> lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Insert, lines[i]});
  }
}

// Defined further down in this file; declared here so the line-op builders can
// honour ignore_whitespace inside changed hunks, not just on the matched edges.
bool LinesEqualForDiff(std::string_view left,
                       std::string_view right,
                       const CompareBuildOptions& options);
bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right);

}  // namespace

// These four types are members of `detail::CompareBuildScratch`, which the
// header declares — so they cannot live in the anonymous namespace, or the
// scratch would be an external-linkage class with internal-linkage fields
// (-Wsubobject-linkage). Everything that USES them stays file-local.
namespace detail {

// Whitespace-insensitive line identity, so the ignore_whitespace interning pass
// can key on the original view instead of a normalised copy of it. The hash
// walks the same bytes the equality does — everything that is not ASCII space —
// which is what makes it agree with LinesEqualIgnoringWhitespace.
struct IgnoreWhitespaceLineHash {
  std::size_t operator()(std::string_view line) const {
    std::uint64_t hash = util::kFnv1aOffsetBasis;
    for (const char c : line) {
      if (util::IsAsciiSpace(static_cast<unsigned char>(c)) != 0) {
        continue;
      }
      hash = util::Fnv1aByte(hash, static_cast<unsigned char>(c));
    }
    return static_cast<std::size_t>(hash);
  }
};

struct IgnoreWhitespaceLineEq {
  bool operator()(std::string_view left, std::string_view right) const {
    return LinesEqualIgnoringWhitespace(left, right);
  }
};

// Per-line info for the unique-line anchor search, kept out of the function so
// DiffScratch can hold the vector it indexes.
struct AnchorInfo {
  std::size_t left_count = 0;
  std::size_t right_count = 0;
  std::size_t left_index = 0;
  std::size_t right_index = 0;
};

// Working buffers for one whole diff, threaded through the recursion.
//
// The anchored fallback recurses once per anchor segment, and each level used to
// build its own working set from scratch: the exact-LCS aligner allocated six
// vectors plus an intern table, the anchor search seven more. On the 12,000-line
// worktree diff the compare scenarios use — thirty hunks, so thirty-odd levels —
// that is a few hundred allocations per rebuild, and an editable compare pane
// rebuilds on EVERY keystroke.
//
// Every buffer here is dead by the time the function that filled it returns, so
// one instance is safe for the whole recursion even though levels nest. The one
// buffer that is *not* — the anchor list, which stays live while the level below
// it runs — is deliberately absent and stays a local at each level.
struct DiffScratch {
  // Exact-LCS aligner.
  std::vector<std::uint32_t> left_ids;
  std::vector<std::uint32_t> right_ids;
  std::vector<std::uint32_t> class_occurrences;
  std::vector<std::size_t> left_match_weight;
  std::vector<std::size_t> dp;
  util::FlatDedupSet<std::string_view> line_ids{0};
  util::FlatDedupSet<std::string_view, IgnoreWhitespaceLineHash, IgnoreWhitespaceLineEq>
      whitespace_insensitive_line_ids{0};
  // Anchor search.
  std::vector<AnchorInfo> info_by_id;
  std::vector<std::pair<std::size_t, std::size_t>> candidates;
  std::vector<std::size_t> pile_tops;
  std::vector<std::size_t> pile_candidate_indices;
  std::vector<std::size_t> predecessor;
};

// The buffers one whole rebuild needs, retained between rebuilds. See the
// declaration in `CompareModel.h` for why they live on the model.
struct CompareBuildScratch {
  std::vector<std::string_view> left_lines;
  std::vector<std::string_view> right_lines;
  std::vector<DiffOp> ops;
  DiffScratch diff;
};

}  // namespace detail

namespace {

using detail::AnchorInfo;
using detail::DiffScratch;

// Emit a run of Equal ops carrying both columns' text for the lockstep-matched
// pair range [left_begin, left_end) <-> [right_begin, ...). The two ranges have
// equal length by construction at every caller.
void AppendEqualPairs(std::span<const std::string_view> left_lines,
                      std::size_t left_begin,
                      std::size_t left_end,
                      std::span<const std::string_view> right_lines,
                      std::size_t right_begin,
                      std::vector<DiffOp>& ops) {
  for (std::size_t offset = 0; left_begin + offset < left_end; ++offset) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[left_begin + offset],
                         right_lines[right_begin + offset]});
  }
}

void AppendExactLineOps(std::span<const std::string_view> left_lines,
                        std::span<const std::string_view> right_lines,
                        const CompareBuildOptions& options,
                        DiffScratch& scratch,
                        std::vector<DiffOp>& ops) {
  const std::size_t left_count = left_lines.size();
  const std::size_t right_count = right_lines.size();
  if (left_count == 0 && right_count == 0) {
    return;
  }

  // Intern each line to an equality-class id before the DP. The table below is
  // bounded at kMaxLineLcsMatrixCells (250k) cells, and the loop used to call
  // LinesEqualForDiff at EVERY one of them — a full memcmp over the line, or
  // under ignore_whitespace a two-cursor whitespace-skipping walk. Hashing each
  // line once (O(N + M)) turns all of that into an integer compare. The ids
  // reproduce LinesEqualForDiff exactly because it is a genuine equivalence
  // relation either way: exact byte equality, or equality after removing every
  // whitespace byte (`==` implies the whitespace-insensitive form, so the
  // ignore_whitespace case is not a union of two relations).
  //
  // Interning is the ONLY hash structure this function builds now
  // (TD-2026-08-07-164). It used to build three: a `line_occurrences` map for
  // the rarity weight, plus one of two id maps — and under ignore_whitespace the
  // id map keyed on an owned, whitespace-stripped `std::string` per distinct
  // line, purely so `unordered_map` had something to hash. All three were a heap
  // node per distinct line, so a 14k-line compare paid ~28k node allocations
  // before the DP started. `FlatDedupSet` hands out ids that are dense indices
  // into its own key vector, so the occurrence counts are a plain vector indexed
  // by id, and a whitespace-insensitive Hash/Eq pair lets the ignore_whitespace
  // path key on the original view with no normalised copy at all.
  //
  // The counts are per equality CLASS, which under ignore_whitespace groups
  // lines that differ only in whitespace — the same relation the DP matches on,
  // so the rarity weight and the match it weighs now agree.
  std::vector<std::uint32_t>& left_ids = scratch.left_ids;
  std::vector<std::uint32_t>& right_ids = scratch.right_ids;
  std::vector<std::uint32_t>& class_occurrences = scratch.class_occurrences;
  left_ids.assign(left_count, 0);
  right_ids.assign(right_count, 0);
  class_occurrences.clear();
  class_occurrences.reserve(left_count + right_count);
  const auto assign_ids = [&](auto& ids) {
    ids.Reset(left_count + right_count);
    const auto intern = [&](std::string_view line) {
      const std::size_t id = ids.Intern(line);
      if (id == class_occurrences.size()) {
        class_occurrences.push_back(1);
      } else {
        ++class_occurrences[id];
      }
      return static_cast<std::uint32_t>(id);
    };
    for (std::size_t i = 0; i < left_count; ++i) left_ids[i] = intern(left_lines[i]);
    for (std::size_t j = 0; j < right_count; ++j) right_ids[j] = intern(right_lines[j]);
  };
  if (options.ignore_whitespace) {
    assign_ids(scratch.whitespace_insensitive_line_ids);
  } else {
    assign_ids(scratch.line_ids);
  }

  std::vector<std::size_t>& left_match_weight = scratch.left_match_weight;
  left_match_weight.assign(left_count, 1);
  for (std::size_t i = 0; i < left_count; ++i) {
    left_match_weight[i] = LineMatchWeight(left_lines[i], SignificantTokenBytes(left_lines[i]),
                                           class_occurrences[left_ids[i]]);
  }

  const std::size_t stride = right_count + 1;
  std::vector<std::size_t>& dp = scratch.dp;
  dp.assign((left_count + 1) * stride, 0);
  auto at = [&](std::size_t i, std::size_t j) -> std::size_t& { return dp[i * stride + j]; };

  // Row-pointer walk: `at()` recomputed `i * stride + j` for each of the four
  // accesses per cell. Hoisting the two rows out of the inner loop leaves one
  // add per access.
  for (std::size_t i = left_count; i-- > 0;) {
    std::size_t* row = dp.data() + i * stride;
    const std::size_t* next_row = row + stride;
    const std::uint32_t left_id = left_ids[i];
    const std::size_t weight = left_match_weight[i];
    for (std::size_t j = right_count; j-- > 0;) {
      std::size_t best = std::max(next_row[j], row[j + 1]);
      if (left_id == right_ids[j]) {
        const std::size_t diagonal = weight + next_row[j + 1];
        if (diagonal > best) {
          best = diagonal;
        }
      }
      row[j] = best;
    }
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_count && j < right_count) {
    if (left_ids[i] == right_ids[j]) {
      const std::size_t diagonal = left_match_weight[i] + at(i + 1, j + 1);
      if (diagonal >= at(i + 1, j) && diagonal >= at(i, j + 1) && at(i, j) == diagonal) {
        ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i], right_lines[j]});
        ++i;
        ++j;
        continue;
      }
    }

    if (at(i + 1, j) >= at(i, j + 1)) {
      ops.push_back(DiffOp{DiffOpKind::Delete, left_lines[i]});
      ++i;
    } else {
      ops.push_back(DiffOp{DiffOpKind::Insert, right_lines[j]});
      ++j;
    }
  }
  AppendDeleteOps(left_lines, i, left_count, ops);
  AppendInsertOps(right_lines, j, right_count, ops);
}

// Anchors are written into `anchors` rather than returned, because the caller's
// list stays live across the recursive calls it drives and so cannot come from
// the shared scratch. Everything else this needs does.
void BuildUniqueLineAnchors(std::span<const std::string_view> left_lines,
                            std::size_t left_begin,
                            std::size_t left_end,
                            std::span<const std::string_view> right_lines,
                            std::size_t right_begin,
                            std::size_t right_end,
                            DiffScratch& scratch,
                            std::vector<std::pair<std::size_t, std::size_t>>& anchors) {
  anchors.clear();

  // Same shape as AppendExactLineOps' interning, and the same reason: this ran on
  // the anchored fallback path — the one large diffs take — and cost a heap node
  // per distinct line before any anchor was found (TD-2026-08-07-164). Ids from
  // `Intern` are dense indices, so the per-line info is a parallel vector.
  const std::size_t line_count = (left_end - left_begin) + (right_end - right_begin);
  util::FlatDedupSet<std::string_view>& line_ids = scratch.line_ids;
  line_ids.Reset(line_count);
  std::vector<AnchorInfo>& info_by_id = scratch.info_by_id;
  info_by_id.clear();
  info_by_id.reserve(line_count);
  const auto info_for = [&](std::string_view line) -> AnchorInfo& {
    const std::size_t id = line_ids.Intern(line);
    if (id == info_by_id.size()) {
      info_by_id.emplace_back();
    }
    return info_by_id[id];
  };
  for (std::size_t i = left_begin; i < left_end; ++i) {
    AnchorInfo& info = info_for(left_lines[i]);
    ++info.left_count;
    info.left_index = i;
  }
  for (std::size_t i = right_begin; i < right_end; ++i) {
    AnchorInfo& info = info_for(right_lines[i]);
    ++info.right_count;
    info.right_index = i;
  }

  std::vector<std::pair<std::size_t, std::size_t>>& candidates = scratch.candidates;
  candidates.clear();
  candidates.reserve(std::min(left_end - left_begin, right_end - right_begin));
  for (std::size_t id = 0; id < info_by_id.size(); ++id) {
    const AnchorInfo& info = info_by_id[id];
    if (line_ids.keys()[id].empty()) {
      continue;
    }
    if (info.left_count == 1 && info.right_count == 1) {
      candidates.emplace_back(info.left_index, info.right_index);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  if (candidates.empty()) {
    return;
  }

  const std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t>& pile_tops = scratch.pile_tops;
  std::vector<std::size_t>& pile_candidate_indices = scratch.pile_candidate_indices;
  std::vector<std::size_t>& predecessor = scratch.predecessor;
  pile_tops.clear();
  pile_candidate_indices.clear();
  predecessor.assign(candidates.size(), kNoIndex);
  for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
    const std::size_t right_index = candidates[candidate_index].second;
    const auto it = std::lower_bound(pile_tops.begin(), pile_tops.end(), right_index);
    const std::size_t pile_index = static_cast<std::size_t>(it - pile_tops.begin());
    if (pile_index > 0) {
      predecessor[candidate_index] = pile_candidate_indices[pile_index - 1];
    }
    if (it == pile_tops.end()) {
      pile_tops.push_back(right_index);
      pile_candidate_indices.push_back(candidate_index);
    } else {
      *it = right_index;
      pile_candidate_indices[pile_index] = candidate_index;
    }
  }

  if (pile_candidate_indices.empty()) {
    return;
  }
  for (std::size_t index = pile_candidate_indices.back(); index != kNoIndex;
       index = predecessor[index]) {
    anchors.push_back(candidates[index]);
  }
  std::reverse(anchors.begin(), anchors.end());
}

void AppendAnchoredFallbackOps(std::span<const std::string_view> left_lines,
                               std::size_t left_begin,
                               std::size_t left_end,
                               std::span<const std::string_view> right_lines,
                               std::size_t right_begin,
                               std::size_t right_end,
                               const CompareBuildOptions& options,
                               DiffScratch& scratch,
                               std::vector<DiffOp>& ops,
                               std::size_t depth) {
  while (left_begin < left_end && right_begin < right_end &&
         LinesEqualForDiff(left_lines[left_begin], right_lines[right_begin], options)) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[left_begin], right_lines[right_begin]});
    ++left_begin;
    ++right_begin;
  }

  std::size_t left_suffix = left_end;
  std::size_t right_suffix = right_end;
  while (left_suffix > left_begin && right_suffix > right_begin &&
         LinesEqualForDiff(left_lines[left_suffix - 1], right_lines[right_suffix - 1], options)) {
    --left_suffix;
    --right_suffix;
  }
  // The matched suffix [left_suffix, left_end) <-> [right_suffix, right_end) is
  // emitted (with both columns) by every return branch below.

  if (left_begin == left_suffix && right_begin == right_suffix) {
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }
  if (left_begin == left_suffix) {
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }
  if (right_begin == right_suffix) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  const std::size_t middle_left_count = left_suffix - left_begin;
  const std::size_t middle_right_count = right_suffix - right_begin;
  if (!ProductExceeds(middle_left_count + 1, middle_right_count + 1, kMaxLineLcsMatrixCells)) {
    const std::span<const std::string_view> left_slice =
        left_lines.subspan(left_begin, left_suffix - left_begin);
    const std::span<const std::string_view> right_slice =
        right_lines.subspan(right_begin, right_suffix - right_begin);
    AppendExactLineOps(left_slice, right_slice, options, scratch, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  // Depth guard: in the worst case (e.g. N unique lines present in reversed order
  // on the two sides) the anchor recursion peels a single line per level, so an
  // adversarial merge/compare input from an untrusted repo could recurse ~N deep
  // -> stack overflow, plus O(N^2) CPU rebuilding the anchor map at each level.
  // Past the cap, emit the remaining middle as a coarse delete+insert (a correct,
  // just less-minimal, diff) instead of recursing further.
  constexpr std::size_t kMaxAnchoredFallbackDepth = 256;
  if (depth >= kMaxAnchoredFallbackDepth) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  // Local, not scratch: this list stays live while the recursive calls below run,
  // and those levels reuse every buffer in the scratch.
  std::vector<std::pair<std::size_t, std::size_t>> anchors;
  BuildUniqueLineAnchors(left_lines, left_begin, left_suffix, right_lines, right_begin,
                         right_suffix, scratch, anchors);
  if (anchors.empty()) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  std::size_t segment_left_begin = left_begin;
  std::size_t segment_right_begin = right_begin;
  for (const auto& [anchor_left, anchor_right] : anchors) {
    AppendAnchoredFallbackOps(left_lines, segment_left_begin, anchor_left, right_lines,
                              segment_right_begin, anchor_right, options, scratch, ops, depth + 1);
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[anchor_left], right_lines[anchor_right]});
    segment_left_begin = anchor_left + 1;
    segment_right_begin = anchor_right + 1;
  }
  AppendAnchoredFallbackOps(left_lines, segment_left_begin, left_suffix, right_lines,
                            segment_right_begin, right_suffix, options, scratch, ops, depth + 1);
  AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
}

}  // namespace

namespace {

bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right) {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() || right_index < right.size()) {
    while (left_index < left.size() && util::IsAsciiSpace(static_cast<unsigned char>(left[left_index])) != 0) {
      ++left_index;
    }
    while (right_index < right.size() &&
           util::IsAsciiSpace(static_cast<unsigned char>(right[right_index])) != 0) {
      ++right_index;
    }
    if (left_index >= left.size() || right_index >= right.size()) {
      return left_index >= left.size() && right_index >= right.size();
    }
    if (left[left_index] != right[right_index]) {
      return false;
    }
    ++left_index;
    ++right_index;
  }
  return true;
}

bool LinesEqualForDiff(std::string_view left,
                       std::string_view right,
                       const CompareBuildOptions& options) {
  if (left == right) {
    return true;
  }
  return options.ignore_whitespace && LinesEqualIgnoringWhitespace(left, right);
}

}  // namespace

const CompareTextBuffer& EmptyCompareText() {
  // One object process-wide, so "no text" costs a refcount bump rather than an
  // allocation — a default-constructed CompareModel or compare tab hits this.
  static const CompareTextBuffer empty = std::make_shared<const std::string>();
  return empty;
}

CompareTextBuffer MakeCompareText(std::string text) {
  if (text.empty()) {
    return EmptyCompareText();
  }
  return std::make_shared<const std::string>(std::move(text));
}

std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     LineDiffBuildStats* stats) {
  return BuildLineDiffOps(left_lines, right_lines, CompareBuildOptions{}, stats);
}

std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     const CompareBuildOptions& options,
                                     LineDiffBuildStats* stats) {
  std::vector<DiffOp> ops;
  detail::DiffScratch scratch;
  BuildLineDiffOpsInto(left_lines, right_lines, options, scratch, ops, stats);
  return ops;
}

void BuildLineDiffOpsInto(std::span<const std::string_view> left_lines,
                          std::span<const std::string_view> right_lines,
                          const CompareBuildOptions& options,
                          detail::DiffScratch& scratch,
                          std::vector<DiffOp>& ops,
                          LineDiffBuildStats* stats) {
  std::size_t prefix = 0;
  while (prefix < left_lines.size() && prefix < right_lines.size() &&
         LinesEqualForDiff(left_lines[prefix], right_lines[prefix], options)) {
    ++prefix;
  }

  std::size_t left_suffix = left_lines.size();
  std::size_t right_suffix = right_lines.size();
  while (left_suffix > prefix && right_suffix > prefix &&
         LinesEqualForDiff(left_lines[left_suffix - 1], right_lines[right_suffix - 1], options)) {
    --left_suffix;
    --right_suffix;
  }

  ops.clear();
  ops.reserve(left_lines.size() + right_lines.size());
  for (std::size_t index = 0; index < prefix; ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index], right_lines[index]});
  }

  const std::span<const std::string_view> left_middle =
      left_lines.subspan(prefix, left_suffix - prefix);
  const std::span<const std::string_view> right_middle =
      right_lines.subspan(prefix, right_suffix - prefix);
  if (!left_middle.empty() || !right_middle.empty()) {
    const std::size_t left_count = left_middle.size();
    const std::size_t right_count = right_middle.size();
    // One scratch for the whole diff, supplied by the caller so it also survives
    // BETWEEN diffs. Both aligners used to build (and free) a whole `middle_ops`
    // vector here purely to copy it into `ops`, on top of the per-level buffers
    // inside them.
    if (ProductExceeds(left_count + 1, right_count + 1, kMaxLineLcsMatrixCells)) {
      if (stats != nullptr) {
        ++stats->anchored_alignment_calls;
      }
      AppendAnchoredFallbackOps(left_middle, 0, left_middle.size(), right_middle, 0,
                                right_middle.size(), options, scratch, ops, 0);
    } else {
      if (stats != nullptr) {
        ++stats->exact_alignment_calls;
      }
      AppendExactLineOps(left_middle, right_middle, options, scratch, ops);
    }
  }

  // The matched suffix [left_suffix, end) <-> [right_suffix, end) has equal
  // length on both sides, so the right counterpart for left index `index` sits
  // at the same offset past right_suffix.
  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index],
                         right_lines[index - left_suffix + right_suffix]});
  }
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right, CompareBuildOptions{});
}

namespace {

// Row storage recycler.
//
// A compare rebuild used to construct every row from scratch, and `CompareRow`
// then owned TWO std::strings — so a 12,000-row diff cost 24,000 string
// allocations per rebuild, and the rebuild runs on every keystroke in the editable
// pane. That was 98 % of the allocations in `compare_type_in_wrapped_diff`
// (TD-2026-08-13-208).
//
// The rows of consecutive rebuilds are nearly identical, so this hands back the
// PREVIOUS build's row objects. Since TD-2026-08-14-232 a row's text is a view
// into the model's own source buffers rather than an owned string, so what is
// recycled is the row vector's storage and the two changed-span vectors' capacity.
// Rows past the new end are dropped at the end of the build; `resize` down never
// reallocates.
//
// It does NOT change what is built: every field is reset here, so a recycled row
// is indistinguishable from a fresh one.
class RowSink {
 public:
  explicit RowSink(CompareModel& model) : model_(model) {}

  CompareRow& Next() {
    if (next_ == model_.rows.size()) {
      model_.rows.emplace_back();
    }
    CompareRow& row = model_.rows[next_++];
    row.left_text = {};
    row.right_text = {};
    row.left_line = 0;
    row.right_line = 0;
    row.kind = CompareRowKind::Unchanged;
    row.hunk = -1;
    row.left_changed_spans.clear();
    row.right_changed_spans.clear();
    return row;
  }

  std::size_t size() const { return next_; }
  void Finish() { model_.rows.resize(next_); }

 private:
  CompareModel& model_;
  std::size_t next_ = 0;
};

}  // namespace

namespace detail {

void DestroyCompareBuildScratch(CompareBuildScratch* scratch) noexcept {
  delete scratch;
}

}  // namespace detail

namespace {

// Resolve the model's rebuild scratch, creating it on first use. One allocation
// per model, ever.
detail::CompareBuildScratch& EnsureBuildScratch(CompareModel& model) {
  if (model.build_scratch.get() == nullptr) {
    model.build_scratch.reset(new detail::CompareBuildScratch());
  }
  return *model.build_scratch.get();
}

}  // namespace

// Internal linkage: both callers are the two public entry points at the bottom of
// this file, and nothing declares it in the header. Left external it was the only
// -Wmissing-declarations hit in the tree, and it denied the optimizer the one
// thing worth having here -- this is the whole diff build, so whether it can be
// inlined into `BuildCompareModel` is not a rounding error.
static CompareBuildProfile BuildCompareModelProfiledInto(CompareModel& model,
                                                         CompareTextBuffer left_buffer,
                                                         CompareTextBuffer right_buffer,
                                                         const CompareBuildOptions& options) {
  CompareBuildProfile profile;
  // Drop the row views BEFORE the source buffers are replaced: every row views
  // into them, so a row surviving this would read bytes that no longer belong to
  // it. (The row STORAGE is still recycled by RowSink below, which is what keeps a
  // rebuild allocation-free — only the views are cleared here.)
  for (CompareRow& row : model.rows) {
    row.left_text = {};
    row.right_text = {};
  }
  model.hunks.clear();
  model.left_source = left_buffer != nullptr ? std::move(left_buffer) : EmptyCompareText();
  model.right_source = right_buffer != nullptr ? std::move(right_buffer) : EmptyCompareText();
  const std::string& left = *model.left_source;
  const std::string& right = *model.right_source;

  // A non-empty buffer that does not end in '\n' lacks a trailing newline; the
  // patch generator uses this to emit `\ No newline at end of file`.
  model.left_final_newline_missing = !left.empty() && left.back() != '\n';
  model.right_final_newline_missing = !right.empty() && right.back() != '\n';
  model.left_empty = left.empty();
  model.right_empty = right.empty();
  // Classify each side's line terminator from its first newline. git keeps the
  // `\r` of a CRLF file inside the blob, so the patch generator must re-emit it.
  const auto uses_crlf = [](const std::string& buffer) {
    const std::size_t newline = buffer.find('\n');
    return newline != std::string::npos && newline > 0 && buffer[newline - 1] == '\r';
  };
  model.left_uses_crlf = uses_crlf(left);
  model.right_uses_crlf = uses_crlf(right);

  const Clock::time_point total_start = Clock::now();

  const Clock::time_point split_start = Clock::now();
  // Views into `left`/`right`, which outlive this build. Everything between here
  // and row assembly stays views; the rows themselves view into the same buffers.
  //
  // Split into the model's retained scratch rather than two fresh vectors: this
  // was the single largest allocator on the keystroke path of an editable
  // compare pane (TD-2026-08-17-261).
  detail::CompareBuildScratch& scratch = EnsureBuildScratch(model);
  util::SplitLineViewsInto(left, scratch.left_lines);
  util::SplitLineViewsInto(right, scratch.right_lines);
  const std::vector<std::string_view>& left_lines = scratch.left_lines;
  const std::vector<std::string_view>& right_lines = scratch.right_lines;
  profile.split_lines_ns = DurationNs(split_start, Clock::now());

  // The all-equal case is handled by the general prefix/suffix path below: when
  // every line matches, `prefix` walks the whole document, `left_middle`/
  // `right_middle` are empty, and the prefix loop emits one Unchanged row per line
  // with left_line == right_line — byte-identical to a dedicated fast path. A prior
  // `lines_equal` pre-scan re-walked all lines via LinesEqualForDiff only to have
  // the prefix loop immediately re-scan the identical prefix; it was pure redundant
  // work per rebuild and has been removed.
  const Clock::time_point line_alignment_start = Clock::now();
  std::size_t prefix = 0;
  while (prefix < left_lines.size() && prefix < right_lines.size() &&
         LinesEqualForDiff(left_lines[prefix], right_lines[prefix], options)) {
    ++prefix;
  }
  std::size_t left_suffix = left_lines.size();
  std::size_t right_suffix = right_lines.size();
  while (left_suffix > prefix && right_suffix > prefix &&
         LinesEqualForDiff(left_lines[left_suffix - 1], right_lines[right_suffix - 1], options)) {
    --left_suffix;
    --right_suffix;
  }

  const std::span<const std::string_view> left_middle =
      std::span<const std::string_view>(left_lines).subspan(prefix, left_suffix - prefix);
  const std::span<const std::string_view> right_middle =
      std::span<const std::string_view>(right_lines).subspan(prefix, right_suffix - prefix);

  LineDiffBuildStats line_diff_stats;
  BuildLineDiffOpsInto(left_middle, right_middle, options, scratch.diff, scratch.ops,
                       &line_diff_stats);
  const std::vector<DiffOp>& ops = scratch.ops;
  profile.exact_line_alignment_calls += line_diff_stats.exact_alignment_calls;
  profile.anchored_line_alignment_calls += line_diff_stats.anchored_alignment_calls;
  profile.line_alignment_ns = DurationNs(line_alignment_start, Clock::now());

  model.rows.reserve(left_lines.size() + right_lines.size());
  RowSink sink(model);
  int left_line = 1;
  int right_line = 1;
  for (std::size_t index = 0; index < prefix; ++index) {
    CompareRow& row = sink.Next();
    row.left_text = left_lines[index];
    row.right_text = right_lines[index];
    row.left_line = left_line++;
    row.right_line = right_line++;
  }

  for (std::size_t op_index = 0; op_index < ops.size(); ++op_index) {
    const auto& op = ops[op_index];
    if (op.kind == DiffOpKind::Equal) {
      CompareRow& row = sink.Next();
      row.left_text = op.text;
      // Under ignore_whitespace an Equal op can pair two whitespace-different
      // lines, so the right column must show the right file's own text.
      row.right_text = op.right_text;
      row.left_line = left_line++;
      row.right_line = right_line++;
      continue;
    }

    const int hunk_start = static_cast<int>(sink.size());
    HunkAlignScratch& hunk_scratch = HunkScratch();
    std::vector<std::string_view>& deleted_lines = hunk_scratch.deleted_lines;
    std::vector<std::string_view>& inserted_lines = hunk_scratch.inserted_lines;
    deleted_lines.clear();
    inserted_lines.clear();
    while (op_index < ops.size() && ops[op_index].kind != DiffOpKind::Equal) {
      if (ops[op_index].kind == DiffOpKind::Delete) {
        deleted_lines.push_back(ops[op_index].text);
      } else if (ops[op_index].kind == DiffOpKind::Insert) {
        inserted_lines.push_back(ops[op_index].text);
      }
      ++op_index;
    }
    --op_index;

    const int hunk_index = static_cast<int>(model.hunks.size());
    const Clock::time_point hunk_alignment_start = Clock::now();
    std::vector<HunkAlignmentKind>& alignment = hunk_scratch.alignment;
    AlignHunkLinesInto(deleted_lines, inserted_lines, &profile, alignment);
    profile.hunk_alignment_ns += DurationNs(hunk_alignment_start, Clock::now());

    std::size_t deleted_index = 0;
    std::size_t inserted_index = 0;
    std::size_t intraline_cells_remaining = kMaxHunkIntralineTotalCells;
    for (const HunkAlignmentKind alignment_kind : alignment) {
      CompareRow& compare_row = sink.Next();
      compare_row.hunk = hunk_index;
      if (alignment_kind != HunkAlignmentKind::Insert) {
        compare_row.left_text = deleted_lines[deleted_index++];
        compare_row.left_line = left_line++;
      }
      if (alignment_kind != HunkAlignmentKind::Delete) {
        compare_row.right_text = inserted_lines[inserted_index++];
        compare_row.right_line = right_line++;
      }

      if (compare_row.left_line != 0 && compare_row.right_line != 0) {
        compare_row.kind = CompareRowKind::Modified;
      } else if (compare_row.left_line != 0) {
        compare_row.kind = CompareRowKind::Deleted;
      } else {
        compare_row.kind = CompareRowKind::Added;
      }
      const Clock::time_point intraline_start = Clock::now();
      PopulateChangedSpans(compare_row, &profile, &intraline_cells_remaining);
      profile.intraline_ns += DurationNs(intraline_start, Clock::now());
    }

    model.hunks.push_back(CompareHunk{
        .index = hunk_index,
        .start_row = hunk_start,
        .end_row = static_cast<int>(sink.size()) - 1,
    });
  }

  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    CompareRow& row = sink.Next();
    row.left_text = left_lines[index];
    row.right_text = right_lines[index - left_suffix + right_suffix];
    row.left_line = left_line++;
    row.right_line = right_line++;
  }
  sink.Finish();

  profile.total_ns = DurationNs(total_start, Clock::now());
  // Saturating subtract: the residual row-assembly time is total minus the
  // measured sub-steps, but clock jitter / nested measurement overhead can make
  // the sub-steps sum to slightly more than total. On unsigned fields that would
  // wrap to an enormous value and pollute the perf profile, so floor at zero.
  const std::uint64_t measured_substeps = profile.split_lines_ns + profile.line_alignment_ns +
                                          profile.hunk_alignment_ns + profile.intraline_ns;
  profile.row_assembly_ns = util::SaturatingSub(profile.total_ns, measured_substeps);

  // The stage profile above has always existed, but only microide_diff_bench
  // ever read it -- a compare that felt slow in the running app had no way to
  // say which stage was responsible. Fold the same numbers into the ranked
  // summary so a live session answers that question directly. Off unless
  // MICROIDE_PERF_SUMMARY is set, and the timings are already taken either way.
  util::PerformanceTrace::RecordSampleNs("compare::Build::SplitLines", profile.split_lines_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::LineAlignment",
                                         profile.line_alignment_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::HunkAlignment",
                                         profile.hunk_alignment_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::Intraline", profile.intraline_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::RowAssembly", profile.row_assembly_ns);

  util::AddPerformanceCounter(util::PerfCounterId::CompareModelBuilds);
  util::AddPerformanceCounter(util::PerfCounterId::CompareModelInputLines,
                              left_lines.size() + right_lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::CompareModelRowsProduced, model.rows.size());
  util::AddPerformanceCounter(util::PerfCounterId::CompareIntralineDiffLines,
                              profile.token_intraline_calls + profile.codepoint_intraline_calls);
  return profile;
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left,
                                             const std::string& right,
                                             const CompareBuildOptions& options) {
  CompareBuildResult result;
  result.profile = BuildCompareModelProfiledInto(result.model, MakeCompareText(left),
                                                 MakeCompareText(right), options);
  return result;
}

CompareModel BuildCompareModel(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right).model;
}

void BuildCompareModelInto(CompareModel& model,
                           CompareTextBuffer left,
                           CompareTextBuffer right,
                           const CompareBuildOptions& options) {
  (void)BuildCompareModelProfiledInto(model, std::move(left), std::move(right), options);
}

void BuildCompareModelInto(CompareModel& model,
                           const std::string& left,
                           const std::string& right,
                           const CompareBuildOptions& options) {
  (void)BuildCompareModelProfiledInto(model, MakeCompareText(left), MakeCompareText(right),
                                      options);
}

CompareModel BuildCompareModel(const std::string& left,
                               const std::string& right,
                               const CompareBuildOptions& options) {
  return BuildCompareModelProfiled(left, right, options).model;
}

}  // namespace microide::compare
