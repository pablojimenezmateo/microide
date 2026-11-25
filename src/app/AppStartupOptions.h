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
  std::optional<std::filesystem::path> control_spec_path;
  bool control_stdout = false;
  std::vector<std::pair<std::string, std::string>> setting_overrides;
  std::optional<std::filesystem::path> dap_log_path;
  bool quick_exit_on_shutdown = true;
  bool handle_termination_signals = true;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
};

struct AppStartupParseResult {
  AppStartupOptions options;
  bool show_usage = false;
  bool show_version = false;
  int exit_code = 0;
};

AppStartupParseResult ParseAppStartupOptions(int argc, char** argv);

}
