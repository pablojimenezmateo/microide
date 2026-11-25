#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "editor/LineEditSpan.h"
#include "editor/LineSpan.h"

namespace microide::editor {

class TextViewport;

enum class FoldSource : std::uint8_t {
  Bracket = 0,
  Indent = 1,
};

struct FoldRange {
  std::size_t opener_line = 0;
  std::size_t closer_line = 0;
  FoldSource source = FoldSource::Bracket;
};

class FoldingModel {
 public:
  static constexpr std::size_t kAllLines = std::numeric_limits<std::size_t>::max();
  static constexpr std::uint32_t kNoLine = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::size_t kNoLineIndex = std::numeric_limits<std::size_t>::max();

  static constexpr std::size_t kWindowPadLines = 96;

  static constexpr std::size_t kMaxOpenDepth = 256;

  static constexpr std::size_t kMaxForwardResolveLines = 8192;

  static constexpr std::size_t kMaxBracketScanLineBytes = 100000;

  struct Fingerprint {
    std::uint64_t layout_revision = 0;
    std::size_t tab_size = 4;
    std::string language_id;

    bool operator==(const Fingerprint& other) const {
      return layout_revision == other.layout_revision && tab_size == other.tab_size &&
             language_id == other.language_id;
    }
    bool operator!=(const Fingerprint& other) const { return !(*this == other); }
  };

  struct ComputeOptions {
    std::vector<std::pair<char, char>> bracket_pairs;
    bool use_indent_source = true;
    std::size_t tab_size = 4;
  };

  bool Refresh(LineSpan lines,
               const ComputeOptions& options,
               std::size_t first_line,
               std::size_t last_line,
               std::size_t max_lines = 0,
               const LineEditSpan& edit_span = LineEditSpan::FullRebuild(),
               const TextViewport* syntax_viewport = nullptr);

  bool Compute(LineSpan lines, const ComputeOptions& options) {
    return Refresh(lines, options, 0, kAllLines, 0, LineEditSpan::FullRebuild(),
                   nullptr);
  }

  bool ResolveAllFolds(LineSpan lines, const TextViewport* syntax_viewport = nullptr);

  std::optional<FoldRange> FoldStartingAt(std::size_t line) const;
  std::optional<FoldRange> InnermostFoldContaining(std::size_t line) const;
  void AppendFoldsContaining(std::size_t line, std::vector<FoldRange>* out) const;

  const std::vector<FoldRange>& resolved_ranges() const { return ranges_; }
  std::size_t resolved_first_line() const { return resolved_first_line_; }
  std::size_t resolved_last_line() const { return resolved_last_line_; }
  bool IsWindowResolved(std::size_t first_line, std::size_t last_line) const {
    return resolved_ && !dirty_ && first_line >= resolved_first_line_ &&
           (last_line <= resolved_last_line_ || resolved_last_line_ + 1 >= document_line_count_);
  }

  bool ToggleFold(std::size_t opener_line);
  bool Collapse(std::size_t opener_line);
  bool Expand(std::size_t opener_line);
  bool CollapseRange(FoldRange range);
  bool CollapseAllResolved();
  void ExpandAll();

  bool IsLineHidden(std::size_t line) const;
  static constexpr std::size_t kNoVisibleLine = static_cast<std::size_t>(-1);
  std::size_t FirstVisibleLineAtOrAfter(std::size_t line, std::size_t line_count) const;
  std::size_t LastVisibleLineAtOrBefore(std::size_t line) const;
  bool IsCollapsedAtOpener(std::size_t line) const;
  bool has_any_collapsed_fold() const { return !collapsed_.empty(); }
  std::span<const FoldRange> collapsed_ranges() const { return collapsed_; }

  void Clear();

  bool HasFolds() const { return !ranges_.empty() || !collapsed_.empty(); }
  bool complete() const { return complete_; }

  std::size_t revision() const { return revision_; }
  std::size_t layout_revision() const { return layout_revision_; }

  std::size_t ApproximateResidentBytes() const;

  bool IsFresh(const Fingerprint& fingerprint) const {
    return !dirty_ && fingerprint_ == fingerprint;
  }
  void MarkDirty() { dirty_ = true; }
  void SetFingerprint(Fingerprint fingerprint) {
    fingerprint_ = std::move(fingerprint);
    dirty_ = false;
  }
  const Fingerprint& fingerprint() const { return fingerprint_; }

