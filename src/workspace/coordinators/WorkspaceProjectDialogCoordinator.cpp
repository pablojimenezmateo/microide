#include "workspace/shell/WorkspaceShell.h"

#include "util/Waker.h"
#include "workspace/NativeDialogTestMode.h"

#include <atomic>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include "util/ErrorOut.h"

namespace microide::workspace {
namespace {

std::atomic_bool g_native_dialogs_disabled{false};

template <typename Show>
std::string ShowNativeDialog(Show&& show) {
  if (g_native_dialogs_disabled.load(std::memory_order_relaxed)) {
    return "Native dialogs are disabled in tests";
  }
  SDL_ClearError();
  show();
  return SDL_GetError();
}

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

template <typename DialogState, typename Pending>
void LatchDialogResult(DialogState& state, Pending pending, Uint32 wake_event) {
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending_result = std::move(pending);
  }
  util::PushWake(wake_event);
}

template <typename Show>
ProjectOpenPickerResult LaunchNativePicker(WorkspaceShell& shell, NativePickerState& state,
                                           const std::filesystem::path& project_root,
                                           const char* already_open_message,
                                           std::string* error_message, Show&& show) {
  const auto fail = [&](std::string message, ProjectOpenPickerResult result) {
    util::SetError(error_message, std::move(message));
    return result;
  };
  if (state.active) {
    return fail(already_open_message, ProjectOpenPickerResult::AlreadyOpen);
  }

  std::error_code error;
  const std::filesystem::path default_location =
      project_root.empty() ? std::filesystem::current_path(error) : project_root;
  const std::filesystem::path normalized_default =
      error ? std::filesystem::path{} : default_location.lexically_normal();

  state.active = true;
  if (state.launcher) {
    if (!state.launcher(shell, normalized_default)) {
      state.active = false;
      return fail("Native dialog backend unavailable", ProjectOpenPickerResult::Unavailable);
    }
    return ProjectOpenPickerResult::Launched;
  }

  const std::string default_location_string = normalized_default.string();
  std::string dialog_error = ShowNativeDialog([&] {
    show(default_location_string.empty() ? nullptr : default_location_string.c_str());
  });
  if (!dialog_error.empty()) {
    state.active = false;
    return fail(std::move(dialog_error), ProjectOpenPickerResult::Unavailable);
  }
  return ProjectOpenPickerResult::Launched;
}

std::optional<std::filesystem::path> TakeNativePickerSelection(NativePickerState& state) {
  PendingDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.pending_result.ready) {
      return std::nullopt;
    }
    pending = std::move(state.pending_result);
    state.pending_result = PendingDialogResult{};
  }
  state.active = false;
  if (!pending.error_message.empty() || pending.cancelled || pending.selected_path.empty()) {
    return std::nullopt;
  }
  return std::move(pending.selected_path);
}

}

ProjectOpenPickerResult WorkspaceShell::OpenNativeProjectPicker(
    std::string* error_message) {
  return LaunchNativePicker(
      *this, project_dialog_state_, context_.current_project_state.root,
      "Project picker already open", error_message, [&](const char* default_location) {
        SDL_ShowOpenFolderDialog(&WorkspaceShell::OnProjectOpenDialogComplete, this,
                                 dialog_window_, default_location, false);
      });
}

void SDLCALL WorkspaceShell::OnProjectOpenDialogComplete(void* userdata,
                                                         const char* const* filelist,
                                                         int ) {
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
  if (const auto selected = TakeNativePickerSelection(project_dialog_state_); selected.has_value()) {
    OpenProjectTab(*selected, true, true);
  }
}

ProjectOpenPickerResult WorkspaceShell::OpenNativeFilePicker(
    std::string* error_message) {
  return LaunchNativePicker(
      *this, open_file_dialog_state_, context_.current_project_state.root,
      "File picker already open", error_message, [&](const char* default_location) {
        SDL_ShowOpenFileDialog(&WorkspaceShell::OnOpenFileDialogComplete, this, dialog_window_,
                               nullptr, 0, default_location,
                               false);
      });
}

void SDLCALL WorkspaceShell::OnOpenFileDialogComplete(void* userdata,
                                                      const char* const* filelist,
                                                      int ) {
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
  if (const auto selected = TakeNativePickerSelection(open_file_dialog_state_); selected.has_value()) {
    OpenFile(*selected);
  }
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
  const std::string dialog_error = ShowNativeDialog([&] {
    SDL_ShowOpenFileDialog(&WorkspaceShell::OnFontFileDialogComplete, this, dialog_window_,
                           kFontFilters, static_cast<int>(std::size(kFontFilters)),
                           nullptr, false);
  });
  if (!dialog_error.empty()) {
    font_file_dialog_state_.active = false;
  }
}

void SDLCALL WorkspaceShell::OnFontFileDialogComplete(void* userdata, const char* const* filelist,
                                                      int ) {
  auto* shell = static_cast<WorkspaceShell*>(userdata);
  if (shell == nullptr) {
    return;
  }
  PendingFontFileDialogResult pending;
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
  CancelSettingValueEdit();
  if (!pending.error_message.empty() || pending.cancelled ||
      pending.selected_path.empty() || pending.target_setting_id.empty()) {
    return;
  }

  WriteSettingRespectingScope(pending.target_setting_id, pending.selected_path.string());
  RefreshSettingsOverlayCatalog();
  RequestPreparedFrame();
}

void SetNativeDialogsDisabledForTesting(bool disabled) {
  g_native_dialogs_disabled.store(disabled, std::memory_order_relaxed);
}

bool NativeDialogsDisabledForTesting() {
  return g_native_dialogs_disabled.load(std::memory_order_relaxed);
}

}
