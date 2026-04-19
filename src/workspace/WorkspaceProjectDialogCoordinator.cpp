#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <string>

namespace microide::workspace {

WorkspaceShell::ProjectOpenDialogLaunchResult WorkspaceShell::OpenNativeProjectPicker(
    std::string* error_message) {
  if (project_open_dialog_active_) {
    if (error_message != nullptr) {
      *error_message = "Project picker already open";
    }
    return ProjectOpenDialogLaunchResult::AlreadyOpen;
  }

  std::error_code error;
  const std::filesystem::path default_location =
      project_root_.empty() ? std::filesystem::current_path(error) : project_root_;
  const std::filesystem::path normalized_default = error ? std::filesystem::path{}
                                                         : default_location.lexically_normal();

  project_open_dialog_active_ = true;
  if (project_open_dialog_launcher_) {
    if (!project_open_dialog_launcher_(*this, normalized_default)) {
      project_open_dialog_active_ = false;
      if (error_message != nullptr) {
        *error_message = "Native dialog backend unavailable";
      }
      return ProjectOpenDialogLaunchResult::Unavailable;
    }
    return ProjectOpenDialogLaunchResult::Launched;
  }

  const std::string default_location_string = normalized_default.string();
  SDL_ClearError();
  SDL_ShowOpenFolderDialog(&WorkspaceShell::OnProjectOpenDialogComplete, this, dialog_window_,
                           default_location_string.empty() ? nullptr
                                                           : default_location_string.c_str(),
                           false);
  const std::string dialog_error = SDL_GetError();
  if (!dialog_error.empty()) {
    project_open_dialog_active_ = false;
    if (error_message != nullptr) {
      *error_message = dialog_error;
    }
    return ProjectOpenDialogLaunchResult::Unavailable;
  }

  return ProjectOpenDialogLaunchResult::Launched;
}

void SDLCALL WorkspaceShell::OnProjectOpenDialogComplete(void* userdata,
                                                         const char* const* filelist,
                                                         int /*filter*/) {
  auto* shell = static_cast<WorkspaceShell*>(userdata);
  if (shell == nullptr) {
    return;
  }

  PendingProjectOpenDialogResult pending;
  pending.ready = true;
  if (filelist == nullptr) {
    pending.error_message = SDL_GetError();
  } else if (filelist[0] == nullptr) {
    pending.cancelled = true;
  } else {
    pending.selected_path = std::filesystem::path(filelist[0]).lexically_normal();
  }

  {
    std::lock_guard<std::mutex> lock(shell->project_open_dialog_mutex_);
    shell->pending_project_open_dialog_result_ = std::move(pending);
  }

  if (shell->project_open_dialog_event_type_ != 0) {
    SDL_Event event{};
    event.type = shell->project_open_dialog_event_type_;
    SDL_PushEvent(&event);
  }
}

void WorkspaceShell::ConsumePendingProjectOpenDialogResult() {
  PendingProjectOpenDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(project_open_dialog_mutex_);
    if (!pending_project_open_dialog_result_.ready) {
      return;
    }
    pending = std::move(pending_project_open_dialog_result_);
    pending_project_open_dialog_result_ = PendingProjectOpenDialogResult{};
  }

  project_open_dialog_active_ = false;
  if (!pending.error_message.empty()) {
    return;
  }
  if (pending.cancelled) {
    return;
  }
  if (pending.selected_path.empty()) {
    return;
  }

  OpenProjectTab(pending.selected_path, true, true);
}

}  // namespace microide::workspace