  struct CachedBracket {
    std::uint32_t column = 0;
    char byte = 0;
  };

  struct WordOpener {
    char close = 0;
    std::uint32_t line = 0;
  };

  struct WordCloser {
    char close = 0;
    std::uint32_t line = 0;
  };

  struct WordIndent {
    std::uint32_t level = 0;
    std::uint32_t line = 0;
  };

  struct WordDedent {
    std::uint32_t threshold = 0;
    std::uint32_t closer_line = 0;
  };

  struct StackBracket {
    char close = 0;
    std::size_t line = 0;
  };
  struct StackIndent {
    std::uint32_t level = 0;
    std::size_t line = 0;
  };

  struct WorkBudget {
    static constexpr std::size_t kUnlimited = std::numeric_limits<std::size_t>::max();
    std::size_t remaining = kUnlimited;

    bool available() const { return remaining != 0; }
    void Spend(std::size_t lines = 1) {
      if (remaining == kUnlimited) {
        return;
      }
      remaining = lines >= remaining ? 0 : remaining - lines;
    }
  };

 private:
  struct Block {
    std::uint32_t line_count = 0;
    std::uint32_t event_count = 0;
    bool valid = false;
    std::uint32_t last_nonblank = kNoLine;
    std::vector<WordCloser> bracket_closers;
    std::vector<WordOpener> bracket_openers;
    std::vector<WordDedent> indent_dedents;
    std::vector<WordIndent> indent_openers;
  };

  struct PrefixState {
    std::uint32_t bracket_offset = 0;
    std::uint32_t bracket_count = 0;
    std::uint32_t indent_offset = 0;
    std::uint32_t indent_count = 0;
    std::size_t event_index = 0;
    std::size_t last_nonblank = kNoLineIndex;
  };

  struct WalkState {
    std::vector<StackBracket> brackets;
    std::vector<StackIndent> indents;
    std::size_t last_nonblank = kNoLineIndex;

    std::size_t pending_limit = 0;
    std::size_t pending_brackets = 0;
    std::size_t pending_indents = 0;

    void ArmPending(std::size_t limit) {
      pending_limit = limit;
      pending_brackets = brackets.size();
      pending_indents = indents.size();
    }

    void PushBracket(StackBracket entry) {
      brackets.push_back(entry);
      if (brackets.size() > 2 * kMaxOpenDepth) {
        brackets.erase(brackets.begin(),
                       brackets.begin() + static_cast<std::ptrdiff_t>(kMaxOpenDepth));
        pending_brackets = pending_brackets > kMaxOpenDepth ? pending_brackets - kMaxOpenDepth : 0;
      }
    }
    void PushIndent(StackIndent entry) {
      indents.push_back(entry);
      if (indents.size() > 2 * kMaxOpenDepth) {
        indents.erase(indents.begin(),
                      indents.begin() + static_cast<std::ptrdiff_t>(kMaxOpenDepth));
        pending_indents = pending_indents > kMaxOpenDepth ? pending_indents - kMaxOpenDepth : 0;
      }
    }
    bool pending_resolved() const { return pending_brackets == 0 && pending_indents == 0; }
    void NoteBracketPop(std::size_t line) {
      if (pending_brackets != 0 && line <= pending_limit) {
        --pending_brackets;
      }
    }
    void NoteIndentPop(std::size_t line) {
      if (pending_indents != 0 && line <= pending_limit) {
        --pending_indents;
      }
    }

    void Clear() {
      brackets.clear();
      indents.clear();
      last_nonblank = kNoLineIndex;
      pending_limit = 0;
      pending_brackets = 0;
      pending_indents = 0;
    }
  };

  class BracketTable;
  class SuppressionCursor;

