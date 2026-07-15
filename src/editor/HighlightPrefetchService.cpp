#include "editor/HighlightPrefetchService.h"

#include <cstdint>
#include <string>
#include <utility>

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::editor {

void HighlightPrefetchService::SetWakeCallback(std::function<void()> wake) {
  wake_ = std::move(wake);
}

void HighlightPrefetchService::Request(HighlightPrefetchRequest request) {
  if (request.lines.empty()) {
    return;
  }
  // Dedup by originating viewport so a newer viewport position supersedes an
  // unstarted prefetch for the same buffer.
  std::string key =
      "highlight-prefetch:" + std::to_string(reinterpret_cast<std::uintptr_t>(request.viewport));
  executor_.PostLatest(std::move(key), [this, req = std::move(request)]() mutable {
    HighlightPrefetchResult result = ComputeHighlightPrefetch(req);
    {
      std::lock_guard<std::mutex> lock(results_mutex_);
      results_.push_back(std::move(result));
    }
    if (wake_) {
      wake_();
    }
  });
}

void HighlightPrefetchService::RequestCheckpoints(HighlightCheckpointRequest request) {
  if (request.lines.empty()) {
    return;
  }
  // Dedup by originating viewport: a newer backfill (deeper target or post-edit)
  // supersedes an unstarted one for the same buffer.
  std::string key = "highlight-checkpoints:" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(request.viewport));
  executor_.PostLatest(std::move(key), [this, req = std::move(request)]() mutable {
    HighlightCheckpointResult result = ComputeHighlightCheckpoints(req);
    {
      std::lock_guard<std::mutex> lock(checkpoint_results_mutex_);
      checkpoint_results_.push_back(std::move(result));
    }
    if (wake_) {
      wake_();
    }
  });
}

void HighlightPrefetchService::PrewarmDefinition(std::uint32_t definition_id) {
  if (definition_id == 0) {
    return;
  }
  // Single key: at most one prewarm compile is queued at a time. A rapid series
  // of tab switches only keeps the latest pending; any filetype it supersedes is
  // still compiled by that tab's own visible-band prefetch. The compile is
  // idempotent, so a redundant post is cheap. No result is produced, so no wake
  // is needed.
  executor_.PostLatest("syntax-prewarm",
                       [definition_id]() { runtime_syntax::CompileDefinition(definition_id); });
}

void HighlightPrefetchService::PrewarmForViewport(
    const void* viewport_token, const std::function<std::uint32_t()>& resolve_definition_id) {
  if (viewport_token == nullptr || viewport_token == last_prewarm_viewport_) {
    return;
  }
  last_prewarm_viewport_ = viewport_token;
  PrewarmDefinition(resolve_definition_id());
}

std::vector<HighlightPrefetchResult> HighlightPrefetchService::DrainResults() {
  std::vector<HighlightPrefetchResult> drained;
  std::lock_guard<std::mutex> lock(results_mutex_);
  drained.swap(results_);
  return drained;
}

std::vector<HighlightCheckpointResult> HighlightPrefetchService::DrainCheckpointResults() {
  std::vector<HighlightCheckpointResult> drained;
  std::lock_guard<std::mutex> lock(checkpoint_results_mutex_);
  drained.swap(checkpoint_results_);
  return drained;
}

void HighlightPrefetchService::Shutdown() { executor_.Shutdown(); }

}  // namespace microide::editor
