#pragma once

#include <filesystem>
#include <optional>
#include <utility>

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
