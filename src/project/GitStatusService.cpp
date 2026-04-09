#include "project/GitStatusService.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/StartupTrace.h"

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

bool HasGitMarker(const std::filesystem::path& root) {
  return std::filesystem::exists(root / ".git");
}

std::filesystem::path AbsoluteToRelativePath(const std::filesystem::path& root,
                                             const std::filesystem::path& absolute_path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(absolute_path.lexically_normal(), root.lexically_normal(), error);
  if (error || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return {};
  }
  return relative.lexically_normal();
}

bool StatusUsesTargetPath(std::string_view code) {
  return code.find('R') != std::string_view::npos || code.find('C') != std::string_view::npos;
}

int GitStatusPriority(GitFileStatus status) {
  switch (status) {
    case GitFileStatus::Conflicted:
      return 5;
    case GitFileStatus::Deleted:
      return 4;
    case GitFileStatus::Modified:
      return 3;
    case GitFileStatus::Added:
      return 2;
    case GitFileStatus::Untracked:
      return 1;
    case GitFileStatus::Clean:
    default:
      return 0;
  }
}

GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next) {
  return GitStatusPriority(next) > GitStatusPriority(current) ? next : current;
}

GitFileStatus StatusFromPorcelainCode(std::string_view code) {
  if (code == "??") {
    return GitFileStatus::Untracked;
  }
  if (code.find('U') != std::string_view::npos || code == "AA" || code == "DD") {
    return GitFileStatus::Conflicted;
  }
  if (code.find('D') != std::string_view::npos) {
    return GitFileStatus::Deleted;
  }
  if (code.find('A') != std::string_view::npos || code.find('C') != std::string_view::npos) {
    return GitFileStatus::Added;
  }
  if (code.find_first_of("MRTU") != std::string_view::npos) {
    return GitFileStatus::Modified;
  }
  return GitFileStatus::Clean;
}

void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                     std::filesystem::path relative_path,
                     GitFileStatus status) {
  relative_path = relative_path.lexically_normal();
  const std::string normalized = relative_path.string();
  if (!normalized.empty() && normalized != ".") {
    statuses[normalized] = CombineGitStatus(statuses[normalized], status);
  }

  std::filesystem::path dir = relative_path.parent_path();
  while (!dir.empty() && dir != ".") {
    const std::string key = dir.lexically_normal().string();
    statuses[key] = CombineGitStatus(statuses[key], status);
    const auto next = dir.parent_path();
    if (next == dir) {
      break;
    }
    dir = next;
  }
}

std::unordered_map<std::string, GitFileStatus> ParseGitPorcelainStatus(std::string_view output) {
  std::unordered_map<std::string, GitFileStatus> statuses;

  std::size_t offset = 0;
  while (offset < output.size()) {
    const std::size_t end = output.find('\0', offset);
    const std::size_t current_end = end == std::string_view::npos ? output.size() : end;
    const std::string_view entry = output.substr(offset, current_end - offset);
    offset = current_end == output.size() ? output.size() : current_end + 1;

    if (entry.size() < 4) {
      continue;
    }

    const std::string_view code = entry.substr(0, 2);
    std::string path(entry.substr(3));
    if (path.empty()) {
      continue;
    }

    if (StatusUsesTargetPath(code) && offset < output.size()) {
      const std::size_t target_end = output.find('\0', offset);
      const std::size_t resolved_end =
          target_end == std::string_view::npos ? output.size() : target_end;
      const std::string_view target = output.substr(offset, resolved_end - offset);
      if (!target.empty()) {
        path = std::string(target);
      }
      offset = resolved_end == output.size() ? output.size() : resolved_end + 1;
    }

    RecordGitStatus(statuses, std::filesystem::path(path), StatusFromPorcelainCode(code));
  }

  return statuses;
}

std::vector<GitWorkingTreeEntry> ParseGitWorkingTreeEntries(std::string_view output) {
  std::vector<GitWorkingTreeEntry> entries;

  std::size_t offset = 0;
  while (offset < output.size()) {
    const std::size_t end = output.find('\0', offset);
    const std::size_t current_end = end == std::string_view::npos ? output.size() : end;
    const std::string_view entry = output.substr(offset, current_end - offset);
    offset = current_end == output.size() ? output.size() : current_end + 1;

    if (entry.size() < 4) {
      continue;
    }

    const std::string_view code = entry.substr(0, 2);
    std::string path(entry.substr(3));
    if (path.empty()) {
      continue;
    }

    if (StatusUsesTargetPath(code) && offset < output.size()) {
      const std::size_t target_end = output.find('\0', offset);
      const std::size_t resolved_end =
          target_end == std::string_view::npos ? output.size() : target_end;
      const std::string_view target = output.substr(offset, resolved_end - offset);
      if (!target.empty()) {
        path = std::string(target);
      }
      offset = resolved_end == output.size() ? output.size() : resolved_end + 1;
    }

    const bool conflicted =
        code.find('U') != std::string_view::npos || code == "AA" || code == "DD";
    const bool staged =
        code.size() >= 2 && code[0] != ' ' && code[0] != '?' && !conflicted;
    entries.push_back(GitWorkingTreeEntry{
        .relative_path = std::filesystem::path(path).lexically_normal(),
        .status = StatusFromPorcelainCode(code),
        .staged = staged,
        .conflicted = conflicted,
    });
  }

  std::sort(entries.begin(), entries.end(), [](const GitWorkingTreeEntry& lhs,
                                               const GitWorkingTreeEntry& rhs) {
    if (lhs.staged != rhs.staged) {
      return lhs.staged > rhs.staged;
    }
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  return entries;
}

std::string ReadCommandOutput(const std::string& command) {
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return output;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const std::size_t bytes_read = fread(buffer.data(), 1, buffer.size(), pipe);
    if (bytes_read > 0) {
      output.append(buffer.data(), bytes_read);
    }
    if (bytes_read < buffer.size()) {
      break;
    }
  }

  const int status = pclose(pipe);
  if (status != 0) {
    return {};
  }

  return output;
}

bool CommandSucceeds(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return false;
  }
  std::array<char, 256> buffer{};
  while (fread(buffer.data(), 1, buffer.size(), pipe) > 0) {
  }
  return pclose(pipe) == 0;
}

