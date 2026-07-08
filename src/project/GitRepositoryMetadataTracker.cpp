#include "project/GitRepositoryMetadataTracker.h"

#include <fstream>
#include <string>
#include <system_error>

#include "util/StringUtil.h"

namespace microide::project {
namespace {

std::optional<std::uint64_t> FileModificationTick(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return std::nullopt;
  }
  const auto tick = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(tick.time_since_epoch().count());
}

// Resolve the real git directory for a project root. For an ordinary checkout
// `<root>/.git` is a directory. For a linked worktree or a submodule it is a
// regular *file* containing `gitdir: <path>` (that gitdir holds this
// worktree's own HEAD and index). Statting `<root>/.git/HEAD` then silently
// fails and change detection dies. Follow the pointer so commit/stage still
// triggers an auto-refresh.
std::optional<std::filesystem::path> ResolveGitDir(const std::filesystem::path& project_root) {
  const std::filesystem::path git_marker = project_root / ".git";
  std::error_code error;
  if (!std::filesystem::exists(git_marker, error)) {
    return std::nullopt;
  }
  if (std::filesystem::is_directory(git_marker, error)) {
    return git_marker;
  }

  std::ifstream stream(git_marker);
  if (!stream) {
    return std::nullopt;
  }
  std::string line;
  std::getline(stream, line);
  const std::string trimmed = util::TrimAsciiWhitespace(line);
  constexpr std::string_view kPrefix = "gitdir:";
  if (std::string_view(trimmed).substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  const std::string target =
      util::TrimAsciiWhitespace(std::string_view(trimmed).substr(kPrefix.size()));
  if (target.empty()) {
    return std::nullopt;
  }
  std::filesystem::path resolved(target);
  if (resolved.is_relative()) {
    resolved = project_root / resolved;
  }
  return resolved.lexically_normal();
}

}  // namespace

void GitRepositoryMetadataTracker::Reset() {
  project_root_.clear();
  baseline_.reset();
}

void GitRepositoryMetadataTracker::SetProjectRoot(const std::filesystem::path& project_root) {
  project_root_ = project_root.lexically_normal();
  baseline_ = ReadCurrentTicks();
}

std::vector<RepositoryChange> GitRepositoryMetadataTracker::SampleChanges() {
  const std::optional<MetadataTick> current = ReadCurrentTicks();
  if (!current.has_value() || !baseline_.has_value()) {
    baseline_ = current;
    return {};
  }

  std::vector<RepositoryChange> changes;
  if (current->head != baseline_->head) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::HeadChanged});
  }
  if (current->index != baseline_->index) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::IndexChanged});
  }
  baseline_ = current;
  return changes;
}

std::optional<GitRepositoryMetadataTracker::MetadataTick>
GitRepositoryMetadataTracker::ReadCurrentTicks() const {
  if (project_root_.empty()) {
    return std::nullopt;
  }

  const std::optional<std::filesystem::path> git_dir_opt = ResolveGitDir(project_root_);
  if (!git_dir_opt.has_value()) {
    return std::nullopt;
  }
  const std::filesystem::path& git_dir = *git_dir_opt;

  MetadataTick tick;
  if (const auto head_tick = FileModificationTick(git_dir / "HEAD"); head_tick.has_value()) {
    tick.head = *head_tick;
  }
  if (const auto index_tick = FileModificationTick(git_dir / "index"); index_tick.has_value()) {
    tick.index = *index_tick;
  }
  return tick;
}

}  // namespace microide::project
