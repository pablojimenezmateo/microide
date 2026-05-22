#pragma once

#include <filesystem>
#include <optional>

namespace microide::workspace {

struct WorkspaceStartupOptions {
  bool disable_plugins = false;
  bool safe_mode = false;
  std::optional<std::filesystem::path> project_path;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
  bool skip_workspace_session_restore() const { return safe_mode; }
};

}  // namespace microide::workspace
