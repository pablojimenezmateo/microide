#include "app/AppStartupOptions.h"
#include "app/Application.h"
#include "persistence/PersistedRecordDump.h"
#include "platform/HostPlatform.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  // Writing to a subprocess/terminal/LSP pipe whose reader has died must surface as
  // EPIPE, not kill the editor. Install this before any I/O can occur.
  microide::platform::IgnoreBrokenPipeSignal();

  if (argc >= 2) {
    const std::string_view command =
        argv[1] != nullptr ? std::string_view(argv[1]) : std::string_view{};
    if (command == "dump-state" || command == "microide-dump-state") {
      if (argc != 3 || argv[2] == nullptr || std::string_view(argv[2]).empty()) {
        std::cerr << "usage: microide dump-state <persisted-file>\n";
        return 2;
      }
      std::string dump;
      std::string error;
      if (!microide::persistence::DumpPersistedRecordFile(
              std::filesystem::path(argv[2]), &dump, &error)) {
        std::cerr << "dump-state failed: " << error << '\n';
        return 1;
      }
      std::cout << dump;
      return 0;
    }
  }

  const microide::app::AppStartupParseResult parsed =
      microide::app::ParseAppStartupOptions(argc, argv);
  if (parsed.show_usage) {
    return 0;
  }
  if (parsed.exit_code != 0) {
    return parsed.exit_code;
  }

  microide::app::Application application(parsed.options);
  return application.Run();
}
