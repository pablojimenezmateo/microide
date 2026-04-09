#include "project/GitStatusService.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitCommandUtil.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

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

bool FileExistsAtHead(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
  const std::string spec = "HEAD:" + relative_path.generic_string();
  const std::string command =
      gitutil::BuildGitCommand(root, "cat-file -e '" + gitutil::EscapeShellArg(spec) + "'");
  return gitutil::CommandSucceeds(command);
}

bool HasHeadCommit(const std::filesystem::path& root) {
  const std::string command = gitutil::BuildGitCommand(root, "rev-parse --verify HEAD >/dev/null");
  return gitutil::CommandSucceeds(command);
}

}  // namespace

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("CollectGitStatuses");
  if (root.empty() || !gitutil::HasGitMarker(root)) {
    return {};
  }

  const auto result = gitutil::ReadCommandOutput(
      gitutil::BuildGitCommand(root, "status --porcelain=v1 -z --untracked-files=all"));
  if (!result.success() || result.output.empty()) {
    return {};
  }

  return ParseGitPorcelainStatus(result.output);
}

std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root) {
  if (root.empty() || !gitutil::HasGitMarker(root)) {
    return {};
  }

  const auto result = gitutil::ReadCommandOutput(
      gitutil::BuildGitCommand(root, "status --porcelain=v1 -z --untracked-files=all"));
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return ParseGitWorkingTreeEntries(result.output);
}

bool GitStageAll(const std::filesystem::path& root) {
  if (root.empty() || !gitutil::HasGitMarker(root)) {
    return false;
  }

  const std::string command = gitutil::BuildGitCommand(root, "add -A -- . >/dev/null");
  return gitutil::CommandSucceeds(command);
}

bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !gitutil::HasGitMarker(root)) {
    return false;
  }

  const auto relative_path = gitutil::AbsoluteToRelativePath(root, absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }

  const std::string command = gitutil::BuildGitCommand(
      root, "add -- '" + gitutil::EscapeShellArg(relative_path->generic_string()) + "' >/dev/null");
  return gitutil::CommandSucceeds(command);
}

bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !gitutil::HasGitMarker(root)) {
    return false;
  }

  const auto relative_path = gitutil::AbsoluteToRelativePath(root, absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }

  const std::string escaped_relative = gitutil::EscapeShellArg(relative_path->generic_string());
  const std::string command =
      FileExistsAtHead(root, *relative_path)
          ? gitutil::BuildGitCommand(root, "restore --staged -- '" + escaped_relative + "' >/dev/null")
          : gitutil::BuildGitCommand(root, "rm --cached -- '" + escaped_relative + "' >/dev/null");
  return gitutil::CommandSucceeds(command);
}

bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty() || !gitutil::HasGitMarker(root)) {
    return false;
  }

  const auto relative_path = gitutil::AbsoluteToRelativePath(root, absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }

  const std::string escaped_relative = gitutil::EscapeShellArg(relative_path->generic_string());
  if (FileExistsAtHead(root, *relative_path)) {
    const std::string command = gitutil::BuildGitCommand(
        root, "restore --source=HEAD --staged --worktree -- '" + escaped_relative + "' >/dev/null");
    return gitutil::CommandSucceeds(command);
  }

  const std::string unstage_command = gitutil::BuildGitCommand(
      root, "rm -f --cached --ignore-unmatch -- '" + escaped_relative + "' >/dev/null");
  const std::string clean_command =
      gitutil::BuildGitCommand(root, "clean -fd -- '" + escaped_relative + "' >/dev/null");
  return gitutil::CommandSucceeds(unstage_command) && gitutil::CommandSucceeds(clean_command);
}

bool GitDiscardAll(const std::filesystem::path& root) {
  if (root.empty() || !gitutil::HasGitMarker(root)) {
    return false;
  }

  if (HasHeadCommit(root)) {
    const std::string reset_command =
        gitutil::BuildGitCommand(root, "reset --quiet HEAD -- . >/dev/null");
    const std::string restore_command =
        gitutil::BuildGitCommand(root, "restore --source=HEAD --worktree -- . >/dev/null");
    const std::string clean_command = gitutil::BuildGitCommand(root, "clean -fd -- . >/dev/null");
    return gitutil::CommandSucceeds(reset_command) && gitutil::CommandSucceeds(restore_command) &&
           gitutil::CommandSucceeds(clean_command);
  }

  const std::string unstage_command =
      gitutil::BuildGitCommand(root, "rm -r -f --cached --ignore-unmatch -- . >/dev/null");
  const std::string clean_command = gitutil::BuildGitCommand(root, "clean -fd -- . >/dev/null");
  return gitutil::CommandSucceeds(unstage_command) && gitutil::CommandSucceeds(clean_command);
}

}  // namespace microide::project
