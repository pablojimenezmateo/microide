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
  // `--detach-handoff <file>`: a detached-tab window seeds its tabs from this
  // handoff session file (written by the parent window) instead of the canonical
  // session, and consumes (deletes) it after hydration.
  std::optional<std::filesystem::path> detach_handoff_path;
  // When false, this window never writes the canonical project session (an
  // editor-tab detach child shares the parent's project root and must not clobber
  // the parent's session). A project-detach child owns its root and persists.
  bool session_persist_enabled = true;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
  // Skip restoring the previous session when an explicit project was requested
  // (positional path or a cold-start spec's `project`) so the named project wins
  // over a saved session, and always in safe mode.
  bool skip_workspace_session_restore() const {
    return safe_mode || project_path.has_value();
  }
};

}  // namespace microide::workspace
