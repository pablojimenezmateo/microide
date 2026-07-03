#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace microide::workspace {

class WorkspaceShell;

struct PendingProjectOpenDialogResult {
  bool ready = false;
  bool cancelled = false;
  std::filesystem::path selected_path;
  std::string error_message;
};

struct ProjectDialogState {
  std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher;
  bool active = false;
  std::mutex mutex;
  PendingProjectOpenDialogResult pending_result;
};

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
