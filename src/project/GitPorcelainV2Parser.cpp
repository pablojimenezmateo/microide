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
  return body.substr(offset);
}

std::string_view TokenAt(std::string_view body, std::size_t index) {
  std::size_t offset = 0;
  for (std::size_t i = 0; i < index; ++i) {
    offset = body.find(' ', offset);
    if (offset == std::string_view::npos) {
      return {};
    }
    ++offset;
  }
  if (offset >= body.size()) {
    return {};
  }
  const std::size_t end = body.find(' ', offset);
  return body.substr(offset, end == std::string_view::npos ? std::string_view::npos : end - offset);
}

bool SubmoduleField(std::string_view body, std::size_t sub_token_index) {
  const std::string_view sub = TokenAt(body, sub_token_index);
  return !sub.empty() && sub.front() == 'S';
}

bool ParseAheadBehind(std::string_view token, int* ahead, int* behind) {
  if (ahead == nullptr || behind == nullptr || token.size() < 4 || token[0] != '+' ||
      token.find('-') == std::string_view::npos) {
    return false;
  }
  const std::size_t minus = token.find('-');
  const auto trim_ascii_ws = [](std::string_view text) {
    const auto is_ws = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!text.empty() && is_ws(text.front())) {
      text.remove_prefix(1);
    }
    while (!text.empty() && is_ws(text.back())) {
      text.remove_suffix(1);
    }
    return text;
  };
  const std::string_view ahead_text = trim_ascii_ws(token.substr(1, minus - 1));
  const std::string_view behind_text = trim_ascii_ws(token.substr(minus + 1));
  if (ahead_text.empty() || behind_text.empty()) {
    return false;
  }
  int parsed_ahead = 0;
  int parsed_behind = 0;
  const auto ahead_result = std::from_chars(ahead_text.data(), ahead_text.data() + ahead_text.size(),
                                            parsed_ahead);
  const auto behind_result =
      std::from_chars(behind_text.data(), behind_text.data() + behind_text.size(), parsed_behind);
  if (ahead_result.ec != std::errc{} || behind_result.ec != std::errc{} ||
      ahead_result.ptr != ahead_text.data() + ahead_text.size() ||
      behind_result.ptr != behind_text.data() + behind_text.size()) {
    return false;
  }
  *ahead = parsed_ahead;
  *behind = parsed_behind;
  return true;
}

void RecordTreeGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                         const GitRepositoryEntry& entry,
                         std::string& scratch) {
  const GitFileStatus status = entry.conflicted ? GitFileStatus::Conflicted : entry.status;
  GitPorcelainParser::RecordNormalizedGitStatus(statuses, GenericPathView(entry.path), status,
                                               scratch);
  if (entry.old_path.has_value()) {
    GitPorcelainParser::RecordNormalizedGitStatus(statuses, GenericPathView(*entry.old_path),
                                                  GitFileStatus::Deleted, scratch);
  }
}

GitRepositoryEntry MakeEntry(GitRepositoryEntryKind kind,
                             std::string_view xy,
                             std::string path,
                             std::optional<std::string> old_path,
                             bool conflicted,
                             bool submodule = false) {
  GitRepositoryEntry entry{
      .kind = kind,
      .status = StatusFromPorcelainV2XY(xy, conflicted),
      .conflict_kind = conflicted ? ConflictKindFromUnmergedCodes(xy) : GitConflictKind::None,
      .path = MakeGitRepositoryPathIdentity(std::move(path)),
      .old_path = std::nullopt,
      .staged = !conflicted && xy.size() >= 1 && xy[0] != ' ' && xy[0] != '?' &&
                 xy[0] != '!' && xy[0] != '.',
      .worktree_dirty = !conflicted && xy.size() >= 2 && xy[1] != ' ' && xy[1] != '?' &&
                        xy[1] != '!' && xy[1] != '.',
      .conflicted = conflicted,
      .submodule = submodule,
  };
  if (old_path.has_value()) {
    entry.old_path = MakeGitRepositoryPathIdentity(std::move(*old_path));
  }
  return entry;
}

}

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

  const std::vector<std::string_view> records =
      util::SplitNulDelimited(output, kMaxGitStatusEntries * 2 + 2);
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

    if (record.size() < 2) {
      continue;
    }
    const char kind = record[0];
    const std::string_view body = record.substr(2);
    std::string_view xy;
    if (kind == '1' || kind == '2' || kind == 'u') {
      const std::size_t xy_end = body.find(' ');
      xy = body.substr(0, xy_end == std::string_view::npos ? body.size() : xy_end);
      if (xy.empty()) {
        continue;
      }
    }

    switch (kind) {
      case '1': {
        const std::string_view path = PathAfterLeadingTokens(body, 7);
        if (path.empty()) {
          break;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Ordinary, xy,
                                          std::string(path), std::nullopt,
                                          xy.find('U') != std::string_view::npos,
                                          SubmoduleField(body, 1)));
        break;
      }
      case '2': {
        const std::string_view path = PathAfterLeadingTokens(body, 8);
        if (path.empty()) {
          break;
        }
        std::optional<std::string> old_path;
        if (index + 1 < records.size()) {
          old_path = std::string(records[index + 1]);
          ++index;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Renamed, xy,
                                          std::string(path), std::move(old_path),
                                          false, SubmoduleField(body, 1)));
        break;
      }
      case 'u': {
        const std::string_view path = PathAfterLeadingTokens(body, 9);
        if (path.empty()) {
          break;
        }
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Unmerged, xy,
                                          std::string(path), std::nullopt, true,
                                          SubmoduleField(body, 1)));
        break;
      }
      case '?': {
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Untracked, "??",
                                          std::string(body), std::nullopt, false));
        break;
      }
      case '!': {
        state.entries.push_back(MakeEntry(GitRepositoryEntryKind::Ignored, "!!",
                                          std::string(body), std::nullopt, false));
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

  auto tree_statuses = std::make_shared<GitTreeStatusMap>();
  std::string scratch;
  for (const GitRepositoryEntry& entry : state.entries) {
    if (entry.kind == GitRepositoryEntryKind::Ignored) {
      continue;
    }
    RecordTreeGitStatus(*tree_statuses, entry, scratch);
  }
  state.tree_git_statuses = std::move(tree_statuses);

  return state;
}

}
