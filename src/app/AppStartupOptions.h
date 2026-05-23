#pragma once

#include <filesystem>
#include <optional>
#include <utility>

namespace microide::app {

struct AppStartupOptions {
  bool disable_plugins = false;
  bool safe_mode = false;
  std::optional<std::filesystem::path> project_path;

  bool plugins_disabled() const { return disable_plugins || safe_mode; }
};

struct AppStartupParseResult {
  AppStartupOptions options;
  bool show_usage = false;
  int exit_code = 0;
};

AppStartupParseResult ParseAppStartupOptions(int argc, char** argv);

}  // namespace microide::app
