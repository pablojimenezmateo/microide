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

  static std::unordered_map<std::string, GitFileStatus> ParseStatusV1(std::string_view output);
  static std::vector<GitWorkingTreeEntry> ParseWorkingTreeEntries(std::string_view output);
  static std::vector<GitCommitEntry> ParseLog(std::string_view output);

 private:
  static bool StatusUsesTargetPath(std::string_view code);
  static int GitStatusPriority(GitFileStatus status);
  static GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next);
  static void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                              std::filesystem::path relative_path,
                              GitFileStatus status);
};

}  // namespace microide::project
