#include "project/GitRepositoryMetadataTracker.h"

#include "project/GitMetadataSource.h"

#include <algorithm>
#include <string>
#include <system_error>

#include "project/GitCommandUtil.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project {
namespace {

bool IsSafeRelativeRefName(const std::string& ref) {
  if (ref.empty() || ref.find('\\') != std::string::npos) {
    return false;
  }
  const std::filesystem::path ref_path(ref);
  if (ref_path.is_absolute() || ref_path.has_root_name() || ref_path.has_root_directory()) {
    return false;
  }
  for (const std::filesystem::path& part : ref_path) {
    const std::string component = part.string();
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

std::optional<std::string> SymbolicRefFromHeadLine(const std::optional<std::string>& line) {
  if (!line.has_value()) {
    return std::nullopt;
  }
  const std::string trimmed = util::TrimAsciiWhitespace(*line);
  constexpr std::string_view kPrefix = "ref:";
  if (std::string_view(trimmed).substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  std::string ref = util::TrimAsciiWhitespace(std::string_view(trimmed).substr(kPrefix.size()));
  if (!IsSafeRelativeRefName(ref)) {
    return std::nullopt;
  }
  return ref;
}

}

std::optional<std::string> ReadHeadBranchName(const std::filesystem::path& project_root,
                                              const GitMetadataSource& metadata) {
  const std::optional<std::filesystem::path> git_dir = metadata.ReadableGitDirectory(project_root);
  if (!git_dir.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string> ref =
      SymbolicRefFromHeadLine(internal::ReadFirstLineOfGitFile(*git_dir / "HEAD"));
  if (!ref.has_value()) {
    return std::nullopt;
  }
  const std::string_view name = internal::StripBranchRefPrefix(*ref);
  if (name.empty()) {
    return std::nullopt;
  }
  return std::string(name);
}

void GitRepositoryMetadataTracker::Reset() {
  project_root_.clear();
  baseline_.reset();
}

void GitRepositoryMetadataTracker::SetProjectRoot(const std::filesystem::path& project_root) {
  project_root_ = project_root.lexically_normal();
  baseline_ = ReadCurrentFingerprint();
}

std::vector<RepositoryChange> GitRepositoryMetadataTracker::SampleChanges() {
  const std::optional<MetadataFingerprint> current = ReadCurrentFingerprint();
  if (current.has_value() != baseline_.has_value()) {
    baseline_ = current;
    return {RepositoryChange{.kind = RepositoryChangeKind::HeadChanged}};
  }
  if (!current.has_value() || !baseline_.has_value()) {
    baseline_ = current;
    return {};
  }

  std::vector<RepositoryChange> changes;
  if (current->head_text != baseline_->head_text ||
      current->branch_ref_text != baseline_->branch_ref_text ||
      current->packed_refs != baseline_->packed_refs || current->worktrees != baseline_->worktrees) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::HeadChanged});
  }
  if (current->index != baseline_->index) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::IndexChanged});
  }
  baseline_ = current;
  return changes;
}

std::vector<std::filesystem::path> GitRepositoryMetadataTracker::WatchedDirectories() const {
  std::vector<std::filesystem::path> directories;
  if (project_root_.empty()) {
    return directories;
  }
  const auto add = [&directories](std::filesystem::path directory) {
    if (std::find(directories.begin(), directories.end(), directory) == directories.end()) {
      directories.push_back(std::move(directory));
    }
  };
  add(project_root_);
  const std::optional<std::filesystem::path> git_dir = internal::ResolveGitDirectory(project_root_);
  if (!git_dir.has_value()) {
    return directories;
  }
  add(git_dir->lexically_normal());
  const std::filesystem::path common_dir = internal::ResolveCommonDir(*git_dir);
  add(common_dir);
  if (std::error_code error; std::filesystem::is_directory(common_dir / "worktrees", error) && !error) {
    add(common_dir / "worktrees");
  }
  if (const std::optional<std::string> ref =
          SymbolicRefFromHeadLine(internal::ReadFirstLineOfGitFile(*git_dir / "HEAD"));
      ref.has_value()) {
    add((common_dir / *ref).parent_path().lexically_normal());
  }
  return directories;
}

std::optional<GitRepositoryMetadataTracker::MetadataFingerprint>
GitRepositoryMetadataTracker::ReadCurrentFingerprint() const {
  if (project_root_.empty()) {
    return std::nullopt;
  }

  const std::optional<std::filesystem::path> git_dir_opt = internal::ResolveGitDirectory(project_root_);
  if (!git_dir_opt.has_value()) {
    return std::nullopt;
  }
  const std::filesystem::path& git_dir = *git_dir_opt;

  MetadataFingerprint fingerprint;
  const std::optional<std::string> head_line = internal::ReadFirstLineOfGitFile(git_dir / "HEAD");
  if (head_line.has_value()) {
    fingerprint.head_text = util::TrimAsciiWhitespace(*head_line);
  }
  if (const auto index_tick = util::FileModificationTick(git_dir / "index"); index_tick.has_value()) {
    fingerprint.index = *index_tick;
  }

  const std::filesystem::path common_dir = internal::ResolveCommonDir(git_dir);
  if (const std::optional<std::string> ref = SymbolicRefFromHeadLine(head_line);
      ref.has_value()) {
    if (const std::optional<std::string> ref_line =
            internal::ReadFirstLineOfGitFile(common_dir / *ref);
        ref_line.has_value()) {
      fingerprint.branch_ref_text = util::TrimAsciiWhitespace(*ref_line);
    }
  }
  if (const auto packed_tick = util::FileModificationTick(common_dir / "packed-refs");
      packed_tick.has_value()) {
    fingerprint.packed_refs = *packed_tick;
  }
  if (const auto worktrees_tick = util::FileModificationTick(common_dir / "worktrees"); worktrees_tick.has_value()) {
    fingerprint.worktrees = *worktrees_tick;
  }
  return fingerprint;
}

}
