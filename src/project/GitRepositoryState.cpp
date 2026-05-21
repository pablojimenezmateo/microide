#include "project/GitRepositoryState.h"

#include <sstream>

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

GitRepositoryPathIdentity MakeGitRepositoryPathIdentity(std::filesystem::path relative_path) {
  relative_path = relative_path.lexically_normal();
  const std::string generic = relative_path.generic_string();
  GitRepositoryPathIdentity identity{
      .relative_path = std::move(relative_path),
      .display_label = generic,
      .path_is_valid_utf8 = IsValidUtf8(generic),
  };
  if (!identity.path_is_valid_utf8) {
    identity.display_label = EscapeNonUtf8PathLabel(generic);
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
  if (xy.find('D') != std::string_view::npos) {
    return GitFileStatus::Deleted;
  }
  if (xy.find('A') != std::string_view::npos || xy.find('C') != std::string_view::npos) {
    return GitFileStatus::Added;
  }
  if (xy.find_first_of("MRT") != std::string_view::npos) {
    return GitFileStatus::Modified;
  }
  return GitFileStatus::Clean;
}

}  // namespace microide::project
