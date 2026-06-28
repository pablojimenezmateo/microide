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
#include <optional>

#include "editor/SyntaxHighlighter.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {

namespace {

constexpr std::size_t kHighlightCacheLimit = 256;

// Upper bound on how many lines a *synchronous* highlight-state replay may
// advance in one call. A deep cold jump (session restore scrolled deep into a
// large file) previously replayed from the nearest valid checkpoint all the way
// to the visible line on the main thread — hundreds of ms to seconds. We cap the
// synchronous work to keep the frame interactive; the remainder is filled
// off-thread via the checkpoint backfill. Sized comfortably above a screenful
// (and the look-ahead prefetch window) so ordinary scrolling never trips the
// backfill path, but far below a large file so a deep jump defers, not freezes.
constexpr std::size_t kMaxSyncHighlightReplayLines = 512;

// Max lines copied into a single off-thread checkpoint-backfill request. Deep
// gaps converge across several repaints (each install lets the next synchronous
// replay resume deeper), bounding both the per-request copy and worker burst.
constexpr std::size_t kCheckpointBackfillChunkLines = 16384;

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
  const bool exact = last_highlight_state_exact_;
  HighlightedLine highlighted;
  {
    util::PerformanceTrace::Scope highlight_scope(
        "TextViewport::HighlightedLineTokens::HighlightLine");
    highlighted = SyntaxHighlighter::HighlightLine(document_->lines.LineView(line_index), document_->path,
                                                   previous_state);
  }
  // Only record the per-line end state (and advance the validity frontier) when
  // the resume state was exact. An approximate deep-jump result must not be
  // promoted to authoritative; the off-thread backfill makes a later repaint
  // exact (and clears this token-cache entry so it is recomputed).
  if (exact) {
    line_highlight_states_[line_index] = highlighted.end_state;
    if (line_index >= line_highlight_states_valid_through_) {
      line_highlight_states_valid_through_ = line_index + 1;
    }
  }

  if (highlight_cache_.size() >= kHighlightCacheLimit) {
    highlight_cache_.erase(highlight_cache_order_.front());
    highlight_cache_order_.pop_front();
    util::AddPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
  }
  auto [it, _] = highlight_cache_.emplace(line_index, std::move(highlighted.tokens));
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
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value()) {
    return;
  }
  // Build the trace label only when tracing is on and only past the early-outs
  // above -- this is reached from every EnsureHighlightCaches, so an
  // unconditional std::string here was a heap allocation on every highlight
  // query.
  std::string perf_label;
  std::string_view perf_label_view = "TextViewport::EnsureInitialHighlightState";
  if (util::PerformanceTrace::Enabled() && !document_->path.empty()) {
    perf_label = "TextViewport::EnsureInitialHighlightState(path=" + document_->path.string() + ")";
    perf_label_view = perf_label;
  }
  util::PerformanceTrace::Scope perf_scope(perf_label_view);
  // Pass the live buffer directly: InitialState only inspects a bounded head, so
  // this never materializes the whole document (the previous Snapshot() copied
  // the entire file -- megabytes, one alloc per line -- just to read 64 lines,
  // and left that copy resident in the buffer's snapshot cache).
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
    pending_checkpoint_backfill_target_line_ = 0;
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
  std::size_t target_line =
      std::min(document_->lines.size(), checkpoint_index * detail::kHighlightCheckpointInterval);
  // Bound the synchronous replay so a deep cold jump never blocks the frame. If
  // we cap short, the requested checkpoint stays invalid; the caller resumes
  // from the deepest checkpoint we did reach and records a backfill target.
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
        state = SyntaxHighlighter::AdvanceState(document_->lines.LineView(line), document_->path, state);
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
  // Copy once via SliceLines (matching the checkpoint-backfill path) instead of
  // operator[] per line, which materializes each string into TextBuffer's
  // line_cache_ and then copies it again into the request.
  request.lines = document_->lines.SliceLines(start_line, end);
  return request;
}

