#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::app {

struct AppStartupOptions {
  bool disable_plugins = false;
  bool safe_mode = false;
  std::optional<std::filesystem::path> project_path;
  // Cold-start control spec (`--control-spec <file>`): a JSON document that
  // opens a project with breakpoints already set (and optionally files revealed
  // / a debug session started) before the window is interactive. See
  // ControlChannelHelpText() for the schema.
  std::optional<std::filesystem::path> control_spec_path;
  // `--control`: force-start the live control channel (bypassing the
  // `control.enabled` gate) and mirror every response/event/applied line to
  // stdout as JSONL. The headless agent-driving entry point.
  bool control_stdout = false;
  // `--set <id> <value>` (repeatable): transient setting overrides applied at
  // startup. Never persisted to the user's saved config.
  std::vector<std::pair<std::string, std::string>> setting_overrides;
  // `--dap-log [path]`: open a debugger/DAP trace sink at `path` (default
  // /tmp/microide-dap.log) capturing every DAP message plus key debug-subsystem
  // decisions. Diagnostic only; see util::DebugTrace.
  std::optional<std::filesystem::path> dap_log_path;
  // The shipping binary fast-exits via std::quick_exit() at the end of
  // Shutdown() to skip destructor chains. Tests set this false so they can run
  // Initialize()/Shutdown() in-process and verify clean teardown under ASAN.
  bool quick_exit_on_shutdown = true;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
};

struct AppStartupParseResult {
  AppStartupOptions options;
  bool show_usage = false;
  int exit_code = 0;
};

AppStartupParseResult ParseAppStartupOptions(int argc, char** argv);

}  // namespace microide::app
