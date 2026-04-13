#include "project/GitStatusService.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

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

  return GitPorcelainParser::ParseStatusV1(result.output);
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
  return GitPorcelainParser::ParseWorkingTreeEntries(result.output);
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