  void SyncLineIndentCache(std::size_t line_count, std::size_t tab_size,
                           const LineEditSpan& edit_span);
  void SyncLineBracketCache(LineSpan lines, const BracketTable& table,
                            const std::vector<std::pair<char, char>>& pairs,
                            const LineEditSpan& edit_span, bool pairs_changed);
  void SpliceLineBracketCache(LineSpan lines, const BracketTable& table,
                              const LineEditSpan& edit_span);
  bool ExtendBracketCache(LineSpan lines, const BracketTable& table, std::size_t needed_end,
                          WorkBudget& budget);
  std::uint32_t IndentAt(LineSpan lines, std::size_t line, std::size_t tab_size,
                         WorkBudget& budget);
  std::size_t EventIndexForLine(std::size_t line) const;

  void SyncBlocks(std::size_t line_count, const LineEditSpan& edit_span, bool reset_all);
  void RepartitionAll(std::size_t line_count);
  void RecomputeBlockEventCount(std::size_t block_index);
  void EnsureBlockStarts() const;
  std::size_t BlockIndexForLine(std::size_t line) const;
  bool EnsureBlockSummary(LineSpan lines, const ComputeOptions& options, const BracketTable& table,
                          std::size_t block_index, std::size_t event_index, WorkBudget& budget);
  std::size_t EnsurePrefixState(LineSpan lines, const ComputeOptions& options,
                                const BracketTable& table, std::size_t boundary,
                                WorkBudget& budget);
  void LoadPrefixState(std::size_t boundary, WalkState& out) const;
  void StorePrefixState(std::size_t boundary, const WalkState& state, std::size_t event_index);
  void InvalidatePrefixFrom(std::size_t boundary);

  void WalkLines(LineSpan lines, const ComputeOptions& options, const BracketTable& table,
                 SuppressionCursor& suppression, std::size_t begin, std::size_t end,
                 std::size_t& event_index, WalkState& state, std::vector<FoldRange>* out,
                 std::size_t emit_opener_limit, WorkBudget& budget);
  void ApplyBlockWord(const Block& block, std::size_t block_start, WalkState& state,
                      std::vector<FoldRange>* out, std::size_t emit_opener_limit) const;
  void FlushIndentsAtEof(WalkState& state, std::vector<FoldRange>* out,
                         std::size_t emit_opener_limit) const;

  void FinishRanges();
  void ShiftCollapsedRanges(const LineEditSpan& edit_span, std::size_t line_count);
  void RevalidateCollapsedInWindow();
  void EnsureCollapsedIndex() const;
  void BumpCollapseRevision();

  std::vector<CachedBracket> line_brackets_;
  std::vector<std::uint32_t> line_bracket_count_;
  std::size_t bracket_cache_valid_through_ = 0;
  std::vector<std::pair<char, char>> bracket_cache_pairs_;
  std::vector<CachedBracket> bracket_event_scratch_;
  std::vector<std::uint32_t> bracket_count_scratch_;
  std::vector<std::uint32_t> line_indent_;
  std::size_t line_indent_tab_size_ = 0;

  std::vector<Block> blocks_;
  mutable std::vector<std::size_t> block_start_line_;
  mutable bool block_starts_dirty_ = true;

  std::vector<PrefixState> prefix_states_;
  std::vector<StackBracket> prefix_bracket_pool_;
  std::vector<StackIndent> prefix_indent_pool_;

  std::vector<FoldRange> ranges_;
  std::vector<FoldRange> range_scratch_;
  std::vector<std::size_t> range_closer_prefix_max_;
  std::size_t resolved_first_line_ = 0;
  std::size_t resolved_last_line_ = 0;
  bool resolved_ = false;

  std::vector<FoldRange> collapsed_;
  mutable std::vector<std::size_t> collapsed_hi_prefix_max_;
  mutable bool collapsed_index_dirty_ = true;

  WalkState walk_;
  std::vector<StackBracket> build_bracket_stack_;
  std::vector<StackIndent> build_indent_stack_;
  std::vector<WordCloser> build_bracket_closers_;
  std::vector<WordOpener> build_bracket_openers_;
  std::vector<WordDedent> build_indent_dedents_;
  std::vector<WordIndent> build_indent_openers_;
  std::vector<std::size_t> suppression_lines_scratch_;

  Fingerprint fingerprint_;
  std::size_t document_line_count_ = 0;
  std::size_t budget_multiplier_ = 1;
  bool indent_source_enabled_ = true;
  bool complete_ = true;
  bool dirty_ = true;
  std::size_t revision_ = 0;
  std::size_t layout_revision_ = 0;
};

}
