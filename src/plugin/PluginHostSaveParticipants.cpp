#include "plugin/PluginHostInternal.h"

namespace microide::plugin {

#if MICROIDE_HAS_LUA_PLUGINS
PluginHost::Impl::SaveParticipantResult PluginHost::Impl::RunSaveParticipantsBounded(
    const std::filesystem::path& path, std::string input) {
  // The transformed text lives in a shared buffer co-owned by the worker job so a
  // completion that lands AFTER a UI timeout writes into a still-live object the UI
  // has stopped reading -- never into a dangling stack local.
  auto shared = std::make_shared<SaveParticipantResult>();
  shared->text = std::move(input);

  PluginHostSnapshot snapshot = CaptureSnapshot();

  if (worker_ == nullptr || g_exec.executing) {
    // No worker wired (tests / pre-wire) or already on the worker (re-entrant):
    // run inline with exclusive access. No deadline applies -- nothing else can be
    // touching a lua_State.
    ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/false, [&]() {
      shared->ok = provider_query_interop::RunSaveParticipants(
          path, &shared->text, save_participant_runtimes,
          [this](lua_State* state) { return FindPluginByState(state); },
          [this](lua_State* state, const std::filesystem::path& buffer_path,
                 std::string_view value) { PushBufferContext(state, buffer_path, value); },
          &shared->error);
    });
    return *shared;
  }

  return WaitForSaveParticipantsOnWorker(std::move(snapshot), path, std::move(shared));
}

PluginHost::Impl::SaveParticipantResult PluginHost::Impl::WaitForSaveParticipantsOnWorker(
    PluginHostSnapshot snapshot, const std::filesystem::path& path,
    std::shared_ptr<SaveParticipantResult> shared) {
  worker_->EnsureStarted();
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();
  // PostFront: a user-blocking save must not sit behind a backlog of speculative
  // query jobs. direct=false: any host side effects defer to the mailbox, so a job
  // that outlives our wait can never mutate live shell state after we move on.
  worker_->PostFront([this, snapshot = std::move(snapshot), shared, path, done]() mutable {
    ExecuteWithContext(&snapshot, /*direct=*/false, /*allow_registration=*/false, [&]() {
      shared->ok = provider_query_interop::RunSaveParticipants(
          path, &shared->text, save_participant_runtimes,
          [this](lua_State* state) { return FindPluginByState(state); },
          [this](lua_State* state, const std::filesystem::path& buffer_path,
                 std::string_view value) { PushBufferContext(state, buffer_path, value); },
          &shared->error);
    });
    done->set_value();
  });

  if (finished.wait_for(save_participant_deadline_) == std::future_status::ready) {
    return *shared;
  }
  // Timed out: leave `text` to the still-running worker job and tell the caller to
  // proceed with its own untransformed buffer.
  SaveParticipantResult timed_out;
  timed_out.timed_out = true;
  return timed_out;
}

std::function<PluginHost::SaveParticipantOutcome()> PluginHost::Impl::PrepareSaveParticipantsJob(
    const std::filesystem::path& path, std::string text) {
  const auto outcome_of = [](SaveParticipantResult result, std::string original) {
    SaveParticipantOutcome outcome;
    outcome.ok = result.ok || result.timed_out;
    outcome.timed_out = result.timed_out;
    outcome.text = result.timed_out ? std::move(original) : std::move(result.text);
    outcome.error = std::move(result.error);
    return outcome;
  };
  if (worker_ == nullptr || g_exec.executing) {
    // The participants cannot leave this thread (no worker wired, or already on
    // it), so they run here, now, exactly as the blocking path does; the job only
    // hands their answer over.
    std::string original = text;
    SaveParticipantOutcome outcome =
        outcome_of(RunSaveParticipantsBounded(path, std::move(text)), std::move(original));
    return [outcome = std::move(outcome)]() { return outcome; };
  }
  // The snapshot is the only shell state a participant reads, and it is taken
  // here, on the shell thread. The rest of the job may run on any thread.
  return [this, snapshot = CaptureSnapshot(), path, text = std::move(text),
          outcome_of]() mutable {
    auto shared = std::make_shared<SaveParticipantResult>();
    shared->text = text;
    return outcome_of(WaitForSaveParticipantsOnWorker(std::move(snapshot), path, std::move(shared)),
                      std::move(text));
  };
}
#endif

}  // namespace microide::plugin
