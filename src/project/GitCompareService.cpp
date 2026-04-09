#include "project/GitCompareService.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitCommandUtil.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

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
  if (root.empty() || absolute_path.empty() || !gitutil::HasGitMarker(root)) {
    return {};
  }

  const auto relative = gitutil::AbsoluteToRelativePath(root, absolute_path);
  if (!relative.has_value()) {
    return {};
  }

  const std::string command = gitutil::BuildGitCommand(
      root, "log --follow --no-color --pretty=format:%H%x09%h%x09%s -- '" +
                gitutil::EscapeShellArg(relative->generic_string()) + "'");
  const gitutil::CommandResult result = gitutil::ReadCommandOutput(command);
  if (!result.success() || result.output.empty()) {
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

  const auto relative = gitutil::AbsoluteToRelativePath(root, absolute_path);
  if (!relative.has_value()) {
    return std::nullopt;
  }

  const std::string spec = hash + ":" + relative->generic_string();
  const std::string exists_command =
      gitutil::BuildGitCommand(root, "cat-file -e '" + gitutil::EscapeShellArg(spec) + "'");
  const gitutil::CommandResult exists_result = gitutil::ReadCommandOutput(exists_command);
  if (!exists_result.success()) {
    return GitFileContentAtCommit{.exists = false, .content = ""};
  }

  const std::string show_command =
      gitutil::BuildGitCommand(root, "show '" + gitutil::EscapeShellArg(spec) + "'");
  const gitutil::CommandResult show_result = gitutil::ReadCommandOutput(show_command);
  if (!show_result.success()) {
    return std::nullopt;
  }

  return GitFileContentAtCommit{.exists = true, .content = std::move(show_result.output)};
}

std::optional<GitBranchReference> ResolveGitBaseReference(const std::filesystem::path& root) {
  if (root.empty() || !gitutil::HasGitMarker(root)) {
    return std::nullopt;
  }

  const std::string origin_head_command =
      gitutil::BuildGitCommand(root, "symbolic-ref --quiet refs/remotes/origin/HEAD");
  const gitutil::CommandResult origin_head_result = gitutil::ReadCommandOutput(origin_head_command);
  const std::string origin_head = TrimTrailingWhitespace(origin_head_result.output);
  if (origin_head_result.success() && !origin_head.empty()) {
    return GitBranchReference{
        .ref = origin_head,
        .label = ShortRefLabel(origin_head),
    };
  }

  const std::array<std::string_view, 2> local_defaults = {"main", "master"};
  for (std::string_view candidate : local_defaults) {
    const std::string exists_command = gitutil::BuildGitCommand(
        root, "show-ref --verify --quiet 'refs/heads/" + std::string(candidate) + "'");
    const gitutil::CommandResult exists_result = gitutil::ReadCommandOutput(exists_command);
    if (exists_result.success()) {
      return GitBranchReference{
          .ref = std::string(candidate),
          .label = std::string(candidate),
      };
    }
  }

  const std::string upstream_command = gitutil::BuildGitCommand(
      root, "rev-parse --abbrev-ref --symbolic-full-name '@{upstream}'");
  const gitutil::CommandResult upstream_result = gitutil::ReadCommandOutput(upstream_command);
  const std::string upstream = TrimTrailingWhitespace(upstream_result.output);
  if (upstream_result.success() && !upstream.empty()) {
    return GitBranchReference{
        .ref = upstream,
        .label = ShortRefLabel(upstream),
    };
  }

  return std::nullopt;
}

std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(const std::filesystem::path& root,
                                                              std::string_view base_ref) {
  if (root.empty() || base_ref.empty() || !gitutil::HasGitMarker(root)) {
    return {};
  }

  const std::string command = gitutil::BuildGitCommand(
      root, "diff --name-status --find-renames '" +
                gitutil::EscapeShellArg(std::string(base_ref)) + "...HEAD'");
  const gitutil::CommandResult result = gitutil::ReadCommandOutput(command);
  if (!result.success() || result.output.empty()) {
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
