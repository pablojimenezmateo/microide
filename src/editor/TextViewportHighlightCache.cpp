#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <optional>

#include "editor/SyntaxHighlighter.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {

namespace {

constexpr std::size_t kHighlightCacheLimit = 256;

constexpr std::size_t kMaxSyncHighlightReplayLines = 512;

constexpr std::size_t kCheckpointBackfillChunkLines = 16384;

bool IsCachedHighlightState(const SyntaxState& state) {
  return state.definition_id != 0;
}

std::string_view TokenizableLineView(const TextBuffer& lines, std::size_t line_index) {
  if (lines.LineLength(line_index) > runtime_syntax::kMaxHighlightLineBytes) {
    return {};
  }
  return lines.LineView(line_index);
}

}

void TextViewport::DropHighlightTokenCache() const {
  ++highlight_cache_generation_;
}

void TextViewport::StaleHighlightTokensFrom(std::size_t start_line) const {
  if (start_line == 0) {
    DropHighlightTokenCache();
    return;
  }
  for (auto& [line, entry] : highlight_cache_) {
    if (line >= start_line) {
      entry.generation = entry.generation == highlight_cache_generation_
                             ? (highlight_cache_generation_ | kStaleByLastEdit)
                             : 0;
    }
  }
}

std::optional<std::span<const SyntaxTokenKind>> TextViewport::CurrentHighlightedLineTokens(
    std::size_t line_index) const {
  if (document_ == nullptr || line_index >= document_->lines.size()) {
    return std::nullopt;
  }
  if (!syntax_highlighting_enabled()) {
    return std::span<const SyntaxTokenKind>{};
  }
  const HighlightCacheEntry* entry = CurrentHighlightCacheEntry(line_index);
  if (entry == nullptr) {
    return std::nullopt;
  }
  return std::span<const SyntaxTokenKind>(entry->tokens.data(), entry->tokens.size());
}

void TextViewport::SettleHighlightAfterLastEdit() const {
  if (!edit_carry_before_.has_value() || edit_carry_before_->settled || document_ == nullptr) {
    return;
  }
  EditCarry& carry = *edit_carry_before_;
  carry.settled = true;
  if (carry.content_revision != document_->content_revision ||
      highlight_state_syntax_revision_ != document_->syntax_revision) {
    return;
  }
  if (LastEditChangedCarriedSyntaxState(carry.line)) {
    return;
  }
  const std::size_t states_before = std::min(carry.states_valid_through_before, line_highlight_states_.size());
  if (line_highlight_states_valid_through_ == carry.line + 1 && states_before > line_highlight_states_valid_through_) {
    line_highlight_states_valid_through_ = states_before;
  }
  const std::size_t interval = detail::kHighlightCheckpointInterval;
  const std::size_t first_inside = carry.first_line / interval + 1;
  const bool checkpoint_inside_edit = first_inside * interval <= carry.line;
  const std::size_t checkpoints_before =
      std::min(carry.checkpoints_valid_through_before, highlight_checkpoints_.size());
  if (!checkpoint_inside_edit && checkpoints_before > highlight_checkpoints_valid_through_) {
    highlight_checkpoints_valid_through_ = checkpoints_before;
  }
  const std::uint64_t staled_by_edit = highlight_cache_generation_ | kStaleByLastEdit;
  for (auto& [line, entry] : highlight_cache_) {
    if (line > carry.line && entry.generation == staled_by_edit) {
      entry.generation = highlight_cache_generation_;
    }
  }
}

