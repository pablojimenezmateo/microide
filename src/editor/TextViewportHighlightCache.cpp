// Highlight cache for TextViewport. Holds per-line token caches, end-of-line
// SyntaxState snapshots, and the checkpoint chain that lets us resume
// highlighting after a jump without replaying from line 0. Split out of
// TextViewport.cpp so the cache surface can be inspected without the rest of
// the editor core.
//
// These methods are still members of the `TextViewport` class — see
// editor/TextViewport.h for the declarations. The checkpoint spacing constant
// `kHighlightCheckpointInterval` is shared with TextViewport.cpp via
// editor/TextViewportInternal.h so InvalidateDerivedCaches stays in lockstep.

#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

#include "editor/SyntaxHighlighter.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {

namespace {

constexpr std::size_t kHighlightCacheLimit = 256;

bool IsCachedHighlightState(const SyntaxState& state) {
  return state.definition_id != 0;
}

}  // namespace

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

  if (const auto it = highlight_cache_.find(line_index); it != highlight_cache_.end()) {
    util::PerformanceTrace::Scope hit_scope("TextViewport::HighlightedLineTokens::CacheHit");
    ++highlight_hits_;
    return it->second;
  }

  util::PerformanceTrace::Scope miss_scope("TextViewport::HighlightedLineTokens::CacheMiss");
  util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheForcedMisses);
  const SyntaxState previous_state = HighlightStateBeforeLine(line_index);
  HighlightedLine highlighted;
  {
    util::PerformanceTrace::Scope highlight_scope(
        "TextViewport::HighlightedLineTokens::HighlightLine");
    highlighted = SyntaxHighlighter::HighlightLine(document_->lines[line_index], document_->path,
                                                   previous_state);
  }
  line_highlight_states_[line_index] = highlighted.end_state;
  if (line_index >= line_highlight_states_valid_through_) {
    line_highlight_states_valid_through_ = line_index + 1;
  }

  if (highlight_cache_.size() >= kHighlightCacheLimit) {
    highlight_cache_.erase(highlight_cache_order_.front());
    highlight_cache_order_.pop_front();
    util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
  }
  auto [it, _] = highlight_cache_.emplace(line_index, highlighted.tokens);
  highlight_cache_order_.push_back(line_index);
  return it->second;
}

std::span<const SyntaxTokenKind> TextViewport::HighlightedLineTokensIfCached(
    std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return {};
  }
  if (!syntax_highlighting_enabled()) {
    return {};
  }
  const auto it = highlight_cache_.find(line_index);
  if (it == highlight_cache_.end()) {
    return {};
  }
  return std::span<const SyntaxTokenKind>(it->second.data(), it->second.size());
}

void TextViewport::EnsureInitialHighlightState() const {
  std::string perf_label = "TextViewport::EnsureInitialHighlightState";
  if (util::PerformanceTrace::Enabled() && !document_->path.empty()) {
    perf_label += "(path=" + document_->path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value()) {
    return;
  }
  initial_highlight_state_ = SyntaxHighlighter::InitialState(document_->path, document_->lines);
}

void TextViewport::EnsureHighlightCaches() const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCaches");
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }

  EnsureInitialHighlightState();
  if (highlight_state_content_revision_ != document_->content_revision ||
      highlight_state_syntax_revision_ != document_->syntax_revision) {
    // Lazy invalidation: leave the existing buffer in place and just reset
    // the validity cursors. Readers gate on the cursor (see
    // CachedHighlightStateAt / CachedHighlightCheckpointAt). The previous
    // implementation called `assign(size, SyntaxState{})` here, which was a
    // full O(N) wipe on every layout-revision bump.
    line_highlight_states_valid_through_ = 0;
    highlight_checkpoints_valid_through_ = 0;
    highlight_state_content_revision_ = document_->content_revision;
    highlight_state_syntax_revision_ = document_->syntax_revision;
  }
  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.resize(document_->lines.size());
    line_highlight_states_valid_through_ =
        std::min(line_highlight_states_valid_through_, line_highlight_states_.size());
  }
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
  const std::size_t target_line =
      std::min(document_->lines.size(), checkpoint_index * detail::kHighlightCheckpointInterval);
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
        state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
      }
      line_highlight_states_[line] = state;
      if (line >= line_highlight_states_valid_through_) {
        line_highlight_states_valid_through_ = line + 1;
      }
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

bool TextViewport::HasHighlightPrefetchGap(std::size_t start_line, std::size_t count) const {
  if (!syntax_highlighting_enabled() || document_->lines.empty() || count == 0) {
    return false;
  }
  const std::size_t end = std::min(document_->lines.size(), start_line + count);
  for (std::size_t line = start_line; line < end; ++line) {
    if (highlight_cache_.find(line) == highlight_cache_.end()) {
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
  // Resume state for the first snapshot line, computed against the (main-thread)
  // checkpoint chain so the worker need not replay from line 0.
  request.start_state = HighlightStateBeforeLine(start_line);
  const std::size_t end = std::min(document_->lines.size(), start_line + count);
  request.lines.reserve(end - start_line);
  for (std::size_t line = start_line; line < end; ++line) {
    request.lines.push_back(document_->lines[line]);
  }
  return request;
}

void TextViewport::InstallPrefetchedHighlights(const HighlightPrefetchResult& result) {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }
  // Discard stale results: the document changed (or syntax config changed)
  // since the snapshot was taken, so the precomputed tokens no longer apply.
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
    // content_revision bumps on any edit, so a matching revision means every
    // snapshot line is still current; just fold the precomputed data in.
    if (offset < result.end_states.size() && line < line_highlight_states_.size()) {
      line_highlight_states_[line] = result.end_states[offset];
      if (line >= line_highlight_states_valid_through_) {
        line_highlight_states_valid_through_ = line + 1;
      }
    }
    // Single hash lookup: emplace reports whether the line was already cached,
    // so we skip the separate find() probe.
    auto [it, inserted] = highlight_cache_.emplace(line, result.tokens[offset]);
    if (!inserted) {
      continue;
    }
    highlight_cache_order_.push_back(line);
    if (highlight_cache_.size() > kHighlightCacheLimit) {
      highlight_cache_.erase(highlight_cache_order_.front());
      highlight_cache_order_.pop_front();
      util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
    }
  }
}

SyntaxState TextViewport::HighlightStateBeforeLine(std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightStateBeforeLine");
  EnsureHighlightCaches();
  if (line_index == 0) {
    return *initial_highlight_state_;
  }

  const std::size_t checkpoint_index = line_index / detail::kHighlightCheckpointInterval;
  EnsureHighlightCheckpoint(checkpoint_index);
  const std::size_t checkpoint_line = checkpoint_index * detail::kHighlightCheckpointInterval;
  SyntaxState state = checkpoint_index == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[checkpoint_index];

  util::PerformanceTrace::Scope replay_scope("TextViewport::HighlightStateBeforeLine::Replay");
  for (std::size_t line = checkpoint_line; line < line_index; ++line) {
    if (line < line_highlight_states_valid_through_ &&
        IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
      continue;
    }
    {
      util::PerformanceTrace::Scope advance_scope(
          "TextViewport::HighlightStateBeforeLine::AdvanceState");
      state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
    }
    line_highlight_states_[line] = state;
    if (line >= line_highlight_states_valid_through_) {
      line_highlight_states_valid_through_ = line + 1;
    }
    ++highlight_state_advances_;
  }
  return state;
}

}  // namespace microide::editor
