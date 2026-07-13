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
  for (std::size_t i = 0; i < records.size() && entries.size() < kMaxGitStatusEntries; ++i) {
    const std::string_view entry = records[i];
    if (entry.size() < 4) {
      continue;
    }

    const std::string_view code = entry.substr(0, 2);
    const std::string_view path = entry.substr(3);
    if (path.empty()) {
      continue;
    }

    // Rename/copy status codes emit two NUL-delimited records: git writes the
    // destination first (in `entry`, right after the code) and the source path
    // in the following record ("<code> <new>\0<old>"). Keep `path` pointing at
    // the destination and consume the source record so it is not parsed as its
    // own entry.
    if (StatusUsesTargetPath(code) && i + 1 < records.size()) {
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

  // Cap ancestor-badge propagation depth. A hostile repo can contain a single
  // pathologically deep path (`a/a/.../x`); walking every ancestor and retaining
  // a map key per level is O(depth) string allocations per entry — quadratic in
  // path length, an OOM. Folder badges beyond a modest depth are not usefully
  // visible anyway. `relative_path` is already normalized, so each parent_path
  // stays normalized without the (previously per-step) lexically_normal() call.
  constexpr int kMaxBadgeAncestorDepth = 64;
  std::filesystem::path dir = relative_path.parent_path();
  for (int depth = 0; depth < kMaxBadgeAncestorDepth && !dir.empty() && dir != ".";
       ++depth) {
    GitFileStatus& slot = statuses[dir.generic_string()];
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

std::vector<GitCommitEntry> GitPorcelainParser::ParseLog(std::string_view output,
                                                         std::size_t max_entries) {
  // Expected line layout, unit-separator (US, 0x1f) delimited, subject last so it
  // may contain the delimiter (it never realistically does):
  //   <hash>\x1f<short_hash>\x1f<author>\x1f<relative_date>\x1f<subject>
  // US is used instead of a tab because a git author name (%an) can legitimately
  // contain a literal tab, which would shift every subsequent field; US cannot.
  constexpr char kSep = '\x1f';
  std::vector<GitCommitEntry> commits;
  std::size_t line_start = 0;
  while (line_start < output.size() && commits.size() < max_entries) {
    const std::size_t newline = output.find('\n', line_start);
    const std::string_view line =
        output.substr(line_start, (newline == std::string_view::npos ? output.size() : newline) -
                                       line_start);
    line_start = newline == std::string_view::npos ? output.size() : newline + 1;
    if (line.empty()) {
      continue;
    }
    std::array<std::size_t, 4> seps{};
    std::size_t search = 0;
    bool well_formed = true;
    for (std::size_t i = 0; i < seps.size(); ++i) {
      const std::size_t pos = line.find(kSep, search);
      if (pos == std::string_view::npos) {
        well_formed = false;
        break;
      }
      seps[i] = pos;
      search = pos + 1;
    }
    if (!well_formed) {
      continue;
    }
    commits.push_back(GitCommitEntry{
        .hash = std::string(line.substr(0, seps[0])),
        .short_hash = std::string(line.substr(seps[0] + 1, seps[1] - seps[0] - 1)),
        .subject = std::string(line.substr(seps[3] + 1)),
        .author = std::string(line.substr(seps[1] + 1, seps[2] - seps[1] - 1)),
        .relative_date = std::string(line.substr(seps[2] + 1, seps[3] - seps[2] - 1)),
    });
  }
  return commits;
}

}  // namespace microide::project
