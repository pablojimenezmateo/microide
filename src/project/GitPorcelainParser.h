#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

// Upper bound on git-status entries materialized from one `git status` capture.
// A hostile repo with millions of untracked/changed files would otherwise
// reserve+parse them all and hand the list to a UI-thread N·logN sort with
// per-comparison path normalization — a multi-minute freeze + heap spike. The
// 128 MiB subprocess-capture cap does not bound the entry count (a `?? f\0`
// record is ~5 bytes → ~25M entries). A changed-file list past this is unusable
// as a sidebar anyway; extra records are dropped. Shared by the v1 and v2 parsers.
inline constexpr std::size_t kMaxGitStatusEntries = 50000;

class GitPorcelainParser {
 public:
  static GitFileStatus StatusFromPorcelainCode(std::string_view code);
  static GitFileStatus StatusFromDiffCode(char code);
  // Shared porcelain change-code precedence (Deleted > Added(A|C) > Modified(M|R|T)
  // > Clean). Conflict and untracked (`??`) states are classified by the caller
  // *before* this, so both the v1 and v2 status mappers agree on the ordinary
  // codes from one source instead of two drifting copies. Note this is distinct
  // from StatusFromDiffCode, where `C` (copy) maps to Modified, not Added.
  static GitFileStatus StatusFromChangeCodeChars(std::string_view code);

  static std::unordered_map<std::string, GitFileStatus> ParseStatusV1(std::string_view output);
  static std::vector<GitWorkingTreeEntry> ParseWorkingTreeEntries(std::string_view output);
  // Parse `git log` porcelain into commit entries. `max_entries` bounds the
  // result so a future caller (or a hostile/corrupt log stream) cannot
  // materialize an unbounded vector; the default is a generous safety cap.
  static constexpr std::size_t kDefaultParseLogCap = 100000;
  static std::vector<GitCommitEntry> ParseLog(std::string_view output,
                                              std::size_t max_entries = kDefaultParseLogCap);

  // Record `relative_path`'s status plus the aggregated folder badge on each of
  // its ancestors. Normalizes the path first; prefer RecordNormalizedGitStatus
  // when the caller already holds a normalized generic path.
  static void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                              std::filesystem::path relative_path,
                              GitFileStatus status);
  // Same, for a path that is ALREADY lexically normal and in generic ('/') form —
  // which is what GitRepositoryPathIdentity stores. Skips a redundant
  // lexically_normal() + generic_string() per entry on the git-refresh hot path.
  //
  // `scratch` is the caller's buffer for the ancestor walk, which shortens one
  // string in place rather than building a path per level. It was a local, i.e.
  // one allocation per changed file on a path a 1,000-file refresh runs 1,000
  // times; hoisting it to the caller's loop makes the walk allocation-free after
  // the first entry. Contents on return are unspecified.
  static void RecordNormalizedGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                                        std::string_view normalized_generic_path,
                                        GitFileStatus status,
                                        std::string& scratch);

 private:
  static bool StatusUsesTargetPath(std::string_view code);
  static int GitStatusPriority(GitFileStatus status);
  static GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next);
};

}  // namespace microide::project
