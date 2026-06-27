#include "editor/HighlightPrefetchService.h"

#include <cstdint>
#include <string>
#include <utility>

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
