#include "app/AppStartupOptions.h"

#include <iostream>
#include <string_view>

namespace microide::app {

namespace {

bool IsFlag(std::string_view arg) {
  return arg.starts_with("--");
}

}  // namespace

AppStartupParseResult ParseAppStartupOptions(int argc, char** argv) {
  AppStartupParseResult result;
  if (argc < 2) {
    return result;
  }

  std::optional<std::filesystem::path> positional;
  for (int i = 1; i < argc; ++i) {
    const char* raw = argv[i];
    if (raw == nullptr) {
      continue;
    }
    const std::string_view arg(raw);
    if (arg == "--disable-plugins") {
      result.options.disable_plugins = true;
      continue;
    }
    if (arg == "--safe-mode") {
      result.options.safe_mode = true;
      result.options.disable_plugins = true;
      continue;
    }
    if (arg == "--control-spec") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "--control-spec requires a file path\n";
        result.exit_code = 2;
        return result;
      }
      result.options.control_spec_path = std::filesystem::path(argv[++i]);
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      std::cerr << "usage: microide [--disable-plugins] [--safe-mode] "
                   "[--control-spec <file>] [project-path]\n"
                   "       microide control-help        protocol + spec reference\n"
                   "       microide control-commands     list runnable command names\n"
                   "       microide control-list         running instances + sockets\n"
                   "\n"
                   "Live remote control is gated on the `control.enabled` setting "
                   "(off by default).\n";
      result.show_usage = true;
      return result;
    }
    if (IsFlag(arg)) {
      std::cerr << "unknown option: " << arg << '\n';
      result.exit_code = 2;
      return result;
    }
    positional = std::filesystem::path(arg);
  }

  result.options.project_path = std::move(positional);
  return result;
}

}  // namespace microide::app
