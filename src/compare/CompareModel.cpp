#include "compare/CompareModel.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "util/StringUtil.h"

namespace microide::compare {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxLineLcsMatrixCells = 250'000;
constexpr std::size_t kMaxHunkAlignmentMatrixCells = 65'536;
constexpr std::size_t kMaxIntralineLcsMatrixCells = 65'536;

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
  if (std::isspace(byte) != 0) {
    return LineTokenKind::Whitespace;
  }
  if (std::isalnum(byte) != 0 || byte == '_') {
    return LineTokenKind::Word;
  }
  return LineTokenKind::Symbol;
}

std::vector<std::size_t> BuildUtf8Offsets(std::string_view text) {
  std::vector<std::size_t> offsets;
  offsets.reserve(text.size() + 1);
  for (std::size_t offset = 0; offset < text.size();) {
    offsets.push_back(offset);
    offset += util::Utf8SequenceLength(text, offset);
  }
  offsets.push_back(text.size());
  return offsets;
}

TokenizedLine TokenizeLine(std::string_view text) {
  TokenizedLine tokenized;
  tokenized.text = text;
  tokenized.tokens.reserve(text.size());
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
  return tokenized;
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

std::size_t CommonSignificantTokenBytes(const TokenizedLine& left, const TokenizedLine& right) {
  const std::size_t left_count = left.significant_token_indices.size();
  const std::size_t right_count = right.significant_token_indices.size();
  if (left_count == 0 || right_count == 0) {
    return 0;
  }

  std::vector<std::size_t> dp((left_count + 1) * (right_count + 1), 0);
  auto at = [&](std::size_t i, std::size_t j) -> std::size_t& {
    return dp[i * (right_count + 1) + j];
  };

  for (std::size_t i = left_count; i-- > 0;) {
    for (std::size_t j = right_count; j-- > 0;) {
      const LineToken& left_token = left.tokens[left.significant_token_indices[i]];
      const LineToken& right_token = right.tokens[right.significant_token_indices[j]];
      if (TokenEquals(left.text, left_token, right.text, right_token)) {
        at(i, j) = (left_token.end - left_token.start) + at(i + 1, j + 1);
      } else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  return at(0, 0);
}

std::size_t LineMatchWeight(const TokenizedLine& line, std::size_t occurrences) {
  const std::size_t informative_bytes =
      line.significant_token_bytes > 0 ? line.significant_token_bytes
                                       : (line.text.empty() ? 1 : std::size_t{2});
  const std::size_t rarity = std::max<std::size_t>(1, occurrences);
  return std::max<std::size_t>(1, (informative_bytes + 8) / rarity);
}

double LineSimilarity(const TokenizedLine& left, const TokenizedLine& right) {
  if (left.text == right.text) {
    return 1.0;
  }
  if (left.significant_token_indices.empty() && right.significant_token_indices.empty()) {
    return 1.0;
  }
  if (left.significant_token_bytes == 0 || right.significant_token_bytes == 0) {
    return 0.0;
  }

  const std::size_t common_bytes = CommonSignificantTokenBytes(left, right);
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
    return true;
  }
  if ((left_count == 1 && right_count > 1) || (right_count == 1 && left_count > 1)) {
    return false;
  }
  return similarity >= 0.35;
}

std::vector<HunkAlignmentKind> AlignHunkLines(const std::vector<std::string>& deleted_lines,
                                              const std::vector<std::string>& inserted_lines,
                                              CompareBuildProfile* profile) {
  const std::size_t left_count = deleted_lines.size();
  const std::size_t right_count = inserted_lines.size();
  std::vector<HunkAlignmentKind> alignment;
  if (left_count == 0) {
    alignment.assign(right_count, HunkAlignmentKind::Insert);
    return alignment;
  }
  if (right_count == 0) {
    alignment.assign(left_count, HunkAlignmentKind::Delete);
    return alignment;
  }
  if (ProductExceeds(left_count + 1, right_count + 1, kMaxHunkAlignmentMatrixCells)) {
    if (profile != nullptr) {
      ++profile->fallback_hunk_alignment_calls;
    }
    const std::size_t paired = std::min(left_count, right_count);
    alignment.reserve(left_count + right_count - paired);
    alignment.insert(alignment.end(), paired, HunkAlignmentKind::Pair);
    alignment.insert(alignment.end(), left_count - paired, HunkAlignmentKind::Delete);
    alignment.insert(alignment.end(), right_count - paired, HunkAlignmentKind::Insert);
    return alignment;
  }
  if (profile != nullptr) {
    ++profile->exact_hunk_alignment_calls;
  }

  const std::vector<TokenizedLine> left_tokenized = [&] {
    std::vector<TokenizedLine> lines;
    lines.reserve(left_count);
    for (const auto& line : deleted_lines) {
      lines.push_back(TokenizeLine(line));
    }
    return lines;
  }();
  const std::vector<TokenizedLine> right_tokenized = [&] {
    std::vector<TokenizedLine> lines;
    lines.reserve(right_count);
    for (const auto& line : inserted_lines) {
      lines.push_back(TokenizeLine(line));
    }
    return lines;
  }();

  // Compute similarities lazily: -1 means "not yet computed".
  std::vector<double> similarity(left_count * right_count, -1.0);
  auto similarity_at = [&](std::size_t i, std::size_t j) -> double {
    double& val = similarity[i * right_count + j];
    if (val < 0.0) {
      val = LineSimilarity(left_tokenized[i], right_tokenized[j]);
    }
    return val;
  };

  std::vector<double> dp((left_count + 1) * (right_count + 1), 0.0);
  std::vector<HunkAlignmentKind> choice((left_count + 1) * (right_count + 1),
                                        HunkAlignmentKind::Pair);
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

      const double pair_similarity = similarity_at(i, j);
      if (CanPairAlignedLines(pair_similarity, left_count, right_count)) {
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

  return alignment;
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

std::vector<CompareTextSpan> BuildSpansFromChangedCodepoints(
    const std::vector<std::size_t>& offsets,
    const std::vector<bool>& changed) {
  std::vector<CompareTextSpan> spans;
  if (offsets.size() < 2 || changed.empty()) {
    return spans;
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
  return spans;
}

std::vector<CompareTextSpan> BuildSpansFromChangedTokens(const TokenizedLine& line,
                                                         const std::vector<bool>& changed) {
  std::vector<CompareTextSpan> spans;
  if (line.tokens.empty() || changed.empty()) {
    return spans;
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
  return spans;
}

std::vector<CompareTextSpan> TrimSpansToByteWindow(const std::vector<CompareTextSpan>& spans,
                                                   std::size_t start,
                                                   std::size_t end) {
  std::vector<CompareTextSpan> trimmed;
  trimmed.reserve(spans.size());
  for (const auto& span : spans) {
    if (span.end <= start || span.start >= end) {
      continue;
    }
    const std::size_t clipped_start = std::max(span.start, start);
    const std::size_t clipped_end = std::min(span.end, end);
    if (clipped_end > clipped_start) {
      trimmed.push_back(CompareTextSpan{clipped_start, clipped_end});
    }
  }
  return trimmed;
}

void TrimChangedSpansToSharedEdges(CompareRow& row) {
  if (row.kind != CompareRowKind::Modified) {
    return;
  }

  const std::vector<std::size_t> left_offsets = BuildUtf8Offsets(row.left_text);
  const std::vector<std::size_t> right_offsets = BuildUtf8Offsets(row.right_text);
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

  row.left_changed_spans = TrimSpansToByteWindow(row.left_changed_spans, left_offsets[prefix],
                                                 left_offsets[left_suffix]);
  row.right_changed_spans = TrimSpansToByteWindow(row.right_changed_spans, right_offsets[prefix],
                                                  right_offsets[right_suffix]);
}

bool PopulateTokenChangedSpans(CompareRow& row) {
  const TokenizedLine left = TokenizeLine(row.left_text);
  const TokenizedLine right = TokenizeLine(row.right_text);
  if (left.tokens.size() < 2 || right.tokens.size() < 2 ||
      ProductExceeds(left.tokens.size() + 1, right.tokens.size() + 1, kMaxIntralineLcsMatrixCells)) {
    return false;
  }

  std::vector<bool> left_changed(left.tokens.size(), true);
  std::vector<bool> right_changed(right.tokens.size(), true);

  std::size_t prefix = 0;
  while (prefix < left.tokens.size() && prefix < right.tokens.size() &&
         TokenEquals(left.text, left.tokens[prefix], right.text, right.tokens[prefix])) {
    left_changed[prefix] = false;
    right_changed[prefix] = false;
    ++prefix;
  }

  std::size_t left_suffix = left.tokens.size();
  std::size_t right_suffix = right.tokens.size();
  while (left_suffix > prefix && right_suffix > prefix &&
         TokenEquals(left.text, left.tokens[left_suffix - 1], right.text,
                     right.tokens[right_suffix - 1])) {
    --left_suffix;
    --right_suffix;
    left_changed[left_suffix] = false;
    right_changed[right_suffix] = false;
  }

  const std::size_t left_middle_count = left_suffix - prefix;
  const std::size_t right_middle_count = right_suffix - prefix;
  if (left_middle_count == 0 && right_middle_count == 0) {
    row.left_changed_spans.clear();
    row.right_changed_spans.clear();
    return true;
  }
  if (left_middle_count == 0 || right_middle_count == 0) {
    row.left_changed_spans = BuildSpansFromChangedTokens(left, left_changed);
    row.right_changed_spans = BuildSpansFromChangedTokens(right, right_changed);
    return true;
  }

  std::vector<std::size_t> dp((left_middle_count + 1) * (right_middle_count + 1), 0);
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
        left_changed[left_index] = false;
        right_changed[right_index] = false;
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

  row.left_changed_spans = BuildSpansFromChangedTokens(left, left_changed);
  row.right_changed_spans = BuildSpansFromChangedTokens(right, right_changed);
  return !(row.left_changed_spans.empty() && row.right_changed_spans.empty());
}

void PopulateCodepointChangedSpans(CompareRow& row) {
  row.left_changed_spans.clear();
  row.right_changed_spans.clear();

  const std::vector<std::size_t> left_offsets = BuildUtf8Offsets(row.left_text);
  const std::vector<std::size_t> right_offsets = BuildUtf8Offsets(row.right_text);
  const std::size_t left_count = left_offsets.size() - 1;
  const std::size_t right_count = right_offsets.size() - 1;
  std::vector<bool> left_changed(left_count, true);
  std::vector<bool> right_changed(right_count, true);

  std::size_t prefix = 0;
  while (prefix < left_count && prefix < right_count &&
         Utf8CodepointEquals(row.left_text, left_offsets, prefix, row.right_text, right_offsets,
                             prefix)) {
    left_changed[prefix] = false;
    right_changed[prefix] = false;
    ++prefix;
  }

  std::size_t left_suffix = left_count;
  std::size_t right_suffix = right_count;
  while (left_suffix > prefix && right_suffix > prefix &&
         Utf8CodepointEquals(row.left_text, left_offsets, left_suffix - 1, row.right_text,
                             right_offsets, right_suffix - 1)) {
    --left_suffix;
    --right_suffix;
    left_changed[left_suffix] = false;
    right_changed[right_suffix] = false;
  }

  const std::size_t left_middle_count = left_suffix - prefix;
  const std::size_t right_middle_count = right_suffix - prefix;
  if (left_middle_count > 0 && right_middle_count > 0 &&
      !ProductExceeds(left_middle_count + 1, right_middle_count + 1,
                      kMaxIntralineLcsMatrixCells)) {
    std::vector<int> dp((left_middle_count + 1) * (right_middle_count + 1), 0);
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
        left_changed[prefix + i] = false;
        right_changed[prefix + j] = false;
        ++i;
        ++j;
      } else if (at(i + 1, j) >= at(i, j + 1)) {
        ++i;
      } else {
        ++j;
      }
    }
  }

  row.left_changed_spans = BuildSpansFromChangedCodepoints(left_offsets, left_changed);
  row.right_changed_spans = BuildSpansFromChangedCodepoints(right_offsets, right_changed);
}

void PopulateChangedSpans(CompareRow& row, CompareBuildProfile* profile) {
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

void AppendDeleteOps(const std::vector<std::string>& lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Delete, lines[i]});
  }
}

void AppendInsertOps(const std::vector<std::string>& lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Insert, lines[i]});
  }
}

