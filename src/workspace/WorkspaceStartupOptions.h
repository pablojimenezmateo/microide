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
  // `--control`: force-start the control channel + stdout JSONL mirror.
  bool control_stdout = false;
  // `--set <id> <value>` overrides applied transiently at startup.
  std::vector<std::pair<std::string, std::string>> setting_overrides;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
  // Skip restoring the previous session when an explicit project was requested
  // (positional path or a cold-start spec's `project`) so the named project wins
  // over a saved session, and always in safe mode.
  bool skip_workspace_session_restore() const {
    return safe_mode || project_path.has_value();
  }
};

}  // namespace microide::workspace
