#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace microide::workspace {

class WorkspaceShell;

// What an SDL file-dialog callback latches for the shell thread to pick up.
// Exactly one of `error_message`, `cancelled` and `selected_path` is meaningful;
// `ready` is what the consumer tests before taking it.
//
// The project (folder) picker and the "Open File…" picker declared this
// separately, with a comment on the second noting it was "structurally identical
// to the project picker". It was — field for field.
struct PendingDialogResult {
  bool ready = false;
  bool cancelled = false;
  std::filesystem::path selected_path;
  std::string error_message;
};

struct ProjectDialogState {
  std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher;
  bool active = false;
  std::mutex mutex;
  PendingDialogResult pending_result;
};

// "Open File…" native picker. Same staged-result-under-a-mutex + shared wake
// event shape as the project picker; it opens a file into an editor tab instead
// of a project tab.
struct OpenFileDialogState {
  // Optional test seam mirroring ProjectDialogState::launcher: returns false when no
  // native backend is available. Receives the default location the picker opens at.
  std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher;
  bool active = false;
  std::mutex mutex;
  PendingDialogResult pending_result;
};

// The font picker latches the same outcome plus the setting row that launched it,
// so it keeps its own type rather than carrying a dead field on the shared one.
struct PendingFontFileDialogResult {
  bool ready = false;
  bool cancelled = false;
  std::string target_setting_id;  // font-family setting row that launched the dialog
  std::filesystem::path selected_path;
  std::string error_message;
};

struct FontFileDialogState {
  // Optional test seam mirroring ProjectDialogState::launcher: returns false when
  // no native backend is available. Receives the setting id being edited.
  std::function<bool(WorkspaceShell&, std::string_view)> launcher;
  bool active = false;
  std::string target_setting_id;  // captured at launch, echoed into the result
  std::mutex mutex;
  PendingFontFileDialogResult pending_result;
};

}  // namespace microide::workspace
