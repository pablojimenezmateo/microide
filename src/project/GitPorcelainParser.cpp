#include "project/GitPorcelainParser.h"

#include <algorithm>
#include <array>
#include <filesystem>

#include "util/StringUtil.h"

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

  const std::vector<std::string_view> records = util::SplitNulDelimited(output);
  for (std::size_t i = 0; i < records.size(); ++i) {
    const std::string_view entry = records[i];
    if (entry.size() < 4) {
      continue;
    }

    const std::string_view code = entry.substr(0, 2);
    std::string_view path = entry.substr(3);
    if (path.empty()) {
      continue;
    }

    // Rename/copy status codes are followed by the source path in the next
    // NUL-delimited record; the destination path stays in `entry`.
    if (StatusUsesTargetPath(code) && i + 1 < records.size()) {
      const std::string_view target = records[i + 1];
      if (!target.empty()) {
        path = target;
      }
      ++i;
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

GitFileStatus GitPorcelainParser::StatusFromChangeCodeChars(std::string_view code) {
  if (code.find('D') != std::string_view::npos) {
    return GitFileStatus::Deleted;
  }
  if (code.find('A') != std::string_view::npos || code.find('C') != std::string_view::npos) {
    return GitFileStatus::Added;
  }
  if (code.find_first_of("MRT") != std::string_view::npos) {
    return GitFileStatus::Modified;
  }
  return GitFileStatus::Clean;
}

GitFileStatus GitPorcelainParser::StatusFromPorcelainCode(std::string_view code) {
  if (code == "??" || code == "?? ") {
    return GitFileStatus::Untracked;
  }
  if (IsConflictedStatus(code)) {
    return GitFileStatus::Conflicted;
  }
  // Conflict ('U') codes are handled above, so the ordinary precedence suffices.
  return StatusFromChangeCodeChars(code);
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
    // Hoist the slot reference so operator[] runs once, not twice (read + assign).
    GitFileStatus& slot = statuses[normalized];
    slot = CombineGitStatus(slot, status);
  }

  std::filesystem::path dir = relative_path.parent_path();
  while (!dir.empty() && dir != ".") {
    const std::string key = dir.lexically_normal().generic_string();
    GitFileStatus& slot = statuses[key];
    slot = CombineGitStatus(slot, status);
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
    // native() returns a const reference; generic_string() would allocate two
    // throwaway strings per comparison. Ordering is for display stability only.
    return lhs.relative_path.native() < rhs.relative_path.native();
  });
  return entries;
}

std::vector<GitCommitEntry> GitPorcelainParser::ParseLog(std::string_view output) {
  // Expected line layout (tab-separated, subject last so it may contain tabs):
  //   <hash>\t<short_hash>\t<author>\t<relative_date>\t<subject>
  std::vector<GitCommitEntry> commits;
  std::size_t line_start = 0;
  while (line_start < output.size()) {
    const std::size_t newline = output.find('\n', line_start);
    const std::string_view line =
        output.substr(line_start, (newline == std::string_view::npos ? output.size() : newline) -
                                       line_start);
    line_start = newline == std::string_view::npos ? output.size() : newline + 1;
    if (line.empty()) {
      continue;
    }
    std::array<std::size_t, 4> tabs{};
    std::size_t search = 0;
    bool well_formed = true;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      const std::size_t pos = line.find('\t', search);
      if (pos == std::string_view::npos) {
        well_formed = false;
        break;
      }
      tabs[i] = pos;
      search = pos + 1;
    }
    if (!well_formed) {
      continue;
    }
    commits.push_back(GitCommitEntry{
        .hash = std::string(line.substr(0, tabs[0])),
        .short_hash = std::string(line.substr(tabs[0] + 1, tabs[1] - tabs[0] - 1)),
        .subject = std::string(line.substr(tabs[3] + 1)),
        .author = std::string(line.substr(tabs[1] + 1, tabs[2] - tabs[1] - 1)),
        .relative_date = std::string(line.substr(tabs[2] + 1, tabs[3] - tabs[2] - 1)),
    });
  }
  return commits;
}

}  // namespace microide::project
