#include "workspace/services/SaveFormatterService.h"

#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

namespace {

void RunSaveStages(SaveFormatterService::Request& request, const platform::ProcessLauncher& launcher,
                   SaveFormatterService::Completion& completion) {
  std::string text = std::move(request.text);
  bool participants_changed = false;
  if (request.participants) {
    util::PerformanceTrace::Scope perf_scope("SaveFormatterService::Participants");
    plugin::PluginHost::SaveParticipantOutcome outcome = request.participants();
    if (!outcome.ok) {
      completion.cancel_save = true;
      completion.error_text = std::move(outcome.error);
      return;
    }
    completion.participants_timed_out = outcome.timed_out;
    participants_changed = outcome.text != text;
    text = std::move(outcome.text);
  }
  if (request.command.empty()) {
    completion.ok = true;
    if (participants_changed) {
      completion.formatted_text = std::move(text);
    }
    return;
  }
  util::PerformanceTrace::Scope perf_scope("SaveFormatterService::Run");
  platform::SubprocessResult result = launcher.Run(
      request.command, platform::SubprocessOptions{
                           .cwd = request.cwd,
                           .stdin_text = participants_changed ? text : std::move(text),
                           .environment_overrides = {},
                           .timeout_ms = request.timeout_ms,
                       });
  completion.ok = result.success();
  completion.timed_out = result.timed_out;
  if (completion.ok) {
    completion.formatted_text = std::move(result.stdout_text);
  } else {
    completion.error_text = std::move(result.stderr_text);
  }
  if (participants_changed && completion.formatted_text.empty()) {
    // NOLINTNEXTLINE(bugprone-use-after-move): stdin copied it when participants_changed.
    completion.formatted_text = std::move(text);
  }
}

}

std::uint64_t SaveFormatterService::Begin(std::string coalesce_key,
                                          Request request,
                                          const platform::ProcessLauncher& launcher,
                                          std::function<void(Completion)> on_complete) {
  if (request.command.empty() && !request.participants) {
    return 0;
  }
  const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
  pending_.fetch_add(1, std::memory_order_acq_rel);

  executor_.Submit(std::move(coalesce_key),
                   [this, id, request = std::move(request), on_complete = std::move(on_complete),
                    &launcher](const util::CancellationToken& token) mutable {
                     Completion completion;
                     completion.id = id;
                     completion.formatter_id = request.formatter_id;
                     if (!token.IsCancellationRequested()) {
                       RunSaveStages(request, launcher, completion);
                     }
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
  executor_.Submit([&completion, &request, &launcher](const util::CancellationToken&) {
    platform::SubprocessResult result =
        launcher.Run(request.command, platform::SubprocessOptions{
                                          .cwd = request.cwd,
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
  executor_.WaitForIdle();
  return completion;
}

void SaveFormatterService::FlushPendingRuns() {
  if (PendingCount() == 0 && mailbox_.PendingCount() == 0) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("SaveFormatterService::FlushPendingRuns");
  executor_.WaitForIdle();
  mailbox_.Drain();
}

}
