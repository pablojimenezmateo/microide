#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::project::internal {

struct CommandResult {
  int exit_code = -1;
  std::string output;

  bool success() const { return exit_code == 0; }
};

inline std::string EscapeShellArg(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

inline bool HasGitMarker(const std::filesystem::path& root) {
  return !root.empty() && std::filesystem::exists(root / ".git");
}

inline std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty()) {
    return std::nullopt;
  }

  std::error_code error;
  const std::filesystem::path relative = std::filesystem::relative(
      absolute_path.lexically_normal(), root.lexically_normal(), error);
  if (error || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return std::nullopt;
  }
  return relative.lexically_normal();
}

inline std::string BuildGitCommand(const std::filesystem::path& root,
                                   std::string_view arguments,
                                   bool silence_stderr = true) {
  std::string command = "git -C '";
  command += EscapeShellArg(root.lexically_normal().string());
  command += "' ";
  command += arguments;
  if (silence_stderr) {
    command += " 2>/dev/null";
  }
  return command;
}

inline CommandResult ReadCommandOutput(std::string_view command) {
  CommandResult result;
  const std::string owned_command(command);
  FILE* pipe = popen(owned_command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const std::size_t bytes_read = fread(buffer.data(), 1, buffer.size(), pipe);
    if (bytes_read > 0) {
      result.output.append(buffer.data(), bytes_read);
    }
    if (bytes_read < buffer.size()) {
      break;
    }
  }

  result.exit_code = pclose(pipe);
  return result;
}

inline bool CommandSucceeds(std::string_view command) {
  return ReadCommandOutput(command).success();
}

}  // namespace microide::project::internal
