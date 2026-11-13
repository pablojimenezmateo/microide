#include "compare/CompareMoves.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Fnv1a.h"

// Moved-code detection (TD-2026-11-11-443). The structural refinement matches
// each hunk alone -- it has to, the editable compare pane rebuilds per keystroke
// and only the changed hunk refines again -- so code moved from one hunk to
// another was novel in both. This pass reads the finished rows the way git's
// --color-moved does: a run of whole deleted lines in one hunk that reappears as
// a run of whole added lines in ANOTHER hunk is a move. Lines compare with their
// indentation trimmed (moved and re-indented is still moved), and a block must
// carry enough text to mean something -- a lone `}` reappearing elsewhere is not
// a move. Linear in the hunks' lines: a hash per line, a bounded candidate list.
namespace microide::compare {
namespace {

// git's default for --color-moved: a block needs this many alphanumeric
// characters to count.
constexpr std::size_t kMinMovedAlnum = 20;
// Candidates tried per deleted line: a file full of identical lines (`}` alone
// is excluded by the threshold, but `return;` is not) must stay linear.
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
  std::vector<std::pair<std::uint64_t, std::uint32_t>> added;  // (hash of trimmed text, row)
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

}  // namespace

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
        continue;  // within one hunk the structural match already decided
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
    // A block ends on text, not on blank lines it happened to run through.
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

}  // namespace microide::compare