bool FileExistsAtHead(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
  const std::string spec = "HEAD:" + relative_path.generic_string();
  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' cat-file -e '" + EscapeShellArg(spec) + "' 2>/dev/null";
  return CommandSucceeds(command);
}

bool HasHeadCommit(const std::filesystem::path& root) {
  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' rev-parse --verify HEAD >/dev/null 2>/dev/null";
  return CommandSucceeds(command);
}

}  // namespace

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("CollectGitStatuses");
  if (root.empty() || !HasGitMarker(root)) {
    return {};
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' status --porcelain=v1 -z --untracked-files=all 2>/dev/null";
  const std::string output = ReadCommandOutput(command);
  if (output.empty()) {
    return {};
  }

  return ParseGitPorcelainStatus(output);
}

std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root) {
  if (root.empty() || !HasGitMarker(root)) {
    return {};
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' status --porcelain=v1 -z --untracked-files=all 2>/dev/null";
  const std::string output = ReadCommandOutput(command);
  if (output.empty()) {
    return {};
  }
  return ParseGitWorkingTreeEntries(output);
}

bool GitStageAll(const std::filesystem::path& root) {
  if (root.empty() || !HasGitMarker(root)) {
    return false;
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' add -A -- . >/dev/null 2>/dev/null";
  return CommandSucceeds(command);
}

bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !HasGitMarker(root)) {
    return false;
  }

  const std::filesystem::path relative_path = AbsoluteToRelativePath(root, absolute_path);
  if (relative_path.empty()) {
    return false;
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) + "' add -- '" +
      EscapeShellArg(relative_path.generic_string()) + "' >/dev/null 2>/dev/null";
  return CommandSucceeds(command);
}

bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !HasGitMarker(root)) {
    return false;
  }

  const std::filesystem::path relative_path = AbsoluteToRelativePath(root, absolute_path);
  if (relative_path.empty()) {
    return false;
  }

  const std::string escaped_root = EscapeShellArg(root.lexically_normal().string());
  const std::string escaped_relative = EscapeShellArg(relative_path.generic_string());
  const std::string command =
      FileExistsAtHead(root, relative_path)
          ? "git -C '" + escaped_root + "' restore --staged -- '" + escaped_relative +
                "' >/dev/null 2>/dev/null"
          : "git -C '" + escaped_root + "' rm --cached -- '" + escaped_relative +
                "' >/dev/null 2>/dev/null";
  return CommandSucceeds(command);
}

bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !HasGitMarker(root)) {
    return false;
  }

  const std::filesystem::path relative_path = AbsoluteToRelativePath(root, absolute_path);
  if (relative_path.empty()) {
    return false;
  }

  const std::string escaped_root = EscapeShellArg(root.lexically_normal().string());
  const std::string escaped_relative = EscapeShellArg(relative_path.generic_string());
  if (FileExistsAtHead(root, relative_path)) {
    const std::string command =
        "git -C '" + escaped_root + "' restore --source=HEAD --staged --worktree -- '" +
        escaped_relative + "' >/dev/null 2>/dev/null";
    return CommandSucceeds(command);
  }

  const std::string unstage_command =
      "git -C '" + escaped_root + "' rm -f --cached --ignore-unmatch -- '" + escaped_relative +
      "' >/dev/null 2>/dev/null";
  const std::string clean_command =
      "git -C '" + escaped_root + "' clean -fd -- '" + escaped_relative + "' >/dev/null 2>/dev/null";
  return CommandSucceeds(unstage_command) && CommandSucceeds(clean_command);
}

bool GitDiscardAll(const std::filesystem::path& root) {
  if (root.empty() || !HasGitMarker(root)) {
    return false;
  }

  const std::string escaped_root = EscapeShellArg(root.lexically_normal().string());
  if (HasHeadCommit(root)) {
    const std::string reset_command =
        "git -C '" + escaped_root + "' reset --quiet HEAD -- . >/dev/null 2>/dev/null";
    const std::string restore_command =
        "git -C '" + escaped_root + "' restore --source=HEAD --worktree -- . >/dev/null 2>/dev/null";
    const std::string clean_command =
        "git -C '" + escaped_root + "' clean -fd -- . >/dev/null 2>/dev/null";
    return CommandSucceeds(reset_command) && CommandSucceeds(restore_command) &&
           CommandSucceeds(clean_command);
  }

  const std::string unstage_command =
      "git -C '" + escaped_root + "' rm -r -f --cached --ignore-unmatch -- . >/dev/null 2>/dev/null";
  const std::string clean_command =
      "git -C '" + escaped_root + "' clean -fd -- . >/dev/null 2>/dev/null";
  return CommandSucceeds(unstage_command) && CommandSucceeds(clean_command);
}

}  // namespace microide::project
