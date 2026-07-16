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
  // Do NOT trim leading spaces here: porcelain v2 fields are single-space delimited,
  // so `offset` already points at the first character of the path. Skipping spaces
  // would corrupt a tracked file whose name legitimately begins with a space (e.g.
  // " leading.cpp" would be reported as "leading.cpp").
  return body.substr(offset);
}

bool ParseAheadBehind(std::string_view token, int* ahead, int* behind) {
  if (ahead == nullptr || behind == nullptr || token.size() < 4 || token[0] != '+' ||
      token.find('-') == std::string_view::npos) {
    return false;
  }
  const std::size_t minus = token.find('-');
  // Real porcelain-v2 emits `+<ahead> -<behind>` with a single space field
  // separator (e.g. "+2 -0"), so splitting on the first '-' leaves that
  // separator space trailing on the ahead field. Trim surrounding ASCII
  // whitespace (view-based, no allocation) so the full-consume check below
  // still accepts genuine git output while rejecting garbage like "+12x-3".
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
  // Reject empty fields outright: from_chars on an empty range reports
  // invalid_argument, but an explicit guard keeps the intent obvious.
  if (ahead_text.empty() || behind_text.empty()) {
    return false;
  }
  int parsed_ahead = 0;
  int parsed_behind = 0;
  const auto ahead_result = std::from_chars(ahead_text.data(), ahead_text.data() + ahead_text.size(),
                                            parsed_ahead);
  const auto behind_result =
      std::from_chars(behind_text.data(), behind_text.data() + behind_text.size(), parsed_behind);
  // Require both fields to parse cleanly AND to consume every byte, so a
  // trailing non-numeric suffix ("+12x-3", "+12-3x") is rejected instead of
  // silently yielding a truncated count.
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
                         const GitRepositoryEntry& entry) {
  const GitFileStatus status = entry.conflicted ? GitFileStatus::Conflicted : entry.status;
  GitPorcelainParser::RecordGitStatus(statuses, entry.path.relative_path, status);
  if (entry.old_path.has_value()) {
    // The rename/copy source no longer exists at its old path in the working tree,
    // so badge it Deleted rather than inheriting the destination's status (which
    // painted the now-gone source as Modified/Added).
    GitPorcelainParser::RecordGitStatus(statuses, entry.old_path->relative_path,
                                        GitFileStatus::Deleted);
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
      .worktree_dirty = !conflicted && xy.size() >= 2 && xy[1] != ' ' && xy[1] != '?' &&
                        xy[1] != '!' && xy[1] != '.',
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

  // Bound BOTH the retained entries AND the pre-cap token materialization: a v2 rename
  // entry consumes 2 records (the origin path is the following NUL field), so split at
  // most 2x the entry cap (+ slack). Splitting the whole output first paid an O(record
  // count) allocation for hostile millions-of-tiny-records output. (TD-2026-07-16-30.)
  const std::vector<std::string_view> records =
      util::SplitNulDelimited(output, kMaxGitStatusEntries * 2 + 2);
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

    // A well-formed changed/untracked record is `<kind><space><XY>...`, so it is
    // at least 2 bytes. A shorter record (e.g. a single byte left by the 128 MiB
    // capture truncation on a repo with millions of entries) would make substr(2)
    // throw std::out_of_range, which propagates off the background worker thread
    // and aborts the process. Guard it like the v1 parser's `< 4` check.
    if (record.size() < 2) {
      continue;
    }
    const char kind = record[0];
    const std::string_view body = record.substr(2);
    // Only the changed-entry kinds (1/2/u) carry a leading XY status field. For
    // '?'/'!' (untracked/ignored) records `body` is the whole path — which may
    // legally begin with a space — and the literal "??"/"!!" is used below, so the
    // XY extraction (and its empty-guard) must not run for them or a leading-space
    // untracked file would be silently dropped.
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
                                          std::filesystem::path(path), std::nullopt,
                                          xy.find('U') != std::string_view::npos));
        break;
      }
      case '2': {
        const std::string_view path = PathAfterLeadingTokens(body, 8);
        if (path.empty()) {
          break;
        }
        // In `--porcelain=v2 -z` a rename/copy record's origPath is always the
        // immediately following NUL-delimited field. SplitNulDelimited already
        // consumed that NUL, so `record` never contains one and the origPath is
        // simply records[index + 1]; consume it unconditionally (bounds
        // permitting), exactly as the v1 status parser and the diff parser do.
        // The previous first-byte heuristic gate silently dropped the source path
        // for any file whose name began with '#', '1', '2', 'u', '?' or '!'
        // (e.g. "1-notes.md", "2023-log.txt", "#readme"), losing its tree badge
        // and leaving the origPath record to be misparsed as a bogus entry.
        std::optional<std::filesystem::path> old_path;
        if (index + 1 < records.size()) {
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
