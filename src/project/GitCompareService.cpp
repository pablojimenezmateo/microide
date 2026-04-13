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
#include "project/GitPorcelainParser.h"
#include "project/GitRepository.h"

namespace microide::project {

namespace {

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

}  // namespace

std::vector<GitCommitEntry> CollectGitFileHistory(const std::filesystem::path& root,
                                                  const std::filesystem::path& absolute_path) {
  const GitRepository repo(root);
  if (absolute_path.empty() || !repo.IsValid()) {
    return {};
  }

  const auto relative = repo.ToRelative(absolute_path);
  if (!relative.has_value()) {
    return {};
  }
  return repo.GetFileHistory(*relative);
}

std::optional<GitFileContentAtCommit> ReadGitFileAtCommit(const std::filesystem::path& root,
                                                          const std::filesystem::path& absolute_path,
                                                          const std::string& hash) {
  if (root.empty() || absolute_path.empty() || hash.empty()) {
    return std::nullopt;
  }

  const GitRepository repo(root);
  const auto relative = repo.ToRelative(absolute_path);
  if (!relative.has_value()) {
    return std::nullopt;
  }

  if (!repo.FileExistsAtRevision(*relative, hash)) {
    return GitFileContentAtCommit{.exists = false, .content = ""};
  }

  const auto content = repo.ReadFileAtRevision(*relative, hash);
  if (!content.has_value()) {
    return std::nullopt;
  }
  return GitFileContentAtCommit{.exists = true, .content = *content};
}

std::optional<GitBranchReference> ResolveGitBaseReference(const std::filesystem::path& root) {
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return std::nullopt;
  }

  const auto origin_head_result = repo.Execute("symbolic-ref --quiet refs/remotes/origin/HEAD");
  const std::string origin_head = TrimTrailingWhitespace(origin_head_result.output);
  if (origin_head_result.success() && !origin_head.empty()) {
    return GitBranchReference{
        .ref = origin_head,
        .label = ShortRefLabel(origin_head),
    };
  }

  const std::array<std::string_view, 2> local_defaults = {"main", "master"};
  for (std::string_view candidate : local_defaults) {
    const auto exists_result =
        repo.Execute("show-ref --verify --quiet 'refs/heads/" + std::string(candidate) + "'");
    if (exists_result.success()) {
      return GitBranchReference{
          .ref = std::string(candidate),
          .label = std::string(candidate),
      };
    }
  }

  const auto upstream_result =
      repo.Execute("rev-parse --abbrev-ref --symbolic-full-name '@{upstream}'");
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
  const GitRepository repo(root);
  if (base_ref.empty() || !repo.IsValid()) {
    return {};
  }

  const auto result = repo.Execute("diff --name-status --find-renames '" +
                                   microide::project::internal::EscapeShellArg(
                                       std::string(base_ref)) + "...HEAD'");
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
        .status = GitPorcelainParser::StatusFromDiffCode(status_code[0]),
    });
  }

  std::sort(entries.begin(), entries.end(), [](const GitBranchFileEntry& lhs,
                                               const GitBranchFileEntry& rhs) {
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  return entries;
}

}  // namespace microide::project
