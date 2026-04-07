#include "project/GitCompareService.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

namespace {

std::string EscapeShellArg(std::string_view text) {
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

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult ReadCommandOutput(const std::string& command) {
  CommandResult result;
  FILE* pipe = popen(command.c_str(), "r");
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

bool HasGitMarker(const std::filesystem::path& root) {
  return std::filesystem::exists(root / ".git");
}

}  // namespace

std::vector<GitCommitEntry> CollectGitFileHistory(const std::filesystem::path& root,
                                                  const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !HasGitMarker(root)) {
    return {};
  }

  std::error_code error;
  auto relative = std::filesystem::relative(absolute_path, root, error);
  if (error || relative.empty()) {
    return {};
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' log --follow --no-color --pretty=format:%H%x09%h%x09%s -- '" +
      EscapeShellArg(relative.generic_string()) + "' 2>/dev/null";
  const CommandResult result = ReadCommandOutput(command);
  if (result.exit_code != 0 || result.output.empty()) {
    return {};
  }

  std::vector<GitCommitEntry> commits;
  std::istringstream stream(result.output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    std::size_t first_tab = line.find('\t');
    std::size_t second_tab = first_tab == std::string::npos ? std::string::npos
                                                            : line.find('\t', first_tab + 1);
    if (first_tab == std::string::npos || second_tab == std::string::npos) {
      continue;
    }
    commits.push_back(GitCommitEntry{
        .hash = line.substr(0, first_tab),
        .short_hash = line.substr(first_tab + 1, second_tab - first_tab - 1),
        .subject = line.substr(second_tab + 1),
    });
  }
  return commits;
}

std::optional<GitFileContentAtCommit> ReadGitFileAtCommit(const std::filesystem::path& root,
                                                          const std::filesystem::path& absolute_path,
                                                          const std::string& hash) {
  if (root.empty() || absolute_path.empty() || hash.empty()) {
    return std::nullopt;
  }

  std::error_code error;
  auto relative = std::filesystem::relative(absolute_path, root, error);
  if (error || relative.empty()) {
    return std::nullopt;
  }

  const std::string spec = hash + ":" + relative.generic_string();
  const std::string exists_command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' cat-file -e '" + EscapeShellArg(spec) + "' 2>/dev/null";
  const CommandResult exists_result = ReadCommandOutput(exists_command);
  if (exists_result.exit_code != 0) {
    return GitFileContentAtCommit{.exists = false, .content = ""};
  }

  const std::string show_command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' show '" + EscapeShellArg(spec) + "' 2>/dev/null";
  const CommandResult show_result = ReadCommandOutput(show_command);
  if (show_result.exit_code != 0) {
    return std::nullopt;
  }

  return GitFileContentAtCommit{.exists = true, .content = std::move(show_result.output)};
}

}  // namespace microide::project
