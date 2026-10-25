#include "workspace/shell/WorkspaceShell.h"

#include "util/Waker.h"
#include "workspace/NativeDialogTestMode.h"

#include <atomic>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>

namespace microide::workspace {
namespace {

std::atomic_bool g_native_dialogs_disabled{false};

// Every native dialog launches through here: `show` is the SDL_Show*Dialog call.
// Returns why it did not open (SDL's error, or the test-mode refusal); empty
// when it did.
template <typename Show>
std::string ShowNativeDialog(Show&& show) {
  if (g_native_dialogs_disabled.load(std::memory_order_relaxed)) {
    return "Native dialogs are disabled in tests";
  }
  SDL_ClearError();
  show();
  return SDL_GetError();
}

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
  util::PushWake(wake_event);
}

// The launch half of the folder and file pickers. Refuses a second picker while
// one is open, opens at the project root (else the process working directory --
// the pickers are reachable from the cold-start welcome screen too), and goes
// through the test-seam launcher when one is installed. `show(default_location)`
// is the SDL_Show*Dialog call, given the location as SDL takes it (null for none).
template <typename Show>
ProjectOpenPickerResult LaunchNativePicker(WorkspaceShell& shell, NativePickerState& state,
                                           const std::filesystem::path& project_root,
                                           const char* already_open_message,
                                           std::string* error_message, Show&& show) {
  const auto fail = [&](std::string message, ProjectOpenPickerResult result) {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
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

// The shell-thread half: takes a latched result and ends the picker. The path the
// user chose, or nullopt when there is no result yet, it failed, or it was cancelled.
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

}  // namespace

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
                               /*filters=*/nullptr, /*nfilters=*/0, default_location,
                               /*allow_many=*/false);
      });
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
                           /*default_location=*/nullptr, /*allow_many=*/false);
  });
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

void SetNativeDialogsDisabledForTesting(bool disabled) {
  g_native_dialogs_disabled.store(disabled, std::memory_order_relaxed);
}

bool NativeDialogsDisabledForTesting() {
  return g_native_dialogs_disabled.load(std::memory_order_relaxed);
}

}  // namespace microide::workspace
