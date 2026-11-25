#include "plugin/PluginHostInternal.h"

namespace microide::plugin {

#if MICROIDE_HAS_LUA_PLUGINS
PluginHost::Impl::SaveParticipantResult PluginHost::Impl::RunSaveParticipantsBounded(
    const std::filesystem::path& path, std::string input) {
  auto shared = std::make_shared<SaveParticipantResult>();
  shared->text = std::move(input);

  PluginHostSnapshot snapshot = CaptureSnapshot();

  if (worker_ == nullptr || g_exec.executing) {
    ExecuteWithContext(&snapshot, true, false, [&]() {
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
  worker_->PostFront([this, snapshot = std::move(snapshot), shared, path, done]() mutable {
    ExecuteWithContext(&snapshot, false, false, [&]() {
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
    std::string original = text;
    SaveParticipantOutcome outcome =
        outcome_of(RunSaveParticipantsBounded(path, std::move(text)), std::move(original));
    return [outcome = std::move(outcome)]() { return outcome; };
  }
  return [this, snapshot = CaptureSnapshot(), path, text = std::move(text),
          outcome_of]() mutable {
    auto shared = std::make_shared<SaveParticipantResult>();
    shared->text = text;
    return outcome_of(WaitForSaveParticipantsOnWorker(std::move(snapshot), path, std::move(shared)),
                      std::move(text));
  };
}
#endif

}
