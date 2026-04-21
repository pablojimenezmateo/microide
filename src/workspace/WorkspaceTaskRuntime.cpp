#include "workspace/WorkspaceTaskRuntime.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "platform/AsyncSubprocess.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

namespace {

std::string BuildShellCommand(const std::vector<std::string>& argv) {
  std::string command;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      command.push_back(' ');
    }
    command += QuoteCommandArg(argv[i]);
  }
  return command;
}

std::vector<std::string> BuildTaskCommand(const TaskSpec& spec) {
  if (spec.command.empty()) {
    return {};
  }

  if (spec.run_in_shell) {
    if (spec.command.size() == 1) {
      return {"sh", "-lc", spec.command.front() + " 2>&1"};
    }
    return {"sh", "-lc", BuildShellCommand(spec.command) + " 2>&1"};
  }
  return {"sh", "-lc", BuildShellCommand(spec.command) + " 2>&1"};
}

void AppendChunkLines(std::string_view chunk,
                      std::string* partial_line,
                      std::vector<std::string>* output) {
  if (partial_line == nullptr || output == nullptr) {
    return;
  }

  partial_line->append(chunk);
  std::size_t newline = partial_line->find('\n');
  while (newline != std::string::npos) {
    output->push_back(partial_line->substr(0, newline));
    partial_line->erase(0, newline + 1);
    newline = partial_line->find('\n');
  }
}

}  // namespace

WorkspaceTaskRuntime::WorkspaceTaskRuntime() = default;

WorkspaceTaskRuntime::~WorkspaceTaskRuntime() {
  Shutdown();
}

void WorkspaceTaskRuntime::Initialize() {
  event_type_ = SDL_RegisterEvents(1);
  if (event_type_ == static_cast<Uint32>(-1)) {
    event_type_ = 0;
  }
}

void WorkspaceTaskRuntime::Shutdown() {
  CancelActive();
  std::lock_guard lock(mutex_);
  pending_update_.reset();
  active_run_id_ = 0;
  event_type_ = 0;
}

bool WorkspaceTaskRuntime::HandlesEvent(Uint32 type) const {
  return event_type_ != 0 && type == event_type_;
}

std::uint64_t WorkspaceTaskRuntime::Start(const TaskSpec& spec,
                                          const std::filesystem::path& project_root) {
  CancelActive();

  const std::uint64_t run_id = next_run_id_++;
  {
    std::lock_guard lock(mutex_);
    active_run_id_ = run_id;
    pending_update_.reset();
  }

  executor_.Submit([this, spec, project_root, run_id](const util::CancellationToken& token) {
    RunTask(spec, project_root, run_id, token);
  });
  return run_id;
}

void WorkspaceTaskRuntime::CancelActive() {
  executor_.CancelAll();
}

std::optional<WorkspaceTaskRuntime::TaskUpdate> WorkspaceTaskRuntime::ConsumeActiveUpdate() {
  std::lock_guard lock(mutex_);
  if (!pending_update_.has_value() || pending_update_->run_id != active_run_id_) {
    return std::nullopt;
  }

  TaskUpdate update = std::move(*pending_update_);
  pending_update_.reset();
  if (update.finished) {
    active_run_id_ = 0;
  }
  return update;
}

void WorkspaceTaskRuntime::RunTask(TaskSpec spec,
                                   std::filesystem::path project_root,
                                   std::uint64_t run_id,
                                   const util::CancellationToken& token) {
  TaskUpdate update{
      .run_id = run_id,
      .task_id = spec.id,
      .channel_id = "task." + spec.id,
      .channel_label = spec.label.empty() ? spec.id : spec.label,
  };

  const std::vector<std::string> command = BuildTaskCommand(spec);
  if (command.empty()) {
    update.finished = true;
    update.succeeded = false;
    update.status_text = "Task has no command";
    PublishUpdate(std::move(update));
    return;
  }

  const std::filesystem::path cwd =
      spec.cwd.empty() ? project_root : (project_root / spec.cwd).lexically_normal();
  platform::AsyncSubprocess process;
  if (!process.Start(command, cwd.string())) {
    update.finished = true;
    update.succeeded = false;
    update.status_text = "Failed to start task";
    PublishUpdate(std::move(update));
    return;
  }

  std::string partial_line;
  while (true) {
    if (token.IsCancellationRequested()) {
      process.Shutdown(0);
      if (!partial_line.empty()) {
        update.appended_lines.push_back(std::move(partial_line));
      }
      update.finished = true;
      update.succeeded = false;
      update.status_text = "Cancelled";
      PublishUpdate(std::move(update));
      return;
    }

    const std::optional<std::string> chunk = process.Read(4096, 100);
    if (!chunk.has_value()) {
      if (!partial_line.empty()) {
        update.appended_lines.push_back(std::move(partial_line));
      }
      const int exit_code = process.exit_code().value_or(1);
      update.finished = true;
      update.succeeded = exit_code == 0;
      update.status_text = exit_code == 0 ? "Finished"
                                          : "Exited with code " + std::to_string(exit_code);
      PublishUpdate(std::move(update));
      return;
    }

    if (!chunk->empty()) {
      AppendChunkLines(*chunk, &partial_line, &update.appended_lines);
      if (!update.appended_lines.empty()) {
        PublishUpdate(TaskUpdate{
            .run_id = update.run_id,
            .task_id = update.task_id,
            .channel_id = update.channel_id,
            .channel_label = update.channel_label,
            .appended_lines = std::move(update.appended_lines),
        });
        update.appended_lines.clear();
      }
      continue;
    }

    if (!process.IsRunning()) {
      if (!partial_line.empty()) {
        update.appended_lines.push_back(std::move(partial_line));
      }
      const int exit_code = process.exit_code().value_or(1);
      update.finished = true;
      update.succeeded = exit_code == 0;
      update.status_text = exit_code == 0 ? "Finished"
                                          : "Exited with code " + std::to_string(exit_code);
      PublishUpdate(std::move(update));
      return;
    }
  }
}

void WorkspaceTaskRuntime::PublishUpdate(TaskUpdate update) {
  {
    std::lock_guard lock(mutex_);
    if (!pending_update_.has_value() || pending_update_->run_id != update.run_id) {
      pending_update_ = std::move(update);
    } else {
      pending_update_->appended_lines.insert(pending_update_->appended_lines.end(),
                                             std::make_move_iterator(update.appended_lines.begin()),
                                             std::make_move_iterator(update.appended_lines.end()));
      if (update.finished) {
        pending_update_->finished = true;
        pending_update_->succeeded = update.succeeded;
        pending_update_->status_text = std::move(update.status_text);
      }
    }
  }
  PushWakeEvent();
}

void WorkspaceTaskRuntime::PushWakeEvent() const {
  if (event_type_ == 0) {
    return;
  }
  SDL_Event event{};
  event.type = event_type_;
  SDL_PushEvent(&event);
}

}  // namespace microide::workspace
