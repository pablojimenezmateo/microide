#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

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

}  // namespace microide::workspace
