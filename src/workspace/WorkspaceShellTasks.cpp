#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

namespace {

std::string OutputChannelIdForTask(const TaskSpec& spec) {
  return "task." + spec.id;
}

}  // namespace

void WorkspaceShell::ConsumeTaskRuntimeUpdates() {
  const std::optional<WorkspaceTaskRuntime::TaskUpdate> update =
      task_runtime_.ConsumeActiveUpdate();
  if (!update.has_value()) {
    return;
  }

  output_channels_.EnsureChannel(update->channel_id, update->channel_label);
  for (const std::string& line : update->appended_lines) {
    output_channels_.AppendLine(update->channel_id, update->channel_label, line);
  }
  if (update->finished && !update->status_text.empty()) {
    output_channels_.AppendLine(update->channel_id, update->channel_label, update->status_text);
  }
  EnsureOutputChannelTabOpen(update->channel_id);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = update->channel_id;
  RequestBottomPanelRedraw();
}

bool WorkspaceShell::ShowTaskPickerOverlay() {
  context_.current_project_state.overlay.workflow.task_picker.entries.clear();
  context_.current_project_state.overlay.workflow.task_picker.error.clear();
  context_.current_project_state.overlay.workflow.task_picker.selected_index = 0;
  for (const TaskSpec& task : task_registry_.Specs()) {
    context_.current_project_state.overlay.workflow.task_picker.entries.push_back(TaskPickerEntry{
        .id = task.id,
        .label = task.label,
        .group = task.group,
    });
  }
  if (context_.current_project_state.overlay.workflow.task_picker.entries.empty()) {
    context_.current_project_state.overlay.workflow.task_picker.error = "No tasks registered";
  }
  ShowOverlay(OverlayMode::TaskPicker);
  return true;
}

bool WorkspaceShell::RunSelectedTask() {
  if (context_.current_project_state.overlay.workflow.task_picker.entries.empty()) {
    return false;
  }
  const TaskPickerEntry& selected =
      context_.current_project_state.overlay.workflow.task_picker.entries[std::min(
          context_.current_project_state.overlay.workflow.task_picker.selected_index,
          context_.current_project_state.overlay.workflow.task_picker.entries.size() - 1)];
  const TaskSpec* spec = task_registry_.FindTask(selected.id);
  if (spec == nullptr) {
    return false;
  }
  return RunTaskById(spec->id, nullptr);
}

bool WorkspaceShell::RunTaskById(std::string_view id, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const TaskSpec* spec = task_registry_.FindTask(std::string(id));
  if (spec == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown task: " + std::string(id);
    }
    return false;
  }
  task_runtime_.Start(*spec, context_.current_project_state.root);
  const std::string channel_id = OutputChannelIdForTask(*spec);
  output_channels_.EnsureChannel(channel_id, spec->label.empty() ? spec->id : spec->label);
  EnsureOutputChannelTabOpen(channel_id);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  DismissOverlay(false);
  RequestBottomPanelRedraw();
  return true;
}

}  // namespace microide::workspace
