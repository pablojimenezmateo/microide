#pragma once

#include <cstdint>
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
  // Drains + joins the worker before the result queues / wake callback are
  // destroyed, so a posted job that reads results_/wake_ through `this` can never
  // outlive them even if an owner forgets to call Shutdown(). Shutdown() is
  // idempotent (SerialWorkQueue::Shutdown early-returns once stopped), so an
  // explicit prior Shutdown() plus this destructor is safe.
  ~HighlightPrefetchService() { Shutdown(); }
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

  // Compiles a cold filetype's rule regexes on the worker so the first visible-
  // line highlight does not pay that cost on the render path. Best-effort: at
  // most one prewarm is pending (PostLatest with a single key), and any
  // superseded filetype is still compiled by its own normal prefetch. Produces
  // no result to drain; the compile is published into the shared registry.
  void PrewarmDefinition(std::uint32_t definition_id);

  // Gated prewarm for the active editor viewport. `viewport_token` is an identity
  // token only (never dereferenced). Filetype detection (`resolve_definition_id`)
  // and the prewarm run only when the token changes — i.e. once per tab switch,
  // not every settled frame — so the per-frame caller pays nothing while the
  // active tab is unchanged. Owns the last-viewport gate so callers need no state.
  void PrewarmForViewport(const void* viewport_token,
                          const std::function<std::uint32_t()>& resolve_definition_id);

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
  // Identity token of the viewport last prewarmed; see PrewarmForViewport. Main
  // thread only (set and read from the settled-frame prefetch request). Never
  // dereferenced.
  const void* last_prewarm_viewport_ = nullptr;
};

}  // namespace microide::editor
