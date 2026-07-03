#include "project/GitPorcelainV2Parser.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

using std::string_view_literals::operator""sv;

#include "project/GitPorcelainParser.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

std::string_view PathAfterLeadingTokens(std::string_view body, std::size_t token_count) {
  std::size_t offset = 0;
  for (std::size_t i = 0; i < token_count; ++i) {
    if (offset >= body.size()) {
      return {};
    }
    offset = body.find(' ', offset);
    if (offset == std::string_view::npos) {
      return {};
    }
    ++offset;
  }
  while (offset < body.size() && body[offset] == ' ') {
    ++offset;
  }
  return body.substr(offset);
}

bool RecordLooksLikePathContinuation(std::string_view record) {
  return !record.empty() && record[0] != '#' && record[0] != '1' && record[0] != '2' &&
         record[0] != 'u' && record[0] != '?' && record[0] != '!';
}

bool ParseAheadBehind(std::string_view token, int* ahead, int* behind) {
  if (ahead == nullptr || behind == nullptr || token.size() < 4 || token[0] != '+' ||
      token.find('-') == std::string_view::npos) {
    return false;
  }
  const std::size_t minus = token.find('-');
  const std::string_view ahead_text = token.substr(1, minus - 1);
  const std::string_view behind_text = token.substr(minus + 1);
  int parsed_ahead = 0;
  int parsed_behind = 0;
  const auto ahead_result = std::from_chars(ahead_text.data(), ahead_text.data() + ahead_text.size(),
                                            parsed_ahead);
  const auto behind_result =
      std::from_chars(behind_text.data(), behind_text.data() + behind_text.size(), parsed_behind);
  if (ahead_result.ec != std::errc{} || behind_result.ec != std::errc{}) {
    return false;
  }
  *ahead = parsed_ahead;
  *behind = parsed_behind;
  return true;
}

void RecordTreeGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                         const GitRepositoryEntry& entry) {
  const GitFileStatus status = entry.conflicted ? GitFileStatus::Conflicted : entry.status;
  const auto record = [&](const GitRepositoryPathIdentity& identity) {
    GitPorcelainParser::RecordGitStatus(statuses, identity.relative_path, status);
  };
  record(entry.path);
  if (entry.old_path.has_value()) {
    record(*entry.old_path);
  }
}

GitRepositoryEntry MakeEntry(GitRepositoryEntryKind kind,
                             std::string_view xy,
                             std::filesystem::path path,
                             std::optional<std::filesystem::path> old_path,
                             bool conflicted) {
  GitRepositoryEntry entry{
      .kind = kind,
      .status = StatusFromPorcelainV2XY(xy, conflicted),
      .conflict_kind = conflicted ? ConflictKindFromUnmergedCodes(xy) : GitConflictKind::None,
      .path = MakeGitRepositoryPathIdentity(std::move(path)),
      .old_path = std::nullopt,
      .staged = !conflicted && xy.size() >= 1 && xy[0] != ' ' && xy[0] != '?' &&
                 xy[0] != '!' && xy[0] != '.',
      .conflicted = conflicted,
  };
  if (old_path.has_value()) {
    entry.old_path = MakeGitRepositoryPathIdentity(std::move(*old_path));
  }
  return entry;
}

}  // namespace