void TextViewport::InstallPrefetchedHighlights(HighlightPrefetchResult result) {
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
    auto [it, inserted] = highlight_cache_.emplace(line, std::move(result.tokens[offset]));
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
  last_highlight_state_exact_ = true;
  if (line_index == 0) {
    return *initial_highlight_state_;
  }

  const std::size_t checkpoint_index = line_index / detail::kHighlightCheckpointInterval;

  // Find the deepest already-valid checkpoint at/below the target WITHOUT forcing
  // a replay. This is the cheap resume point and the gate for the deep-jump case.
  auto deepest_valid_checkpoint = [&]() {
    std::size_t cp = std::min(checkpoint_index, highlight_checkpoints_valid_through_ == 0
                                                    ? std::size_t{0}
                                                    : highlight_checkpoints_valid_through_ - 1);
    while (cp > 0 && !IsCachedHighlightState(highlight_checkpoints_[cp])) {
      --cp;
    }
    return cp;
  };

  // Deep cold jump: the exact state is more than one cap away from any valid
  // checkpoint. Do NOT replay (replaying a capped chunk here is wasted work that
  // every visible line would repeat). Return the nearest checkpoint's state as an
  // approximation, mark it inexact, and arm the off-thread backfill. A later
  // repaint, after the chain catches up, takes the exact path below.
  if (line_index - deepest_valid_checkpoint() * detail::kHighlightCheckpointInterval >
      kMaxSyncHighlightReplayLines) {
    pending_checkpoint_backfill_target_line_ =
        std::max(pending_checkpoint_backfill_target_line_, line_index);
    last_highlight_state_exact_ = false;
    const std::size_t cp = deepest_valid_checkpoint();
    return cp == 0 ? *initial_highlight_state_ : highlight_checkpoints_[cp];
  }

  // Exact path: build the checkpoint just below line_index (a bounded replay,
  // since the gap is within the cap), then replay the short remainder.
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
      state = SyntaxHighlighter::AdvanceState(document_->lines.LineView(line), document_->path, state);
    }
    line_highlight_states_[line] = state;
    if (line >= line_highlight_states_valid_through_) {
      line_highlight_states_valid_through_ = line + 1;
    }
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

  // Resume from the deepest valid checkpoint so the install folds in contiguously.
  std::size_t checkpoint =
      highlight_checkpoints_valid_through_ == 0 ? 0 : highlight_checkpoints_valid_through_ - 1;
  while (checkpoint > 0 && !IsCachedHighlightState(highlight_checkpoints_[checkpoint])) {
    --checkpoint;
  }
  const std::size_t first_line = checkpoint * interval;
  if (first_line >= target_line) {
    // The chain already reached the target (e.g. a prior chunk caught us up).
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
  // Clear the pending flag; if this chunk does not reach the target, the next
  // repaint's bounded replay re-detects the shortfall and re-arms it.
  pending_checkpoint_backfill_target_line_ = 0;
  return request;
}

void TextViewport::InstallHighlightCheckpoints(const HighlightCheckpointResult& result) {
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }
  // Drop stale results: the document or syntax config moved on since the snapshot.
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
    // valid_through is a contiguous-from-front cursor: only extend it when this
    // index sits exactly at the frontier (the request resumed from the deepest
    // valid checkpoint, so the common case advances it contiguously).
    if (idx == highlight_checkpoints_valid_through_) {
      highlight_checkpoints_valid_through_ = idx + 1;
    }
  }
  // If the chain advanced, drop the (small, LRU-bounded) token cache so any
  // approximate deep-jump tokens are recomputed from the now-exact resume state
  // on the next repaint. Cheap (<= kHighlightCacheLimit entries) and only happens
  // while a deep-jump backfill is converging.
  if (highlight_checkpoints_valid_through_ != previous_valid_through) {
    highlight_cache_.clear();
    highlight_cache_order_.clear();
  }
}

}  // namespace microide::editor
