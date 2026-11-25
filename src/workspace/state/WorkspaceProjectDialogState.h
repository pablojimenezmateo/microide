#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace microide::workspace {

class WorkspaceShell;

struct PendingDialogResult {
  bool ready = false;
  bool cancelled = false;
  std::filesystem::path selected_path;
  std::string error_message;
};

struct NativePickerState {
  std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher;
  bool active = false;
  std::mutex mutex;
  PendingDialogResult pending_result;
};

struct PendingFontFileDialogResult {
  bool ready = false;
  bool cancelled = false;
  std::string target_setting_id;
  std::filesystem::path selected_path;
  std::string error_message;
};

struct FontFileDialogState {
  std::function<bool(WorkspaceShell&, std::string_view)> launcher;
  bool active = false;
  std::string target_setting_id;
  std::mutex mutex;
  PendingFontFileDialogResult pending_result;
};

}
