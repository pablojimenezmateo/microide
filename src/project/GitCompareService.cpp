#include "project/GitCompareService.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"
#include "project/GitRepository.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

// Upper bound on entries collected from an untrusted repo's ref / diff output
// (branch picker, changed-file lists feeding CompareModel). A hostile repo with
// millions of refs or a commit touching millions of files would otherwise
// materialize + sort them all on the UI thread. Bounded only by the 128 MiB
// capture cap otherwise, which is far too large for these list surfaces.
constexpr std::size_t kMaxGitCollectionEntries = 50000;

// Helper-level cap on `git log -n <limit>` for the recent-commit surface. The
// caller-provided limit flows straight into `git log` and then into an
// unbounded ParseLog, so a hostile or buggy caller could request millions of
// commits and materialize them all on the UI thread. The recent-commit picker
// asks for ~50; 1000 leaves generous headroom while bounding worst-case work.
constexpr std::size_t kMaxRecentCommits = 1000;

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
  const std::string value = util::TrimAsciiWhitespace(result.output);
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

GitFileHistoryResult CollectGitFileHistory(const std::filesystem::path& root,
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
  if (limit == 0 || !repo.IsValid()) {
    return {};
  }
  const std::size_t effective_limit = std::min(limit, kMaxRecentCommits);
  // No separate `rev-parse --verify HEAD` pre-check: on an unborn branch `git log
  // HEAD` exits non-zero and we already fall through to the empty return below, so
  // the extra spawn was pure latency on every recent-commit query (Execute
  // silences stderr by default, so the unborn-branch fatal never leaks).
  const auto result = repo.Execute(std::vector<std::string>{
      "log", "--no-color", "-n", std::to_string(effective_limit),
      "--pretty=format:%H%x1f%h%x1f%an%x1f%ar%x1f%s", "HEAD"});
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
  // Bound line materialization at the retained cap (plus small slack for filtered
  // symbolic-head/empty lines) so a huge for-each-ref listing cannot build millions of
  // line views before the branch cap stops collection. (TD-2026-07-16-30.)
  for (const std::string_view ref :
       util::SplitLineViews(result.output, kMaxGitCollectionEntries + 8)) {
    if (branches.size() >= kMaxGitCollectionEntries) {
      break;
    }
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
    // Keep the FULL ref (refs/heads/… or refs/remotes/origin/…) as the identity and
    // the short form only as the display label. Using the short label as the ref
    // (e.g. "origin/main") could resolve to the wrong target when a local branch
    // and a remote-tracking name collide; the full ref is unambiguous. This matches
    // the local/remote GitBranchReference builders above.
    branches.push_back(GitBranchReference{.ref = std::string(ref), .label = std::move(label)});
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

  const auto blob = repo.ReadBlobAtRevision(*relative, hash);
  if (!blob.has_value()) {
    return std::nullopt;
  }
  return GitFileContentAtCommit{
      .exists = true, .content = blob->content, .truncated = blob->truncated};
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
  const std::string origin_head = util::TrimAsciiWhitespace(origin_head_result.output);
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
  const std::string upstream = util::TrimAsciiWhitespace(upstream_result.output);
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
  // Each retained entry consumes up to 3 NUL records (status + old-path + new-path for
  // rename/copy), so bound token materialization at 3x the entry cap (+ slack) instead
  // of splitting every record of hostile NUL-heavy diff output. (TD-2026-07-16-30.)
  const std::vector<std::string_view> tokens =
      util::SplitNulDelimited(output, kMaxGitCollectionEntries * 3 + 2);
  for (std::size_t i = 0; i < tokens.size() && entries.size() < kMaxGitCollectionEntries;) {
    const std::string_view status_token = tokens[i++];
    if (status_token.empty()) {
      continue;
    }
    if (i >= tokens.size()) {
      break;
    }
    const char code = status_token.front();
    std::string_view path_token = tokens[i++];
    // Rename/copy records carry the old path then the new path; report the new one.
    if ((code == 'R' || code == 'C') && i < tokens.size()) {
      if (!tokens[i].empty()) {
        path_token = tokens[i];
      }
      ++i;
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
    // native() is a const reference; generic_string() would allocate per comparison.
    return lhs.relative_path.native() < rhs.relative_path.native();
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
  // `--end-of-options` guards a user-typed SpecificRef base (custom_ref) that may
  // begin with `-`: without it git parses `--output=...` etc. as an option rather
  // than a revision. Matches the `--` discipline used elsewhere in this subsystem.
  const auto result = repo.Execute({"diff", "--name-status", "-z", "--find-renames",
                                    "--end-of-options", std::string(base_ref) + "...HEAD"});
  if (!result.success() || result.output.empty()) {
    return {};
  }

  return ParseGitBranchDiffNameStatusZ(result.output);
}

std::vector<GitBranchFileEntry> CollectGitWorkingTreeDiffFiles(const std::filesystem::path& root,
                                                              std::string_view ref) {
  const GitRepository repo(root);
  if (ref.empty() || !repo.IsValid()) {
    return {};
  }

  // Two-dot diff (no `...`): compares `ref` against the working tree, so it
  // includes uncommitted edits — i.e. "what is different between local state and
  // <ref>". `-z` keeps spaced paths and rename records intact.
  const auto result = repo.Execute({"diff", "--name-status", "-z", "--find-renames",
                                    "--end-of-options", std::string(ref)});
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

  // `-z`: NUL-delimited, unquoted output. Without it a path containing a newline
  // splits into bogus entries and non-ASCII names come back C-quoted, so lookups by
  // the real path fail (and the wrong file gets opened/diffed/staged).
  // `--end-of-options` guards a commit_hash that could begin with `-` (matches the
  // `--` discipline used by CollectGitBranchOutgoingFiles/WorkingTreeDiffFiles).
  const auto result = repo.Execute({"diff-tree", "--no-commit-id", "--name-only", "-r", "-z",
                                    "--end-of-options", std::string(commit_hash)});
  if (!result.success() || result.output.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> paths;
  // One record per path: bound token materialization at the entry cap (+ slack for
  // empty records) rather than splitting every record of a huge commit. (TD-30.)
  for (const std::string_view record :
       util::SplitNulDelimited(result.output, kMaxGitCollectionEntries + 8)) {
    if (paths.size() >= kMaxGitCollectionEntries) {
      break;
    }
    if (record.empty()) {
      continue;
    }
    paths.push_back(std::filesystem::path(record).lexically_normal());
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

}  // namespace microide::project
