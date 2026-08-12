#include "project/GitRepositoryState.h"

#include <sstream>
#include <system_error>

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"
#include "util/PathMatch.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

std::string EscapeNonUtf8PathLabel(std::string_view bytes) {
  std::ostringstream escaped;
  escaped << "0x";
  escaped.setf(std::ios::hex, std::ios::basefield);
  for (const unsigned char byte : bytes) {
    escaped.width(2);
    escaped.fill('0');
    escaped << static_cast<unsigned>(byte);
  }
  return escaped.str();
}

bool IsValidUtf8(std::string_view text) {
  return util::IsValidUtf8(text);
}

}  // namespace

GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::string relative_path) {
  // git's porcelain output is already relative, '/'-separated and normal, so
  // `lexically_normal()` is a no-op for every real entry — but it costs ~12
  // allocations (a fresh path plus a string per component) and this runs once per
  // changed file on every refresh. Confirm the spelling instead, and pay the path
  // round-trip only for the unusual spellings (TD-2026-08-10-174).
  if (util::PathTextNeedsNormalizing(relative_path)) {
    relative_path = std::filesystem::path(relative_path).lexically_normal().generic_string();
  }
  const bool valid_utf8 = IsValidUtf8(relative_path);
  GitRepositoryPathIdentity identity{
      .relative_path = std::move(relative_path),
      .escaped_label = {},
      .path_is_valid_utf8 = valid_utf8,
  };
  if (!valid_utf8) {
    identity.escaped_label = EscapeNonUtf8PathLabel(identity.relative_path);
  }
  return identity;
}

GitRefreshErrorCategory ClassifyGitRefreshFailure(int exit_code,
                                                  std::string_view stderr_text) {
  if (exit_code == 0) {
    return GitRefreshErrorCategory::None;
  }
  if (stderr_text.find("not a git repository") != std::string_view::npos) {
    return GitRefreshErrorCategory::NotARepo;
  }
  if (stderr_text.find("index.lock") != std::string_view::npos ||
      stderr_text.find("Unable to create") != std::string_view::npos) {
    return GitRefreshErrorCategory::RepoLocked;
  }
  if (stderr_text.find("Authentication failed") != std::string_view::npos ||
      stderr_text.find("Permission denied") != std::string_view::npos) {
    return GitRefreshErrorCategory::AuthFailed;
  }
  if (stderr_text.find("Submodule") != std::string_view::npos) {
    return GitRefreshErrorCategory::SubmoduleError;
  }
  (void)exit_code;
  return GitRefreshErrorCategory::UnknownError;
}

GitConflictKind ConflictKindFromUnmergedCodes(std::string_view xy) {
  if (xy == "DD") {
    return GitConflictKind::BothDeleted;
  }
  if (xy == "AU" || xy == "UA") {
    return xy[0] == 'A' ? GitConflictKind::AddedByUs : GitConflictKind::AddedByThem;
  }
  if (xy == "DU" || xy == "UD") {
    return xy[0] == 'D' ? GitConflictKind::DeletedByUs : GitConflictKind::DeletedByThem;
  }
  if (xy == "AA") {
    return GitConflictKind::BothAdded;
  }
  if (xy.find('U') != std::string_view::npos) {
    return GitConflictKind::BothModified;
  }
  return GitConflictKind::Unknown;
}

GitFileStatus StatusFromPorcelainV2XY(std::string_view xy, bool conflicted) {
  if (conflicted) {
    return GitFileStatus::Conflicted;
  }
  if (xy == "??") {
    return GitFileStatus::Untracked;
  }
  return GitPorcelainParser::StatusFromChangeCodeChars(xy);
}

GitOperationStateKind DetectGitOperationState(const std::filesystem::path& repository_root) {
  const std::optional<std::filesystem::path> git_dir =
      internal::ResolveGitDirectory(repository_root);
  if (!git_dir.has_value()) {
    return GitOperationStateKind::None;
  }
  // Non-throwing probes: the git directory can sit on an unmounted network
  // volume or a directory that lost +x, and a refresh must degrade to "no
  // operation" rather than abort the background worker.
  const auto present = [&git_dir](std::string_view name) {
    std::error_code error;
    return std::filesystem::exists(*git_dir / name, error) && !error;
  };
  // Precedence mirrors git's own wt_status_get_state: a merge is reported by
  // MERGE_HEAD, a rebase by either of its two state directories, and the
  // sequencer operations by their respective HEAD files.
  //
  // All five marker names were verified against real git, and so was the
  // precedence: a rebase leaves `rebase-merge/` + AUTO_MERGE + MERGE_MSG, a
  // revert leaves REVERT_HEAD + AUTO_MERGE + MERGE_MSG, and a cherry-pick leaves
  // CHERRY_PICK_HEAD — none of them leave MERGE_HEAD. So checking MERGE_HEAD
  // first cannot shadow the others, even though the merge-ish AUTO_MERGE and
  // MERGE_MSG files are present in every case. Those two are deliberately NOT
  // probed for exactly that reason.
  if (present("MERGE_HEAD")) {
    return GitOperationStateKind::Merge;
  }
  if (present("rebase-merge") || present("rebase-apply")) {
    return GitOperationStateKind::Rebase;
  }
  if (present("CHERRY_PICK_HEAD")) {
    return GitOperationStateKind::CherryPick;
  }
  if (present("REVERT_HEAD")) {
    return GitOperationStateKind::Revert;
  }
  if (present("BISECT_LOG")) {
    return GitOperationStateKind::Bisect;
  }
  return GitOperationStateKind::None;
}

}  // namespace microide::project