const TextViewport::HighlightCacheEntry* TextViewport::CurrentHighlightCacheEntry(
    std::size_t line_index) const {
  const auto it = highlight_cache_.find(line_index);
  if (it == highlight_cache_.end() || it->second.generation != highlight_cache_generation_) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<SyntaxTokenKind>& TextViewport::HighlightedLineTokens(
    std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightedLineTokens");
  static const std::vector<SyntaxTokenKind> kEmptyTokens;
  if (line_index >= document_->lines.size()) {
    return kEmptyTokens;
  }
  if (!syntax_highlighting_enabled()) {
    return kEmptyTokens;
  }

  ++highlight_queries_;
  EnsureHighlightCaches();

  if (const HighlightCacheEntry* entry = CurrentHighlightCacheEntry(line_index);
      entry != nullptr) {
    util::PerformanceTrace::Scope hit_scope("TextViewport::HighlightedLineTokens::CacheHit");
    ++highlight_hits_;
    return entry->tokens;
  }

  util::PerformanceTrace::Scope miss_scope("TextViewport::HighlightedLineTokens::CacheMiss");
  util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheForcedMisses);
  const SyntaxState previous_state = HighlightStateBeforeLine(line_index);
  const bool exact = last_highlight_state_exact_;
  const bool slot_is_new = highlight_cache_.find(line_index) == highlight_cache_.end();
  HighlightCacheEntry* recycled_slot = nullptr;
  if (slot_is_new && highlight_cache_.size() >= kHighlightCacheLimit &&
      !highlight_cache_order_.empty()) {
    auto node = highlight_cache_.extract(highlight_cache_order_.front());
    highlight_cache_order_.pop_front();
    util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
    if (!node.empty()) {
      node.key() = line_index;
      recycled_slot = &highlight_cache_.insert(std::move(node)).position->second;
      util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheNodesRecycled);
    }
  }
  HighlightCacheEntry& slot =
      recycled_slot != nullptr ? *recycled_slot : highlight_cache_[line_index];
  SyntaxState end_state;
  {
    util::PerformanceTrace::Scope highlight_scope(
        "TextViewport::HighlightedLineTokens::HighlightLine");
    end_state = SyntaxHighlighter::HighlightLineInto(
        TokenizableLineView(document_->lines, line_index), document_->path, previous_state,
        &slot.tokens);
  }
  slot.generation = highlight_cache_generation_;
  if (slot_is_new) {
    highlight_cache_order_.push_back(line_index);
  }
  if (exact) {
    StoreLineHighlightState(line_index, end_state);
  }

  return slot.tokens;
}

std::span<const SyntaxTokenKind> TextViewport::HighlightedLineTokensIfCached(
    std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return {};
  }
  if (!syntax_highlighting_enabled()) {
    return {};
  }
  const HighlightCacheEntry* entry = CurrentHighlightCacheEntry(line_index);
  if (entry == nullptr) {
    return {};
  }
  return std::span<const SyntaxTokenKind>(entry->tokens.data(), entry->tokens.size());
}

void TextViewport::AppendCachedHighlightedLines(std::vector<std::size_t>& out) const {
  if (!syntax_highlighting_enabled()) {
    return;
  }
  const std::size_t first = out.size();
  out.reserve(first + highlight_cache_.size());
  for (const auto& [line, entry] : highlight_cache_) {
    if (entry.generation == highlight_cache_generation_) {
      out.push_back(line);
    }
  }
  std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
}

void TextViewport::EnsureInitialHighlightState() const {
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value() && highlight_state_syntax_revision_ == document_->syntax_revision) {
    return;
  }
  util::PerformanceTrace::ScopeLabel perf_label("TextViewport::EnsureInitialHighlightState");
  if (!document_->path.empty()) {
    perf_label.Field("path", document_->path);
  }
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  initial_highlight_state_ =
      document_->language_override.empty()
          ? SyntaxHighlighter::InitialState(document_->path, document_->lines)
          : runtime_syntax::StateForFiletype(document_->language_override);
}

void TextViewport::StoreLineHighlightState(std::size_t line, const SyntaxState& state) const {
  if (line < line_highlight_states_valid_through_) {
    line_highlight_states_[line] = state;
    return;
  }
  if (line != line_highlight_states_valid_through_) {
    return;
  }
  if (line < line_highlight_states_.size()) {
    line_highlight_states_[line] = state;
  } else {
    if (line_highlight_states_.capacity() == 0) {
      constexpr std::size_t kInitialReserveLines = 64 * 1024;
      line_highlight_states_.reserve(std::min(document_->lines.size(), kInitialReserveLines));
    }
    line_highlight_states_.push_back(state);
  }
  line_highlight_states_valid_through_ = line + 1;
}

