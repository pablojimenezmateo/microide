#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <iterator>
#include <string>

namespace microide::workspace {

WorkspaceShell::ProjectOpenDialogLaunchResult WorkspaceShell::OpenNativeProjectPicker(
    std::string* error_message) {
  if (project_dialog_state_.active) {
    if (error_message != nullptr) {
      *error_message = "Project picker already open";
    }
    return ProjectOpenDialogLaunchResult::AlreadyOpen;
  }

  std::error_code error;
  const std::filesystem::path default_location =
      context_.current_project_state.root.empty()
          ? std::filesystem::current_path(error)
          : context_.current_project_state.root;
  const std::filesystem::path normalized_default = error ? std::filesystem::path{}
                                                         : default_location.lexically_normal();

  project_dialog_state_.active = true;
  if (project_dialog_state_.launcher) {
    if (!project_dialog_state_.launcher(*this, normalized_default)) {
      project_dialog_state_.active = false;
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
    project_dialog_state_.active = false;
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
    std::lock_guard<std::mutex> lock(shell->project_dialog_state_.mutex);
    shell->project_dialog_state_.pending_result = std::move(pending);
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
    std::lock_guard<std::mutex> lock(project_dialog_state_.mutex);
    if (!project_dialog_state_.pending_result.ready) {
      return;
    }
    pending = std::move(project_dialog_state_.pending_result);
    project_dialog_state_.pending_result = PendingProjectOpenDialogResult{};
  }

  project_dialog_state_.active = false;
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

void WorkspaceShell::OpenNativeFontFilePicker(std::string setting_id) {
  if (font_file_dialog_state_.active) {
    return;
  }
  font_file_dialog_state_.active = true;
  font_file_dialog_state_.target_setting_id = setting_id;

  if (font_file_dialog_state_.launcher) {
    if (!font_file_dialog_state_.launcher(*this, setting_id)) {
      font_file_dialog_state_.active = false;
    }
    return;
  }

  static constexpr SDL_DialogFileFilter kFontFilters[] = {
      {"Fonts (TTF, OTF)", "ttf;otf"},
      {"All files", "*"},
  };
  SDL_ClearError();
  SDL_ShowOpenFileDialog(&WorkspaceShell::OnFontFileDialogComplete, this, dialog_window_,
                         kFontFilters, static_cast<int>(std::size(kFontFilters)),
                         /*default_location=*/nullptr, /*allow_many=*/false);
  const std::string dialog_error = SDL_GetError();
  if (!dialog_error.empty()) {
    font_file_dialog_state_.active = false;
  }
}

void SDLCALL WorkspaceShell::OnFontFileDialogComplete(void* userdata, const char* const* filelist,
                                                      int /*filter*/) {
  auto* shell = static_cast<WorkspaceShell*>(userdata);
  if (shell == nullptr) {
    return;
  }

  PendingFontFileDialogResult pending;
  pending.ready = true;
  pending.target_setting_id = shell->font_file_dialog_state_.target_setting_id;
  if (filelist == nullptr) {
    pending.error_message = SDL_GetError();
  } else if (filelist[0] == nullptr) {
    pending.cancelled = true;
  } else {
    pending.selected_path = std::filesystem::path(filelist[0]).lexically_normal();
  }

  {
    std::lock_guard<std::mutex> lock(shell->font_file_dialog_state_.mutex);
    shell->font_file_dialog_state_.pending_result = std::move(pending);
  }

  // Reuse the project-dialog wake event: both dialog consumers run off it and are
  // idempotent no-ops when their own result isn't ready.
  if (shell->project_open_dialog_event_type_ != 0) {
    SDL_Event event{};
    event.type = shell->project_open_dialog_event_type_;
    SDL_PushEvent(&event);
  }
}

void WorkspaceShell::ConsumePendingFontFileDialogResult() {
  PendingFontFileDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(font_file_dialog_state_.mutex);
    if (!font_file_dialog_state_.pending_result.ready) {
      return;
    }
    pending = std::move(font_file_dialog_state_.pending_result);
    font_file_dialog_state_.pending_result = PendingFontFileDialogResult{};
  }

  font_file_dialog_state_.active = false;
  // Whatever the outcome, close any open picker/value edit so focus never strands.
  CancelSettingValueEdit();
  if (!pending.error_message.empty() || pending.cancelled ||
      pending.selected_path.empty() || pending.target_setting_id.empty()) {
    return;
  }

  WriteSettingRespectingScope(pending.target_setting_id, pending.selected_path.string());
  RefreshSettingsOverlayCatalog();
  RequestOverlayRedraw();
}

}  // namespace microide::workspace
