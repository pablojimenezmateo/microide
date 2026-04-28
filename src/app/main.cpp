#include "app/Application.h"
#include "persistence/PersistedRecordDump.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
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

  microide::app::Application application;
  return application.Run();
}
