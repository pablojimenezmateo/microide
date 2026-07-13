#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

// A redacted command summary for user-visible error text: the executable
// basename plus an argument count. Language-server and debug-adapter command
// lines routinely carry private checkout paths, `--token=…` secrets, or
// environment-derived values, so the full argv must never surface in the status
// UI. The full command stays available in the opt-in LSP/DAP diagnostic trace.
// Shared by the LSP and DAP managers so the redaction cannot drift between them.
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

}  // namespace microide::workspace
