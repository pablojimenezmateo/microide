#include "compare/CompareMoves.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Fnv1a.h"

namespace microide::compare {
namespace {

constexpr std::size_t kMinMovedAlnum = 20;
constexpr std::size_t kMaxCandidates = 16;

std::string_view Trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

std::size_t AlnumCount(std::string_view text) {
  std::size_t count = 0;
  for (const char c : text) {
    count += std::isalnum(static_cast<unsigned char>(c)) != 0 ? 1 : 0;
  }
  return count;
}

struct MoveScratch {
  std::vector<std::pair<std::uint64_t, std::uint32_t>> added;
};

MoveScratch& Scratch() {
  thread_local MoveScratch scratch;
  return scratch;
}

bool IsDeleted(const CompareRow& row) {
  return row.kind == CompareRowKind::Deleted && row.left_line != 0 && row.right_line == 0;
}

bool IsAdded(const CompareRow& row) {
  return row.kind == CompareRowKind::Added && row.right_line != 0 && row.left_line == 0;
}

}

void MarkMovedLines(CompareModel& model) {
  std::vector<CompareRow>& rows = model.rows;
  MoveScratch& scratch = Scratch();
  scratch.added.clear();
  for (std::uint32_t index = 0; index < rows.size(); ++index) {
    rows[index].left_moved = false;
    rows[index].right_moved = false;
    if (IsAdded(rows[index])) {
      const std::string_view text = Trimmed(rows[index].right_text);
      if (!text.empty()) {
        scratch.added.emplace_back(util::Fnv1aBytes(util::kFnv1aOffsetBasis, text), index);
      }
    }
  }
  if (scratch.added.empty()) {
    return;
  }
  std::sort(scratch.added.begin(), scratch.added.end());

  std::size_t at = 0;
  while (at < rows.size()) {
    if (!IsDeleted(rows[at]) || rows[at].left_moved) {
      ++at;
      continue;
    }
    const std::string_view text = Trimmed(rows[at].left_text);
    if (text.empty()) {
      ++at;
      continue;
    }
    const std::uint64_t hash = util::Fnv1aBytes(util::kFnv1aOffsetBasis, text);
    auto candidate = std::lower_bound(scratch.added.begin(), scratch.added.end(),
                                      std::pair<std::uint64_t, std::uint32_t>{hash, 0});
    std::size_t best_length = 0;
    std::uint32_t best_added = 0;
    for (std::size_t tried = 0;
         candidate != scratch.added.end() && candidate->first == hash && tried < kMaxCandidates;
         ++candidate, ++tried) {
      const std::uint32_t added = candidate->second;
      if (rows[added].hunk == rows[at].hunk || rows[added].right_moved) {
        continue;
      }
      std::size_t length = 0;
      while (at + length < rows.size() && added + length < rows.size()) {
        const CompareRow& left = rows[at + length];
        const CompareRow& right = rows[added + length];
        if (!IsDeleted(left) || left.left_moved || left.hunk != rows[at].hunk || !IsAdded(right) ||
            right.right_moved || right.hunk != rows[added].hunk ||
            Trimmed(left.left_text) != Trimmed(right.right_text)) {
          break;
        }
        ++length;
      }
      if (length > best_length) {
        best_length = length;
        best_added = added;
      }
    }
    while (best_length > 0 && Trimmed(rows[at + best_length - 1].left_text).empty()) {
      --best_length;
    }
    std::size_t alnum = 0;
    for (std::size_t offset = 0; offset < best_length; ++offset) {
      alnum += AlnumCount(rows[at + offset].left_text);
    }
    if (best_length == 0 || alnum < kMinMovedAlnum) {
      ++at;
      continue;
    }
    for (std::size_t offset = 0; offset < best_length; ++offset) {
      rows[at + offset].left_moved = true;
      rows[best_added + offset].right_moved = true;
    }
    at += best_length;
  }
}

}
