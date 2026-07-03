#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

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
  static std::vector<GitCommitEntry> ParseLog(std::string_view output);

  static void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                              std::filesystem::path relative_path,
                              GitFileStatus status);

 private:
  static bool StatusUsesTargetPath(std::string_view code);
  static int GitStatusPriority(GitFileStatus status);
  static GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next);
};

}  // namespace microide::project
