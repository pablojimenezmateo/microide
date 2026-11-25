#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

struct WorkspaceStartupOptions {
  bool disable_plugins = false;
  bool safe_mode = false;
  std::optional<std::filesystem::path> project_path;
  bool control_stdout = false;
  std::vector<std::pair<std::string, std::string>> setting_overrides;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
  bool skip_workspace_session_restore() const {
    return safe_mode || project_path.has_value();
  }
};

}
