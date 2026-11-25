#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

inline constexpr std::size_t kMaxGitStatusEntries = 50000;

class GitPorcelainParser {
 public:
  static GitFileStatus StatusFromPorcelainCode(std::string_view code);
  static GitFileStatus StatusFromDiffCode(char code);
  static GitFileStatus StatusFromChangeCodeChars(std::string_view code);

  static std::vector<GitWorkingTreeEntry> ParseWorkingTreeEntries(std::string_view output);
  static constexpr std::size_t kDefaultParseLogCap = 100000;
  static std::vector<GitCommitEntry> ParseLog(std::string_view output,
                                              std::size_t max_entries = kDefaultParseLogCap);

  static void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                              std::filesystem::path relative_path,
                              GitFileStatus status);
  static void RecordNormalizedGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                                        std::string_view normalized_generic_path,
                                        GitFileStatus status,
                                        std::string& scratch);

 private:
  static bool StatusUsesTargetPath(std::string_view code);
  static int GitStatusPriority(GitFileStatus status);
  static GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next);
};

}