std::vector<DiffOp> BuildExactLineOps(const std::vector<std::string>& left_lines,
                                      const std::vector<std::string>& right_lines) {
  const std::size_t left_count = left_lines.size();
  const std::size_t right_count = right_lines.size();
  std::vector<DiffOp> ops;
  if (left_count == 0 && right_count == 0) {
    return ops;
  }

  std::vector<TokenizedLine> left_tokenized;
  left_tokenized.reserve(left_count);
  for (const auto& line : left_lines) {
    left_tokenized.push_back(TokenizeLine(line));
  }
  std::vector<TokenizedLine> right_tokenized;
  right_tokenized.reserve(right_count);
  for (const auto& line : right_lines) {
    right_tokenized.push_back(TokenizeLine(line));
  }

  std::unordered_map<std::string_view, std::size_t> line_occurrences;
  line_occurrences.reserve(left_count + right_count);
  for (const auto& line : left_lines) {
    ++line_occurrences[line];
  }
  for (const auto& line : right_lines) {
    ++line_occurrences[line];
  }

  std::vector<std::size_t> left_match_weight(left_count, 1);
  for (std::size_t i = 0; i < left_count; ++i) {
    left_match_weight[i] = LineMatchWeight(left_tokenized[i], line_occurrences[left_lines[i]]);
  }

  std::vector<std::size_t> dp((left_count + 1) * (right_count + 1), 0);
  auto at = [&](std::size_t i, std::size_t j) -> std::size_t& {
    return dp[i * (right_count + 1) + j];
  };

  for (std::size_t i = left_count; i-- > 0;) {
    for (std::size_t j = right_count; j-- > 0;) {
      std::size_t best = std::max(at(i + 1, j), at(i, j + 1));
      if (left_lines[i] == right_lines[j]) {
        best = std::max(best, left_match_weight[i] + at(i + 1, j + 1));
      }
      at(i, j) = best;
    }
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_count && j < right_count) {
    if (left_lines[i] == right_lines[j]) {
      const std::size_t diagonal = left_match_weight[i] + at(i + 1, j + 1);
      if (diagonal >= at(i + 1, j) && diagonal >= at(i, j + 1) && at(i, j) == diagonal) {
        ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
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
  return ops;
}

std::vector<std::pair<std::size_t, std::size_t>> BuildUniqueLineAnchors(
    const std::vector<std::string>& left_lines,
    std::size_t left_begin,
    std::size_t left_end,
    const std::vector<std::string>& right_lines,
    std::size_t right_begin,
    std::size_t right_end) {
  struct AnchorInfo {
    std::size_t left_count = 0;
    std::size_t right_count = 0;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
  };

  std::unordered_map<std::string_view, AnchorInfo> info_by_line;
  info_by_line.reserve((left_end - left_begin) + (right_end - right_begin));
  for (std::size_t i = left_begin; i < left_end; ++i) {
    AnchorInfo& info = info_by_line[left_lines[i]];
    ++info.left_count;
    info.left_index = i;
  }
  for (std::size_t i = right_begin; i < right_end; ++i) {
    AnchorInfo& info = info_by_line[right_lines[i]];
    ++info.right_count;
    info.right_index = i;
  }

  std::vector<std::pair<std::size_t, std::size_t>> candidates;
  candidates.reserve(std::min(left_end - left_begin, right_end - right_begin));
  for (const auto& [line, info] : info_by_line) {
    if (line.empty()) {
      continue;
    }
    if (info.left_count == 1 && info.right_count == 1) {
      candidates.emplace_back(info.left_index, info.right_index);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  if (candidates.empty()) {
    return {};
  }

  const std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> pile_tops;
  std::vector<std::size_t> pile_candidate_indices;
  std::vector<std::size_t> predecessor(candidates.size(), kNoIndex);
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

  std::vector<std::pair<std::size_t, std::size_t>> anchors;
  if (pile_candidate_indices.empty()) {
    return anchors;
  }
  for (std::size_t index = pile_candidate_indices.back(); index != kNoIndex;
       index = predecessor[index]) {
    anchors.push_back(candidates[index]);
  }
  std::reverse(anchors.begin(), anchors.end());
  return anchors;
}

void AppendAnchoredFallbackOps(const std::vector<std::string>& left_lines,
                               std::size_t left_begin,
                               std::size_t left_end,
                               const std::vector<std::string>& right_lines,
                               std::size_t right_begin,
                               std::size_t right_end,
                               std::vector<DiffOp>& ops) {
  while (left_begin < left_end && right_begin < right_end &&
         left_lines[left_begin] == right_lines[right_begin]) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[left_begin]});
    ++left_begin;
    ++right_begin;
  }

  std::size_t left_suffix = left_end;
  std::size_t right_suffix = right_end;
  while (left_suffix > left_begin && right_suffix > right_begin &&
         left_lines[left_suffix - 1] == right_lines[right_suffix - 1]) {
    --left_suffix;
    --right_suffix;
  }

  if (left_begin == left_suffix && right_begin == right_suffix) {
    for (std::size_t i = left_suffix; i < left_end; ++i) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
    }
    return;
  }
  if (left_begin == left_suffix) {
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    for (std::size_t i = left_suffix; i < left_end; ++i) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
    }
    return;
  }
  if (right_begin == right_suffix) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    for (std::size_t i = left_suffix; i < left_end; ++i) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
    }
    return;
  }

  const std::size_t middle_left_count = left_suffix - left_begin;
  const std::size_t middle_right_count = right_suffix - right_begin;
  if (!ProductExceeds(middle_left_count + 1, middle_right_count + 1, kMaxLineLcsMatrixCells)) {
    const std::vector<std::string> left_slice(
        left_lines.begin() + static_cast<std::ptrdiff_t>(left_begin),
        left_lines.begin() + static_cast<std::ptrdiff_t>(left_suffix));
    const std::vector<std::string> right_slice(
        right_lines.begin() + static_cast<std::ptrdiff_t>(right_begin),
        right_lines.begin() + static_cast<std::ptrdiff_t>(right_suffix));
    const std::vector<DiffOp> exact_ops = BuildExactLineOps(left_slice, right_slice);
    ops.insert(ops.end(), exact_ops.begin(), exact_ops.end());
    for (std::size_t i = left_suffix; i < left_end; ++i) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
    }
    return;
  }

  const std::vector<std::pair<std::size_t, std::size_t>> anchors =
      BuildUniqueLineAnchors(left_lines, left_begin, left_suffix, right_lines, right_begin,
                             right_suffix);
  if (anchors.empty()) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    for (std::size_t i = left_suffix; i < left_end; ++i) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
    }
    return;
  }

  std::size_t segment_left_begin = left_begin;
  std::size_t segment_right_begin = right_begin;
  for (const auto& [anchor_left, anchor_right] : anchors) {
    AppendAnchoredFallbackOps(left_lines, segment_left_begin, anchor_left, right_lines,
                              segment_right_begin, anchor_right, ops);
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[anchor_left]});
    segment_left_begin = anchor_left + 1;
    segment_right_begin = anchor_right + 1;
  }
  AppendAnchoredFallbackOps(left_lines, segment_left_begin, left_suffix, right_lines,
                            segment_right_begin, right_suffix, ops);
  for (std::size_t i = left_suffix; i < left_end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
  }
}