void TextViewport::EnsureHighlightCaches() const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCaches");
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }

  EnsureInitialHighlightState();
  if (highlight_state_content_revision_ != document_->content_revision ||
      highlight_state_syntax_revision_ != document_->syntax_revision) {
    line_highlight_states_valid_through_ = 0;
    highlight_checkpoints_valid_through_ = 0;
    pending_checkpoint_backfill_target_line_ = 0;
    highlight_state_content_revision_ = document_->content_revision;
    highlight_state_syntax_revision_ = document_->syntax_revision;
    DropHighlightTokenCache();
  }
  line_highlight_states_valid_through_ =
      std::min(line_highlight_states_valid_through_, document_->lines.size());
  const std::size_t checkpoint_count =
      ((document_->lines.size() - 1) / detail::kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.resize(checkpoint_count);
    highlight_checkpoints_valid_through_ =
        std::min(highlight_checkpoints_valid_through_, highlight_checkpoints_.size());
  }
  if (!highlight_checkpoints_.empty()) {
    highlight_checkpoints_.front() = *initial_highlight_state_;
    if (highlight_checkpoints_valid_through_ < 1) {
      highlight_checkpoints_valid_through_ = 1;
    }
  }
}

void TextViewport::EnsureHighlightCheckpoint(std::size_t checkpoint_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCheckpoint");
  EnsureHighlightCaches();
  if (!syntax_highlighting_enabled() || document_->lines.empty() ||
      checkpoint_index >= highlight_checkpoints_.size()) {
    return;
  }
  if (checkpoint_index < highlight_checkpoints_valid_through_ &&
      IsCachedHighlightState(highlight_checkpoints_[checkpoint_index])) {
    return;
  }

  std::size_t previous_checkpoint = checkpoint_index;
  while (previous_checkpoint > 0 &&
         (previous_checkpoint >= highlight_checkpoints_valid_through_ ||
          !IsCachedHighlightState(highlight_checkpoints_[previous_checkpoint]))) {
    --previous_checkpoint;
  }

  SyntaxState state = previous_checkpoint == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[previous_checkpoint];
  std::size_t line = previous_checkpoint * detail::kHighlightCheckpointInterval;
  std::size_t target_line =
      std::min(document_->lines.size(), checkpoint_index * detail::kHighlightCheckpointInterval);
  if (target_line - line > kMaxSyncHighlightReplayLines) {
    target_line = line + kMaxSyncHighlightReplayLines;
    pending_checkpoint_backfill_target_line_ =
        std::max(pending_checkpoint_backfill_target_line_,
                 std::min(document_->lines.size(),
                          checkpoint_index * detail::kHighlightCheckpointInterval));
  }
  util::PerformanceTrace::Scope replay_scope(
      "TextViewport::EnsureHighlightCheckpoint::ReplayToCheckpoint");
  for (; line < target_line; ++line) {
    if (line < line_highlight_states_valid_through_ &&
        IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
    } else {
      {
        util::PerformanceTrace::Scope advance_scope(
            "TextViewport::EnsureHighlightCheckpoint::AdvanceState");
        state = SyntaxHighlighter::AdvanceState(TokenizableLineView(document_->lines, line), document_->path, state);
      }
      StoreLineHighlightState(line, state);
      ++highlight_checkpoint_advances_;
    }
    const std::size_t next_line = line + 1;
    if (next_line < document_->lines.size() &&
        next_line % detail::kHighlightCheckpointInterval == 0) {
      const std::size_t cp_idx = next_line / detail::kHighlightCheckpointInterval;
      highlight_checkpoints_[cp_idx] = state;
      if (cp_idx >= highlight_checkpoints_valid_through_) {
        highlight_checkpoints_valid_through_ = cp_idx + 1;
      }
    }
  }
}

std::size_t TextViewport::HighlightCacheCapacity() {
  return kHighlightCacheLimit;
}

bool TextViewport::HasHighlightPrefetchGap(std::size_t start_line, std::size_t count) const {
  if (!syntax_highlighting_enabled() || document_->lines.empty() || count == 0) {
    return false;
  }
  const std::size_t end = std::min(document_->lines.size(), start_line + count);
  for (std::size_t line = start_line; line < end; ++line) {
    if (CurrentHighlightCacheEntry(line) == nullptr) {
      return true;
    }
  }
  return false;
}

HighlightPrefetchRequest TextViewport::BuildHighlightPrefetchRequest(std::size_t start_line,
                                                                     std::size_t count) const {
  HighlightPrefetchRequest request;
  request.viewport = this;
  request.path = document_->path;
  request.content_revision = document_->content_revision;
  request.syntax_revision = document_->syntax_revision;
  request.start_line = start_line;
  if (!syntax_highlighting_enabled() || document_->lines.empty() ||
      start_line >= document_->lines.size() || count == 0) {
    return request;
  }
  request.start_state = HighlightStateBeforeLine(start_line);
  const std::size_t end = std::min(document_->lines.size(), start_line + count);
  request.lines = document_->lines.SliceLines(start_line, end);
  return request;
}

