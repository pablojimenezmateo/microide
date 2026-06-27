#pragma once

#include <functional>
#include <mutex>
#include <vector>

#include "editor/HighlightPrefetch.h"
#include "project/ProjectBackgroundExecutor.h"

namespace microide::editor {

// Owns a dedicated background worker that tokenizes HighlightPrefetchRequests
// off the main thread and queues the results for the main thread to install.
// Keeps zero shared mutable TextViewport state across threads: the worker only
// touches the immutable request snapshot and the shared (read-locked) syntax
// registry, and hands results back through a mutex-guarded queue.
class HighlightPrefetchService {
 public:
  // Invoked on the worker thread after a result is queued, to nudge the main
  // loop to drain. Must be cheap and thread-safe (e.g. SDL_PushEvent). Set once
  // at startup, before any Request, so the worker reads it race-free.
  void SetWakeCallback(std::function<void()> wake);

  // Queues a prefetch request. Requests are deduplicated per originating
  // viewport, so rapid scroll/edit supersedes an unstarted request.
  void Request(HighlightPrefetchRequest request);

  // Queues an off-thread checkpoint-chain backfill (deep first-paint catch-up).
  // Deduplicated per originating viewport like Request.
  void RequestCheckpoints(HighlightCheckpointRequest request);

  // Main thread: removes and returns all completed results.
  std::vector<HighlightPrefetchResult> DrainResults();

  // Main thread: removes and returns all completed checkpoint-backfill results.
  std::vector<HighlightCheckpointResult> DrainCheckpointResults();

  // Cancels queued work and joins the worker thread.
  void Shutdown();

 private:
  project::ProjectBackgroundExecutor executor_;
  std::function<void()> wake_;  // set once before any Request; read on the worker
  std::mutex results_mutex_;
  std::vector<HighlightPrefetchResult> results_;
  std::mutex checkpoint_results_mutex_;
  std::vector<HighlightCheckpointResult> checkpoint_results_;
};

}  // namespace microide::editor