std::vector<DiffOp> BuildAnchoredFallbackOps(const std::vector<std::string>& left_lines,
                                             const std::vector<std::string>& right_lines) {
  std::vector<DiffOp> ops;
  ops.reserve(left_lines.size() + right_lines.size());
  AppendAnchoredFallbackOps(left_lines, 0, left_lines.size(), right_lines, 0, right_lines.size(),
                            ops);
  return ops;
}

}  // namespace

namespace {

bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right) {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() || right_index < right.size()) {
    while (left_index < left.size() && std::isspace(static_cast<unsigned char>(left[left_index])) != 0) {
      ++left_index;
    }
    while (right_index < right.size() &&
           std::isspace(static_cast<unsigned char>(right[right_index])) != 0) {
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

std::vector<DiffOp> BuildLineDiffOps(const std::vector<std::string>& left_lines,
                                     const std::vector<std::string>& right_lines,
                                     LineDiffBuildStats* stats) {
  return BuildLineDiffOps(left_lines, right_lines, CompareBuildOptions{}, stats);
}

std::vector<DiffOp> BuildLineDiffOps(const std::vector<std::string>& left_lines,
                                     const std::vector<std::string>& right_lines,
                                     const CompareBuildOptions& options,
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

  std::vector<DiffOp> ops;
  ops.reserve(left_lines.size() + right_lines.size());
  for (std::size_t index = 0; index < prefix; ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index]});
  }

  const std::vector<std::string> left_middle(left_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                                             left_lines.begin() +
                                                 static_cast<std::ptrdiff_t>(left_suffix));
  const std::vector<std::string> right_middle(
      right_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
      right_lines.begin() + static_cast<std::ptrdiff_t>(right_suffix));
  if (!left_middle.empty() || !right_middle.empty()) {
    const std::size_t left_count = left_middle.size();
    const std::size_t right_count = right_middle.size();
    std::vector<DiffOp> middle_ops;
    if (ProductExceeds(left_count + 1, right_count + 1, kMaxLineLcsMatrixCells)) {
      if (stats != nullptr) {
        ++stats->anchored_alignment_calls;
      }
      middle_ops = BuildAnchoredFallbackOps(left_middle, right_middle);
    } else {
      if (stats != nullptr) {
        ++stats->exact_alignment_calls;
      }
      middle_ops = BuildExactLineOps(left_middle, right_middle);
    }
    ops.insert(ops.end(), middle_ops.begin(), middle_ops.end());
  }

  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index]});
  }
  return ops;
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right, CompareBuildOptions{});
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left,
                                             const std::string& right,
                                             const CompareBuildOptions& options) {
  CompareBuildResult result;
  CompareModel& model = result.model;
  CompareBuildProfile& profile = result.profile;

  const Clock::time_point total_start = Clock::now();

  const Clock::time_point split_start = Clock::now();
  const auto left_lines = util::SplitLines(left);
  const auto right_lines = util::SplitLines(right);
  profile.split_lines_ns = DurationNs(split_start, Clock::now());

  const bool lines_equal = [&]() {
    if (left_lines.size() != right_lines.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left_lines.size(); ++i) {
      if (!LinesEqualForDiff(left_lines[i], right_lines[i], options)) {
        return false;
      }
    }
    return true;
  }();
  if (lines_equal) {
    model.rows.reserve(left_lines.size());
    int line_number = 1;
    for (std::size_t i = 0; i < left_lines.size(); ++i) {
      // Under ignore_whitespace the two sides can compare equal while differing in
      // whitespace; the right column must still show the right file's actual text,
      // not a copy of the left line.
      model.rows.push_back(CompareRow{
          .left_text = left_lines[i],
          .right_text = right_lines[i],
          .left_line = line_number,
          .right_line = line_number,
          .kind = CompareRowKind::Unchanged,
          .hunk = -1,
          .left_changed_spans = {},
          .right_changed_spans = {},
      });
      ++line_number;
    }
    profile.total_ns = DurationNs(total_start, Clock::now());
    profile.row_assembly_ns =
        profile.total_ns - profile.split_lines_ns - profile.line_alignment_ns -
        profile.hunk_alignment_ns - profile.intraline_ns;
    return result;
  }

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

  const std::vector<std::string> left_middle(left_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                                             left_lines.begin() +
                                                 static_cast<std::ptrdiff_t>(left_suffix));
  const std::vector<std::string> right_middle(
      right_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
      right_lines.begin() + static_cast<std::ptrdiff_t>(right_suffix));

  LineDiffBuildStats line_diff_stats;
  const std::vector<DiffOp> ops =
      BuildLineDiffOps(left_middle, right_middle, options, &line_diff_stats);
  profile.exact_line_alignment_calls += line_diff_stats.exact_alignment_calls;
  profile.anchored_line_alignment_calls += line_diff_stats.anchored_alignment_calls;
  profile.line_alignment_ns = DurationNs(line_alignment_start, Clock::now());

  model.rows.reserve(left_lines.size() + right_lines.size());
  int left_line = 1;
  int right_line = 1;
  for (std::size_t index = 0; index < prefix; ++index) {
    model.rows.push_back(CompareRow{
        .left_text = left_lines[index],
        .right_text = right_lines[index],
        .left_line = left_line++,
        .right_line = right_line++,
        .kind = CompareRowKind::Unchanged,
        .hunk = -1,
        .left_changed_spans = {},
        .right_changed_spans = {},
    });
  }

  for (std::size_t op_index = 0; op_index < ops.size(); ++op_index) {
    const auto& op = ops[op_index];
    if (op.kind == DiffOpKind::Equal) {
      model.rows.push_back(CompareRow{
          .left_text = op.text,
          .right_text = op.text,
          .left_line = left_line++,
          .right_line = right_line++,
          .kind = CompareRowKind::Unchanged,
          .hunk = -1,
          .left_changed_spans = {},
          .right_changed_spans = {},
      });
      continue;
    }

    const int hunk_start = static_cast<int>(model.rows.size());
    std::vector<std::string> deleted_lines;
    std::vector<std::string> inserted_lines;
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
    const std::vector<HunkAlignmentKind> alignment =
        AlignHunkLines(deleted_lines, inserted_lines, &profile);
    profile.hunk_alignment_ns += DurationNs(hunk_alignment_start, Clock::now());

    std::size_t deleted_index = 0;
    std::size_t inserted_index = 0;
    for (const HunkAlignmentKind alignment_kind : alignment) {
      CompareRow compare_row;
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
      PopulateChangedSpans(compare_row, &profile);
      profile.intraline_ns += DurationNs(intraline_start, Clock::now());
      model.rows.push_back(std::move(compare_row));
    }

    model.hunks.push_back(CompareHunk{
        .index = hunk_index,
        .start_row = hunk_start,
        .end_row = static_cast<int>(model.rows.size()) - 1,
    });
  }

  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    model.rows.push_back(CompareRow{
        .left_text = left_lines[index],
        .right_text = right_lines[index - left_suffix + right_suffix],
        .left_line = left_line++,
        .right_line = right_line++,
        .kind = CompareRowKind::Unchanged,
        .hunk = -1,
        .left_changed_spans = {},
        .right_changed_spans = {},
    });
  }

  profile.total_ns = DurationNs(total_start, Clock::now());
  profile.row_assembly_ns =
      profile.total_ns - profile.split_lines_ns - profile.line_alignment_ns -
      profile.hunk_alignment_ns - profile.intraline_ns;
  return result;
}

CompareModel BuildCompareModel(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right).model;
}

CompareModel BuildCompareModel(const std::string& left,
                               const std::string& right,
                               const CompareBuildOptions& options) {
  return BuildCompareModelProfiled(left, right, options).model;
}

}  // namespace microide::compare