void TextViewport::InstallPrefetchedHighlights(HighlightPrefetchResult result) {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }
  if (result.content_revision != document_->content_revision ||
      result.syntax_revision != document_->syntax_revision) {
    return;
  }
  EnsureHighlightCaches();
  const std::size_t line_count = result.tokens.size();
  for (std::size_t offset = 0; offset < line_count; ++offset) {
    const std::size_t line = result.start_line + offset;
    if (line >= document_->lines.size()) {
      break;
    }
    if (offset < result.end_states.size()) {
      StoreLineHighlightState(line, result.end_states[offset]);
    }
    if (CurrentHighlightCacheEntry(line) != nullptr) {
      continue;
    }
    const bool slot_is_new = highlight_cache_.find(line) == highlight_cache_.end();
    HighlightCacheEntry* recycled_slot = nullptr;
    if (slot_is_new && highlight_cache_.size() >= kHighlightCacheLimit &&
        !highlight_cache_order_.empty()) {
      auto node = highlight_cache_.extract(highlight_cache_order_.front());
      highlight_cache_order_.pop_front();
      util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
      if (!node.empty()) {
        node.key() = line;
        recycled_slot = &highlight_cache_.insert(std::move(node)).position->second;
        util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheNodesRecycled);
      }
    }
    HighlightCacheEntry& slot =
        recycled_slot != nullptr ? *recycled_slot : highlight_cache_[line];
    slot.tokens = std::move(result.tokens[offset]);
    slot.generation = highlight_cache_generation_;
    if (slot_is_new) {
      highlight_cache_order_.push_back(line);
    }
  }
}

bool TextViewport::LastEditChangedCarriedSyntaxState(std::size_t line) const {
  if (document_ == nullptr || !syntax_highlighting_enabled() || line + 1 >= document_->lines.size()) {
    return false;
  }
  if (!edit_carry_before_.has_value() || edit_carry_before_->line != line) {
    return true;
  }
  (void)HighlightedLineTokens(line);
  if (line >= line_highlight_states_valid_through_) {
    return true;
  }
  return line_highlight_states_[line] != edit_carry_before_->before;
}

SyntaxState TextViewport::HighlightStateBeforeLine(std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightStateBeforeLine");
  EnsureHighlightCaches();
  last_highlight_state_exact_ = true;
  if (line_index == 0) {
    return *initial_highlight_state_;
  }

  const std::size_t checkpoint_index = line_index / detail::kHighlightCheckpointInterval;

  auto deepest_valid_checkpoint = [&]() {
    std::size_t cp = std::min(checkpoint_index, highlight_checkpoints_valid_through_ == 0
                                                    ? std::size_t{0}
                                                    : highlight_checkpoints_valid_through_ - 1);
    while (cp > 0 && !IsCachedHighlightState(highlight_checkpoints_[cp])) {
      --cp;
    }
    return cp;
  };

  if (line_index - deepest_valid_checkpoint() * detail::kHighlightCheckpointInterval >
      kMaxSyncHighlightReplayLines) {
    pending_checkpoint_backfill_target_line_ =
        std::max(pending_checkpoint_backfill_target_line_, line_index);
    last_highlight_state_exact_ = false;
    const std::size_t cp = deepest_valid_checkpoint();
    return cp == 0 ? *initial_highlight_state_ : highlight_checkpoints_[cp];
  }

  EnsureHighlightCheckpoint(checkpoint_index);
  std::size_t usable_checkpoint = deepest_valid_checkpoint();
  std::size_t line = usable_checkpoint * detail::kHighlightCheckpointInterval;
  SyntaxState state = usable_checkpoint == 0 ? *initial_highlight_state_
                                             : highlight_checkpoints_[usable_checkpoint];

  util::PerformanceTrace::Scope replay_scope("TextViewport::HighlightStateBeforeLine::Replay");
  for (; line < line_index; ++line) {
    if (line < line_highlight_states_valid_through_ &&
        IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
      continue;
    }
    {
      util::PerformanceTrace::Scope advance_scope(
          "TextViewport::HighlightStateBeforeLine::AdvanceState");
      state = SyntaxHighlighter::AdvanceState(TokenizableLineView(document_->lines, line), document_->path, state);
    }
    StoreLineHighlightState(line, state);
    ++highlight_state_advances_;
  }
  return state;
}

