#include "workspace/services/SaveFormatterService.h"

#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

std::uint64_t SaveFormatterService::Begin(std::string coalesce_key,
                                          Request request,
                                          const platform::ProcessLauncher& launcher,
                                          std::function<void(Completion)> on_complete) {
  if (request.command.empty()) {
    return 0;
  }
  const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
  pending_.fetch_add(1, std::memory_order_acq_rel);

  // Everything the worker touches is copied into the task: the argv, the cwd, the
  // text, and a reference to a launcher that outlives the process. It reads no
  // shell state and no viewport, which is what makes running it off-thread safe at
  // all.
  executor_.Submit(std::move(coalesce_key),
                   [this, id, request = std::move(request), on_complete = std::move(on_complete),
                    &launcher](const util::CancellationToken& token) mutable {
                     Completion completion;
                     completion.id = id;
                     completion.formatter_id = std::move(request.formatter_id);
                     if (token.IsCancellationRequested()) {
                       // A superseded queued run still reports, so the caller's
                       // pending bookkeeping is always cleared by exactly one
                       // completion. `ok=false` with no error text means "dropped";
                       // the caller writes the buffer unformatted.
                       pending_.fetch_sub(1, std::memory_order_acq_rel);
                       mailbox_.Post([completion = std::move(completion),
                                      on_complete = std::move(on_complete)]() mutable {
                         if (on_complete) {
                           on_complete(std::move(completion));
                         }
                       });
                       return;
                     }
                     util::PerformanceTrace::Scope perf_scope("SaveFormatterService::Run");
                     // Non-const: the failure path MOVES stderr out of it. Left
                     // const, the move would bind to a const lvalue and silently
                     // copy the whole captured stream.
                     platform::SubprocessResult result = launcher.Run(
                         request.command, platform::SubprocessOptions{
                                              .cwd = launcher.ResolveWorkingDirectory(request.cwd),
                                              .stdin_text = std::move(request.text),
                                              .environment_overrides = {},
                                              .timeout_ms = request.timeout_ms,
                                          });
                     completion.ok = result.success();
                     completion.timed_out = result.timed_out;
                     if (completion.ok) {
                       completion.formatted_text = result.stdout_text;
                     } else {
                       completion.error_text = std::move(result.stderr_text);
                     }
                     // Decrement BEFORE posting: FlushPendingRuns waits on the
                     // executor and then drains, so a completion must never be
                     // counted as still pending once its task is about to end.
                     pending_.fetch_sub(1, std::memory_order_acq_rel);
                     mailbox_.Post([completion = std::move(completion),
                                    on_complete = std::move(on_complete)]() mutable {
                       if (on_complete) {
                         on_complete(std::move(completion));
                       }
                     });
                   });
  return id;
}

SaveFormatterService::Completion SaveFormatterService::RunBlocking(
    Request request, const platform::ProcessLauncher& launcher) {
  Completion completion;
  completion.formatter_id = request.formatter_id;
  if (request.command.empty()) {
    completion.ok = true;
    return completion;
  }
  util::PerformanceTrace::Scope perf_scope("SaveFormatterService::RunBlocking");
  // No coalesce key: this run is awaited, so superseding it would strand the waiter.
  executor_.Submit([&completion, &request, &launcher](const util::CancellationToken&) {
    // Non-const for the same reason as the deferred path above: stderr is moved.
    platform::SubprocessResult result =
        launcher.Run(request.command, platform::SubprocessOptions{
                                          .cwd = launcher.ResolveWorkingDirectory(request.cwd),
                                          .stdin_text = request.text,
                                          .environment_overrides = {},
                                          .timeout_ms = request.timeout_ms,
                                      });
    completion.ok = result.success();
    completion.timed_out = result.timed_out;
    if (completion.ok) {
      completion.formatted_text = result.stdout_text;
    } else {
      completion.error_text = std::move(result.stderr_text);
    }
  });
  // Waits for THIS task, and for any deferred run already queued ahead of it. Both
  // must finish before the caller writes, or a deferred completion would land on a
  // buffer this save is about to replace.
  //
  // It deliberately does NOT drain the mailbox. A blocking save reaches here with
  // `save_tab_mutex_` held, and a drained deferred completion re-enters the save —
  // which takes that same non-recursive mutex, and deadlocks the shell thread. The
  // queued completions ride the next frame's DrainCompletions instead, where they
  // find their tab's pending id already cleared by this save and drop themselves.
  executor_.WaitForIdle();
  return completion;
}

void SaveFormatterService::FlushPendingRuns() {
  if (PendingCount() == 0 && mailbox_.PendingCount() == 0) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("SaveFormatterService::FlushPendingRuns");
  // WaitForIdle covers both the queued and the running task; the mailbox then holds
  // every completion those tasks posted. Draining after the wait is what turns a
  // deferred save back into a completed one for a caller that cannot proceed
  // without it.
  executor_.WaitForIdle();
  mailbox_.Drain();
}

}  // namespace microide::workspace
