#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

inline std::string SummarizeCommandForError(const std::vector<std::string>& command) {
  if (command.empty()) {
    return "(no command)";
  }
  std::string base = std::filesystem::path(command.front()).filename().string();
  if (base.empty()) {
    base = command.front();
  }
  return base + " (" + std::to_string(command.size() - 1) + " args)";
}

}