std::optional<HighlightCheckpointRequest> TextViewport::TakeHighlightCheckpointBackfillRequest()
    const {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    pending_checkpoint_backfill_target_line_ = 0;
    return std::nullopt;
  }
  if (pending_checkpoint_backfill_target_line_ == 0) {
    return std::nullopt;
  }
  EnsureHighlightCaches();
  const std::size_t interval = detail::kHighlightCheckpointInterval;
  const std::size_t target_line =
      std::min(pending_checkpoint_backfill_target_line_, document_->lines.size());

  std::size_t checkpoint =
      highlight_checkpoints_valid_through_ == 0 ? 0 : highlight_checkpoints_valid_through_ - 1;
  while (checkpoint > 0 && !IsCachedHighlightState(highlight_checkpoints_[checkpoint])) {
    --checkpoint;
  }
  const std::size_t first_line = checkpoint * interval;
  if (first_line >= target_line) {
    pending_checkpoint_backfill_target_line_ = 0;
    return std::nullopt;
  }
  const std::size_t end_line =
      std::min(target_line, first_line + kCheckpointBackfillChunkLines);

  HighlightCheckpointRequest request;
  request.viewport = this;
  request.path = document_->path;
  request.content_revision = document_->content_revision;
  request.syntax_revision = document_->syntax_revision;
  request.first_line = first_line;
  request.start_state =
      checkpoint == 0 ? *initial_highlight_state_ : highlight_checkpoints_[checkpoint];
  request.checkpoint_interval = interval;
  request.lines = document_->lines.SliceLines(first_line, end_line);
  pending_checkpoint_backfill_target_line_ = 0;
  return request;
}

void TextViewport::InstallHighlightCheckpoints(const HighlightCheckpointResult& result) {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }
  if (result.content_revision != document_->content_revision ||
      result.syntax_revision != document_->syntax_revision) {
    return;
  }
  EnsureHighlightCaches();
  const std::size_t previous_valid_through = highlight_checkpoints_valid_through_;
  for (std::size_t offset = 0; offset < result.checkpoint_states.size(); ++offset) {
    const std::size_t idx = result.first_checkpoint_index + offset;
    if (idx >= highlight_checkpoints_.size()) {
      break;
    }
    highlight_checkpoints_[idx] = result.checkpoint_states[offset];
    if (idx == highlight_checkpoints_valid_through_) {
      highlight_checkpoints_valid_through_ = idx + 1;
    }
  }
  if (highlight_checkpoints_valid_through_ != previous_valid_through) {
    DropHighlightTokenCache();
  }
}

TextViewportDerivedCacheBytes TextViewport::DerivedCacheBytes() const {
  TextViewportDerivedCacheBytes out;
  out.layout_cache = layout_cache_.ApproximateResidentBytes();

  out.highlight_tokens = highlight_cache_.bucket_count() * sizeof(void*);
  for (const auto& [line, entry] : highlight_cache_) {
    out.highlight_tokens +=
        sizeof(std::size_t) + sizeof(HighlightCacheEntry) + sizeof(void*) * 2;
    out.highlight_tokens += entry.tokens.capacity() * sizeof(SyntaxTokenKind);
  }
  out.highlight_tokens += ((highlight_cache_order_.size() * sizeof(std::size_t) + 511) / 512) * 512;

  out.highlight_states = line_highlight_states_.capacity() * sizeof(SyntaxState);
  out.highlight_states += highlight_checkpoints_.capacity() * sizeof(SyntaxState);

  out.caret_caches = secondary_carets_.capacity() * sizeof(SecondaryCaret);
  out.caret_caches += secondary_caret_positions_cache_.capacity() * sizeof(TextPosition);
  out.caret_caches += box_ranges_scratch_.capacity() * sizeof(SelectionRange);
  out.caret_caches += secondary_caret_candidates_scratch_.capacity() * sizeof(SecondaryCaret);

  out.undo_history = document_->undo_history.ApproximateResidentBytes();
  return out;
}

}
