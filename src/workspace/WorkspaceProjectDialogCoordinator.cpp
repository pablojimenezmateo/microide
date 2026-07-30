#include "workspace/WorkspaceShell.h"

#include "util/SdlWake.h"

#include <filesystem>
#include <iterator>
#include <string>

namespace microide::workspace {
namespace {

// Decode SDL's file-dialog callback arguments into the outcome fields every
// pending-result type shares. SDL signals three distinct things through one
// callback: a null list is a failure (reason in SDL_GetError), an empty list is
// the user cancelling, and anything else is a selection.
//
// The three dialogs (project folder, Open File…, font file) each decoded this by
// hand. Getting the null-vs-empty distinction backwards would report a cancel as
// an error or, worse, treat a failure as a silent no-op.
template <typename Pending>
void DecodeDialogFilelist(Pending& pending, const char* const* filelist) {
  pending.ready = true;
  if (filelist == nullptr) {
    pending.error_message = SDL_GetError();
  } else if (filelist[0] == nullptr) {
    pending.cancelled = true;
  } else {
    pending.selected_path = std::filesystem::path(filelist[0]).lexically_normal();
  }
}

// Publish a decoded result to the shell thread: store it under the dialog's own
// mutex, then wake.
//
// The wake is a *checked* push. The result is already stored by the time it
// runs, so a rejected push latches the shared "wake owed" bit for the idle-poll
// fallback rather than leaving the dialog active with its result unapplied. All
// three dialogs share one wake event and each consumer is an idempotent no-op
// when its own result is not ready.
template <typename DialogState, typename Pending>
void LatchDialogResult(DialogState& state, Pending pending, Uint32 wake_event) {
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending_result = std::move(pending);
  }
  util::PushSdlWake(wake_event);
}

}  // namespace

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
  PendingDialogResult pending;
  DecodeDialogFilelist(pending, filelist);
  LatchDialogResult(shell->project_dialog_state_, std::move(pending),
                    shell->project_open_dialog_event_type_);
}

void WorkspaceShell::ConsumePendingProjectOpenDialogResult() {
  PendingDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(project_dialog_state_.mutex);
    if (!project_dialog_state_.pending_result.ready) {
      return;
    }
    pending = std::move(project_dialog_state_.pending_result);
    project_dialog_state_.pending_result = PendingDialogResult{};
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

WorkspaceShell::ProjectOpenDialogLaunchResult WorkspaceShell::OpenNativeFilePicker(
    std::string* error_message) {
  if (open_file_dialog_state_.active) {
    if (error_message != nullptr) {
      *error_message = "File picker already open";
    }
    return ProjectOpenDialogLaunchResult::AlreadyOpen;
  }

  // Open where the user is working: the project root, falling back to the process
  // working directory when no project is open (the picker is reachable from the
  // cold-start welcome screen too).
  std::error_code error;
  const std::filesystem::path default_location =
      context_.current_project_state.root.empty() ? std::filesystem::current_path(error)
                                                  : context_.current_project_state.root;
  const std::filesystem::path normalized_default =
      error ? std::filesystem::path{} : default_location.lexically_normal();

  open_file_dialog_state_.active = true;
  if (open_file_dialog_state_.launcher) {
    if (!open_file_dialog_state_.launcher(*this, normalized_default)) {
      open_file_dialog_state_.active = false;
      if (error_message != nullptr) {
        *error_message = "Native dialog backend unavailable";
      }
      return ProjectOpenDialogLaunchResult::Unavailable;
    }
    return ProjectOpenDialogLaunchResult::Launched;
  }

  const std::string default_location_string = normalized_default.string();
  SDL_ClearError();
  SDL_ShowOpenFileDialog(&WorkspaceShell::OnOpenFileDialogComplete, this, dialog_window_,
                         /*filters=*/nullptr, /*nfilters=*/0,
                         default_location_string.empty() ? nullptr
                                                         : default_location_string.c_str(),
                         /*allow_many=*/false);
  const std::string dialog_error = SDL_GetError();
  if (!dialog_error.empty()) {
    open_file_dialog_state_.active = false;
    if (error_message != nullptr) {
      *error_message = dialog_error;
    }
    return ProjectOpenDialogLaunchResult::Unavailable;
  }

  return ProjectOpenDialogLaunchResult::Launched;
}

void SDLCALL WorkspaceShell::OnOpenFileDialogComplete(void* userdata,
                                                      const char* const* filelist,
                                                      int /*filter*/) {
  auto* shell = static_cast<WorkspaceShell*>(userdata);
  if (shell == nullptr) {
    return;
  }
  PendingDialogResult pending;
  DecodeDialogFilelist(pending, filelist);
  LatchDialogResult(shell->open_file_dialog_state_, std::move(pending),
                    shell->project_open_dialog_event_type_);
}

void WorkspaceShell::ConsumePendingOpenFileDialogResult() {
  PendingDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(open_file_dialog_state_.mutex);
    if (!open_file_dialog_state_.pending_result.ready) {
      return;
    }
    pending = std::move(open_file_dialog_state_.pending_result);
    open_file_dialog_state_.pending_result = PendingDialogResult{};
  }

  open_file_dialog_state_.active = false;
  if (!pending.error_message.empty() || pending.cancelled || pending.selected_path.empty()) {
    return;
  }

  OpenFile(pending.selected_path);
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
  // Echo the setting row that launched the picker so the consumer knows which
  // font-family field to write back to.
  pending.target_setting_id = shell->font_file_dialog_state_.target_setting_id;
  DecodeDialogFilelist(pending, filelist);
  LatchDialogResult(shell->font_file_dialog_state_, std::move(pending),
                    shell->project_open_dialog_event_type_);
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
