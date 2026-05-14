#include "project/GitPorcelainParser.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace microide::project {

namespace {

struct ParsedStatusV1Entry {
  std::filesystem::path relative_path;
  GitFileStatus status = GitFileStatus::Clean;
  bool staged = false;
  bool conflicted = false;
};

bool IsConflictedStatus(std::string_view code) {
  return code.find('U') != std::string_view::npos || code == "AA" || code == "DD" ||
         code == "AU" || code == "UA" || code == "DU" || code == "UD";
}

bool StatusUsesTargetPath(std::string_view code) {
  return code.find('R') != std::string_view::npos || code.find('C') != std::string_view::npos;
}

std::vector<ParsedStatusV1Entry> ParseStatusV1Entries(std::string_view output) {
  std::vector<ParsedStatusV1Entry> entries;

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
    std::string_view path = entry.substr(3);
    if (path.empty()) {
      continue;
    }

    if (StatusUsesTargetPath(code) && offset < output.size()) {
      const std::size_t target_end = output.find('\0', offset);
      const std::size_t resolved_end =
          target_end == std::string_view::npos ? output.size() : target_end;
      const std::string_view target = output.substr(offset, resolved_end - offset);
      if (!target.empty()) {
        path = target;
      }
      offset = resolved_end == output.size() ? output.size() : resolved_end + 1;
    }

    const bool conflicted = IsConflictedStatus(code);
    entries.push_back(ParsedStatusV1Entry{
        .relative_path = std::filesystem::path(path).lexically_normal(),
        .status = GitPorcelainParser::StatusFromPorcelainCode(code),
        .staged = code.size() >= 2 && code[0] != ' ' && code[0] != '?' && !conflicted,
        .conflicted = conflicted,
    });
  }

  return entries;
}

}  // namespace

GitFileStatus GitPorcelainParser::StatusFromPorcelainCode(std::string_view code) {
  if (code == "??" || code == "?? ") {
    return GitFileStatus::Untracked;
  }
  if (IsConflictedStatus(code)) {
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

GitFileStatus GitPorcelainParser::StatusFromDiffCode(char code) {
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

bool GitPorcelainParser::StatusUsesTargetPath(std::string_view code) {
  return ::microide::project::StatusUsesTargetPath(code);
}

int GitPorcelainParser::GitStatusPriority(GitFileStatus status) {
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

GitFileStatus GitPorcelainParser::CombineGitStatus(GitFileStatus current, GitFileStatus next) {
  return GitStatusPriority(next) > GitStatusPriority(current) ? next : current;
}

void GitPorcelainParser::RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                                         std::filesystem::path relative_path,
                                         GitFileStatus status) {
  relative_path = relative_path.lexically_normal();
  const std::string normalized = relative_path.generic_string();
  if (!normalized.empty() && normalized != ".") {
    statuses[normalized] = CombineGitStatus(statuses[normalized], status);
  }

  std::filesystem::path dir = relative_path.parent_path();
  while (!dir.empty() && dir != ".") {
    const std::string key = dir.lexically_normal().generic_string();
    statuses[key] = CombineGitStatus(statuses[key], status);
    const auto next = dir.parent_path();
    if (next == dir) {
      break;
    }
    dir = next;
  }
}

std::unordered_map<std::string, GitFileStatus> GitPorcelainParser::ParseStatusV1(std::string_view output) {
  std::unordered_map<std::string, GitFileStatus> statuses;
  for (const ParsedStatusV1Entry& entry : ParseStatusV1Entries(output)) {
    RecordGitStatus(statuses, entry.relative_path, entry.status);
  }

  return statuses;
}

std::vector<GitWorkingTreeEntry> GitPorcelainParser::ParseWorkingTreeEntries(std::string_view output) {
  std::vector<GitWorkingTreeEntry> entries;
  const std::vector<ParsedStatusV1Entry> parsed_entries = ParseStatusV1Entries(output);
  entries.reserve(parsed_entries.size());
  for (const ParsedStatusV1Entry& entry : parsed_entries) {
    entries.push_back(GitWorkingTreeEntry{
        .relative_path = entry.relative_path,
        .status = entry.status,
        .staged = entry.staged,
        .conflicted = entry.conflicted,
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

std::vector<GitCommitEntry> GitPorcelainParser::ParseLog(std::string_view output) {
  std::vector<GitCommitEntry> commits;
  std::istringstream stream(std::string{output});
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

}  // namespace microide::project
