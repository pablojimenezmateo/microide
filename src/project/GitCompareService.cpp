#include "project/GitCompareService.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
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

std::string TrimTrailingWhitespace(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                           text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

std::string ShortRefLabel(std::string_view ref) {
  if (ref.empty()) {
    return {};
  }
  constexpr std::array<std::string_view, 2> prefixes = {
      "refs/remotes/",
      "refs/heads/",
  };
  for (std::string_view prefix : prefixes) {
    if (ref.rfind(prefix, 0) == 0) {
      return std::string(ref.substr(prefix.size()));
    }
  }
  return std::string(ref);
}

GitFileStatus StatusFromDiffCode(char code) {
  switch (code) {
    case 'A':
      return GitFileStatus::Added;
    case 'D':
      return GitFileStatus::Deleted;
    case 'M':
    case 'R':
    case 'C':
    case 'T':
      return GitFileStatus::Modified;
    default:
      return GitFileStatus::Clean;
  }
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

std::optional<GitBranchReference> ResolveGitBaseReference(const std::filesystem::path& root) {
  if (root.empty() || !HasGitMarker(root)) {
    return std::nullopt;
  }

  const std::string escaped_root = EscapeShellArg(root.lexically_normal().string());
  const std::string origin_head_command =
      "git -C '" + escaped_root + "' symbolic-ref --quiet refs/remotes/origin/HEAD 2>/dev/null";
  const CommandResult origin_head_result = ReadCommandOutput(origin_head_command);
  const std::string origin_head = TrimTrailingWhitespace(origin_head_result.output);
  if (origin_head_result.exit_code == 0 && !origin_head.empty()) {
    return GitBranchReference{
        .ref = origin_head,
        .label = ShortRefLabel(origin_head),
    };
  }

  const std::array<std::string_view, 2> local_defaults = {"main", "master"};
  for (std::string_view candidate : local_defaults) {
    const std::string exists_command =
        "git -C '" + escaped_root + "' show-ref --verify --quiet 'refs/heads/" +
        std::string(candidate) + "' 2>/dev/null";
    const CommandResult exists_result = ReadCommandOutput(exists_command);
    if (exists_result.exit_code == 0) {
      return GitBranchReference{
          .ref = std::string(candidate),
          .label = std::string(candidate),
      };
    }
  }

  const std::string upstream_command =
      "git -C '" + escaped_root +
      "' rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null";
  const CommandResult upstream_result = ReadCommandOutput(upstream_command);
  const std::string upstream = TrimTrailingWhitespace(upstream_result.output);
  if (upstream_result.exit_code == 0 && !upstream.empty()) {
    return GitBranchReference{
        .ref = upstream,
        .label = ShortRefLabel(upstream),
    };
  }

  return std::nullopt;
}

std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(const std::filesystem::path& root,
                                                              std::string_view base_ref) {
  if (root.empty() || base_ref.empty() || !HasGitMarker(root)) {
    return {};
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' diff --name-status --find-renames '" + EscapeShellArg(std::string(base_ref)) +
      "...HEAD' 2>/dev/null";
  const CommandResult result = ReadCommandOutput(command);
  if (result.exit_code != 0 || result.output.empty()) {
    return {};
  }

  std::vector<GitBranchFileEntry> entries;
  std::istringstream stream(result.output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }

    std::istringstream line_stream(line);
    std::string status_code;
    std::string path;
    std::string target_path;
    if (!(line_stream >> status_code)) {
      continue;
    }
    if (!(line_stream >> path)) {
      continue;
    }
    if ((status_code[0] == 'R' || status_code[0] == 'C') && (line_stream >> target_path) &&
        !target_path.empty()) {
      path = target_path;
    }

    entries.push_back(GitBranchFileEntry{
        .relative_path = std::filesystem::path(path).lexically_normal(),
        .status = StatusFromDiffCode(status_code[0]),
    });
  }

  std::sort(entries.begin(), entries.end(), [](const GitBranchFileEntry& lhs,
                                               const GitBranchFileEntry& rhs) {
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  return entries;
}

}  // namespace microide::project
