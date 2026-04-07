#include "compare/CompareModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace microide::compare {

namespace {

enum class DiffOpKind {
  Equal,
  Delete,
  Insert,
};

struct DiffOp {
  DiffOpKind kind = DiffOpKind::Equal;
  std::string text;
};

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

std::vector<std::string> SplitCompareLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
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

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if ((byte & 0xC0u) != 0x80u) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) && continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

std::vector<std::size_t> BuildUtf8Offsets(std::string_view text) {
  std::vector<std::size_t> offsets;
  offsets.reserve(text.size() + 1);
  for (std::size_t offset = 0; offset < text.size();) {
    offsets.push_back(offset);
    offset += Utf8SequenceLength(text, offset);
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
    offset += Utf8SequenceLength(text, offset);
    if (kind != LineTokenKind::Symbol) {
      while (offset < text.size() && ClassifyCodepoint(text, offset) == kind) {
        offset += Utf8SequenceLength(text, offset);
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
  return similarity >= 0.35;
}

std::vector<HunkAlignmentKind> AlignHunkLines(const std::vector<std::string>& deleted_lines,
                                              const std::vector<std::string>& inserted_lines) {
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

  std::vector<double> similarity(left_count * right_count, 0.0);
  auto similarity_at = [&](std::size_t i, std::size_t j) -> double& {
    return similarity[i * right_count + j];
  };
  for (std::size_t i = 0; i < left_count; ++i) {
    for (std::size_t j = 0; j < right_count; ++j) {
      similarity_at(i, j) = LineSimilarity(left_tokenized[i], right_tokenized[j]);
    }
  }

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

void PopulateChangedSpans(CompareRow& row) {
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
  if (left_middle_count > 0 && right_middle_count > 0) {
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

}  // namespace

CompareModel BuildCompareModel(const std::string& left, const std::string& right) {
  const auto left_lines = SplitCompareLines(left);
  const auto right_lines = SplitCompareLines(right);

  const std::size_t left_count = left_lines.size();
  const std::size_t right_count = right_lines.size();
  std::vector<int> dp((left_count + 1) * (right_count + 1), 0);
  auto at = [&](std::size_t i, std::size_t j) -> int& {
    return dp[i * (right_count + 1) + j];
  };

  for (std::size_t i = left_count; i-- > 0;) {
    for (std::size_t j = right_count; j-- > 0;) {
      if (left_lines[i] == right_lines[j]) {
        at(i, j) = at(i + 1, j + 1) + 1;
      } else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  std::vector<DiffOp> ops;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_count && j < right_count) {
    if (left_lines[i] == right_lines[j]) {
      ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i]});
      ++i;
      ++j;
    } else if (at(i + 1, j) >= at(i, j + 1)) {
      ops.push_back(DiffOp{DiffOpKind::Delete, left_lines[i]});
      ++i;
    } else {
      ops.push_back(DiffOp{DiffOpKind::Insert, right_lines[j]});
      ++j;
    }
  }
  while (i < left_count) {
    ops.push_back(DiffOp{DiffOpKind::Delete, left_lines[i++]});
  }
  while (j < right_count) {
    ops.push_back(DiffOp{DiffOpKind::Insert, right_lines[j++]});
  }

  CompareModel model;
  model.rows.reserve(ops.size());

  int left_line = 1;
  int right_line = 1;
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
    const std::vector<HunkAlignmentKind> alignment =
        AlignHunkLines(deleted_lines, inserted_lines);
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
      PopulateChangedSpans(compare_row);
      model.rows.push_back(std::move(compare_row));
    }

    model.hunks.push_back(CompareHunk{
        .index = hunk_index,
        .start_row = hunk_start,
        .end_row = static_cast<int>(model.rows.size()) - 1,
    });
  }

  return model;
}

}  // namespace microide::compare
