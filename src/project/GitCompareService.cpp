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

std::optional<std::string> ReadTrimmedGitValue(const GitRepository& repo,
                                               std::initializer_list<std::string_view> arguments) {
  const auto result = repo.Execute(arguments);
  const std::string value = TrimTrailingWhitespace(result.output);
  if (!result.success() || value.empty()) {
    return std::nullopt;
  }
  return value;
}

bool GitRefExists(const GitRepository& repo, std::string_view ref) {
  if (ref.empty()) {
    return false;
  }
  return repo.ExecuteSucceeds({"show-ref", "--verify", "--quiet", std::string(ref)});
}

std::optional<GitBranchReference> ResolveNamedBranchReference(const GitRepository& repo,
                                                              std::string_view branch_name,
                                                              std::string_view remote_name) {
  if (branch_name.empty()) {
    return std::nullopt;
  }

  if (!remote_name.empty()) {
    const std::string remote_ref =
        "refs/remotes/" + std::string(remote_name) + "/" + std::string(branch_name);
    if (GitRefExists(repo, remote_ref)) {
      return GitBranchReference{
          .ref = remote_ref,
          .label = ShortRefLabel(remote_ref),
      };
    }
  }

  const std::string origin_ref = "refs/remotes/origin/" + std::string(branch_name);
  if (GitRefExists(repo, origin_ref)) {
    return GitBranchReference{
        .ref = origin_ref,
        .label = ShortRefLabel(origin_ref),
    };
  }

  const std::string local_ref = "refs/heads/" + std::string(branch_name);
  if (GitRefExists(repo, local_ref)) {
    return GitBranchReference{
        .ref = local_ref,
        .label = std::string(branch_name),
    };
  }

  if (GitRefExists(repo, branch_name)) {
    return GitBranchReference{
        .ref = std::string(branch_name),
        .label = ShortRefLabel(branch_name),
    };
  }

  return std::nullopt;
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

std::vector<GitCommitEntry> CollectGitRecentCommits(const std::filesystem::path& root,
                                                    std::size_t limit) {
  const GitRepository repo(root);
  if (limit == 0 || !repo.IsValid() || !repo.HasHeadCommit()) {
    return {};
  }
  const auto result = repo.Execute(std::vector<std::string>{
      "log", "--no-color", "-n", std::to_string(limit),
      "--pretty=format:%H%x09%h%x09%an%x09%ar%x09%s", "HEAD"});
  if (!result.success()) {
    return {};
  }
  return GitPorcelainParser::ParseLog(result.output);
}

std::vector<GitBranchReference> CollectGitBranches(const std::filesystem::path& root) {
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return {};
  }
  const auto result = repo.Execute(std::vector<std::string>{
      "for-each-ref", "--format=%(refname)", "--sort=-committerdate", "refs/heads",
      "refs/remotes"});
  if (!result.success()) {
    return {};
  }

  std::vector<GitBranchReference> branches;
  std::istringstream stream(result.output);
  std::string line;
  while (std::getline(stream, line)) {
    const std::string ref = TrimTrailingWhitespace(line);
    if (ref.empty()) {
      continue;
    }
    std::string label = ShortRefLabel(ref);
    // Skip symbolic remote heads (e.g. origin/HEAD -> origin/main) so the list
    // shows real branches only.
    const bool is_symbolic_head =
        label.size() >= 5 && label.compare(label.size() - 5, 5, "/HEAD") == 0;
    if (label.empty() || label == "origin/HEAD" || is_symbolic_head) {
      continue;
    }
    branches.push_back(GitBranchReference{.ref = label, .label = std::move(label)});
  }
  return branches;
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

  const std::optional<std::string> current_branch =
      ReadTrimmedGitValue(repo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
  if (current_branch.has_value()) {
    const std::string merge_base_key = "branch." + *current_branch + ".gh-merge-base";
    const std::optional<std::string> pr_base =
        ReadTrimmedGitValue(repo, {"config", "--get", merge_base_key});
    if (pr_base.has_value()) {
      const std::string remote_key = "branch." + *current_branch + ".remote";
      const std::optional<std::string> branch_remote =
          ReadTrimmedGitValue(repo, {"config", "--get", remote_key});
      if (const auto pr_base_ref =
              ResolveNamedBranchReference(repo, *pr_base,
                                          branch_remote.value_or(std::string{}));
          pr_base_ref.has_value()) {
        return pr_base_ref;
      }
    }
  }

  const auto origin_head_result =
      repo.Execute({"symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"});
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
        repo.Execute({"show-ref", "--verify", "--quiet",
                      "refs/heads/" + std::string(candidate)});
    if (exists_result.success()) {
      return GitBranchReference{
          .ref = std::string(candidate),
          .label = std::string(candidate),
      };
    }
  }

  const auto upstream_result =
      repo.Execute({"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
  const std::string upstream = TrimTrailingWhitespace(upstream_result.output);
  if (upstream_result.success() && !upstream.empty()) {
    return GitBranchReference{
        .ref = upstream,
        .label = ShortRefLabel(upstream),
    };
  }

  return std::nullopt;
}

std::vector<GitBranchFileEntry> ParseGitBranchDiffNameStatusZ(std::string_view output) {
  std::vector<GitBranchFileEntry> entries;
  std::size_t pos = 0;
  const auto next_token = [&](std::string_view& token) -> bool {
    if (pos >= output.size()) {
      return false;
    }
    const std::size_t nul = output.find('\0', pos);
    if (nul == std::string_view::npos) {
      token = output.substr(pos);
      pos = output.size();
      return !token.empty();
    }
    token = output.substr(pos, nul - pos);
    pos = nul + 1;
    return true;
  };

  std::string_view status_token;
  while (next_token(status_token)) {
    if (status_token.empty()) {
      continue;
    }
    const char code = status_token.front();
    std::string_view path_token;
    if (!next_token(path_token)) {
      break;
    }
    // Rename/copy records carry the old path then the new path; report the new one.
    if (code == 'R' || code == 'C') {
      std::string_view new_path;
      if (next_token(new_path) && !new_path.empty()) {
        path_token = new_path;
      }
    }
    if (path_token.empty()) {
      continue;
    }
    entries.push_back(GitBranchFileEntry{
        .relative_path = std::filesystem::path(std::string(path_token)).lexically_normal(),
        .status = GitPorcelainParser::StatusFromDiffCode(code),
    });
  }

  std::sort(entries.begin(), entries.end(), [](const GitBranchFileEntry& lhs,
                                               const GitBranchFileEntry& rhs) {
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  return entries;
}

std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(const std::filesystem::path& root,
                                                              std::string_view base_ref) {
  const GitRepository repo(root);
  if (base_ref.empty() || !repo.IsValid()) {
    return {};
  }

  // `-z` makes paths NUL-delimited so paths containing spaces and rename records
  // (status NUL old NUL new) parse correctly; the prior whitespace-split parser
  // truncated spaced paths and mis-attributed renames.
  const auto result = repo.Execute(
      {"diff", "--name-status", "-z", "--find-renames", std::string(base_ref) + "...HEAD"});
  if (!result.success() || result.output.empty()) {
    return {};
  }

  return ParseGitBranchDiffNameStatusZ(result.output);
}

std::vector<std::filesystem::path> CollectGitCommitChangedFiles(const std::filesystem::path& root,
                                                                std::string_view commit_hash) {
  const GitRepository repo(root);
  if (commit_hash.empty() || !repo.IsValid()) {
    return {};
  }

  const auto result =
      repo.Execute({"diff-tree", "--no-commit-id", "--name-only", "-r", std::string(commit_hash)});
  if (!result.success() || result.output.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> paths;
  std::istringstream stream(result.output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    paths.push_back(std::filesystem::path(line).lexically_normal());
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

}  // namespace microide::project
