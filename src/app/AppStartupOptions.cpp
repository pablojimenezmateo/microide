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
    if (arg == "--control") {
      result.options.control_stdout = true;
      continue;
    }
    if (arg == "--dap-log") {
      // Bare form: default sink. A custom path uses the attached `--dap-log=<path>`
      // form below so a following positional project path is never swallowed
      // (`microide --dap-log /repo` opens /repo, it does not log to it).
      result.options.dap_log_path = std::filesystem::path("/tmp/microide-dap.log");
      continue;
    }
    if (arg.starts_with("--dap-log=")) {
      const std::string_view value = arg.substr(std::string_view("--dap-log=").size());
      if (value.empty()) {
        std::cerr << "--dap-log=<path> requires a non-empty path\n";
        result.exit_code = 2;
        return result;
      }
      result.options.dap_log_path = std::filesystem::path(value);
      continue;
    }
    if (arg == "--set") {
      if (i + 2 >= argc || argv[i + 1] == nullptr || argv[i + 2] == nullptr) {
        std::cerr << "--set requires <id> <value>\n";
        result.exit_code = 2;
        return result;
      }
      result.options.setting_overrides.emplace_back(argv[i + 1], argv[i + 2]);
      i += 2;
      continue;
    }
    if (arg == "--version" || arg == "-V") {
      result.show_version = true;
      return result;
    }
    if (arg == "--help" || arg == "-h") {
      std::cerr << "usage: microide [--disable-plugins] [--safe-mode] [--control] "
                   "[--set <id> <value>]...\n"
                   "                [--control-spec <file>] [--dap-log[=path]] [--version] "
                   "[project-path]\n"
                   "       microide control-send [...]   send one command/query to an instance\n"
                   "       microide control-help         protocol + spec reference\n"
                   "       microide control-commands     list runnable command names\n"
                   "       microide control-list         running instances + sockets\n"
                   "\n"
                   "--control force-starts the control channel and mirrors responses/events to\n"
                   "stdout as JSONL (the headless entry point). Otherwise the live channel is\n"
                   "gated on the `control.enabled` setting (off by default). --set applies a\n"
                   "transient (never-persisted) setting override and may be repeated.\n"
                   "\n"
                   "Driving microide from an agent (open files, set breakpoints, run a debug\n"
                   "session, hand over the live window): the quickest path is\n"
                   "`microide <project> --set control.enabled true &` then `microide control-send\n"
                   "debug-run <program>`. See `microide control-help` for the full protocol,\n"
                   "query verbs, commands, control-send usage, and end-to-end recipes.\n";
      result.show_usage = true;
      return result;
    }
    if (IsFlag(arg)) {
      std::cerr << "unknown option: " << arg << '\n';
      result.exit_code = 2;
      return result;
    }
    if (positional.has_value()) {
      // A second positional path is almost always a command-line mistake; opening
      // only the last one silently would hide it. Reject with usage guidance.
      std::cerr << "unexpected extra project path: " << arg
                << " (only one project path may be given)\n";
      result.exit_code = 2;
      return result;
    }
    positional = std::filesystem::path(arg);
  }

  result.options.project_path = std::move(positional);
  return result;
}

}  // namespace microide::app