GitRepositoryState GitPorcelainV2Parser::Parse(std::string_view output,
                                               std::filesystem::path repository_root,
                                               std::uint64_t generation,
                                               std::uint64_t refreshed_at_ms) {
  GitRepositoryState state{
      .repository_root = std::move(repository_root),
      .branch = {},
      .entries = {},
      .tree_git_statuses = {},
      .refresh_error = {},
      .generation = generation,
      .refreshed_at_ms = refreshed_at_ms,
  };
  state.repo_available = true;

  const std::vector<std::string_view> records = util::SplitNulDelimited(output);
  // Cap entries (and the reserve) against a hostile repo's millions of records;
  // see kMaxGitStatusEntries. Extra records past the cap are dropped.
  state.entries.reserve(std::min(records.size(), kMaxGitStatusEntries));
  for (std::size_t index = 0;
       index < records.size() && state.entries.size() < kMaxGitStatusEntries; ++index) {
    const std::string_view record = records[index];
    if (record.empty()) {
      continue;
    }
    if (record.starts_with("# branch.")) {
      const std::size_t key_end = record.find(' ', "# branch."sv.size());
      if (key_end == std::string_view::npos) {
        continue;
      }
      const std::string_view key = record.substr(0, key_end);
      const std::string_view value = record.substr(key_end + 1);
      if (key == "# branch.oid") {
        state.branch.head_oid.assign(value);
      } else if (key == "# branch.head") {
        if (value == "(detached)") {
          state.branch.head_kind = GitHeadKind::Detached;
        } else if (value == "(unborn)") {
          state.branch.head_kind = GitHeadKind::Unborn;
        } else {
          state.branch.head_kind = GitHeadKind::Normal;
          state.branch.branch_name.assign(value);
        }
      } else if (key == "# branch.upstream") {
        state.branch.upstream.assign(value);
      } else if (key == "# branch.ab") {
        ParseAheadBehind(value, &state.branch.ahead, &state.branch.behind);
      }
      continue;
    }

    const char kind = record[0];
    const std::string_view body = record.substr(2);
    // Only the leading XY status field is consumed here; extract it directly
    // instead of splitting the whole body into a throwaway vector per entry.
    const std::size_t xy_end = body.find(' ');
    const std::string_view xy =
        body.substr(0, xy_end == std::string_view::npos ? body.size() : xy_end);
    if (xy.empty()) {
      continue;
    }

    switch (kind) {
      case '1': {
        const std::string_view path = PathAfterLeadingTokens(body, 7);
        if (path.empty()) {
          break;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Ordinary, xy,
                                          std::filesystem::path(path), std::nullopt,
                                          xy.find('U') != std::string_view::npos));
        break;
      }
      case '2': {
        const std::string_view path = PathAfterLeadingTokens(body, 8);
        if (path.empty()) {
          break;
        }
        std::optional<std::filesystem::path> old_path;
        const std::size_t embedded_nul = record.find('\0', 2);
        if (embedded_nul != std::string_view::npos && embedded_nul + 1 < record.size()) {
          old_path = std::filesystem::path(record.substr(embedded_nul + 1));
        } else if (index + 1 < records.size() &&
                   RecordLooksLikePathContinuation(records[index + 1])) {
          old_path = std::filesystem::path(records[index + 1]);
          ++index;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Renamed, xy,
                                          std::filesystem::path(path), std::move(old_path),
                                          false));
        break;
      }
      case 'u': {
        // Porcelain v2 unmerged: XY sub m1 m2 m3 mW h1 h2 h3 <path>
        const std::string_view path = PathAfterLeadingTokens(body, 9);
        if (path.empty()) {
          break;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Unmerged, xy,
                                          std::filesystem::path(path), std::nullopt, true));
        break;
      }
      case '?': {
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Untracked, "??",
                                          std::filesystem::path(body), std::nullopt, false));
        break;
      }
      case '!': {
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Ignored, "!!",
                                          std::filesystem::path(body), std::nullopt, false));
        break;
      }
      default:
        break;
    }
  }

  if (state.branch.head_kind == GitHeadKind::Detached && state.branch.branch_name.empty() &&
      !state.branch.head_oid.empty()) {
    state.branch.branch_name = state.branch.head_oid.substr(0, std::min<std::size_t>(7, state.branch.head_oid.size()));
  }

  for (const GitRepositoryEntry& entry : state.entries) {
    if (entry.kind == GitRepositoryEntryKind::Ignored) {
      continue;
    }
    RecordTreeGitStatus(state.tree_git_statuses, entry);
  }

  return state;
}

}  // namespace microide::project
