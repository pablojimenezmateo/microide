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

// Read the first line of a tiny git metadata file, refusing non-regular nodes
// (FIFO/device/socket) before opening. Opening/reading a FIFO named `.git`,
// `commondir`, or `HEAD` could block the change-sampling thread indefinitely
// (TD-2026-07-17A-113). status() stats without opening, so a special file is
// rejected before any potentially-blocking stream read.
std::optional<std::string> ReadFirstLineOfRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::status(path, error))) {
    return std::nullopt;
  }
  std::ifstream stream(path);
  if (!stream) {
    return std::nullopt;
  }
  std::string line;
  std::getline(stream, line);
  return line;
}

// A symbolic HEAD ref must be a relative name under the common gitdir (e.g.
// `refs/heads/main`). Reject absolute paths, root names, and empty/`.`/`..`
// components so `common_dir / ref` cannot escape the git directory
// (TD-2026-07-17A-110). A dangling `ref: /tmp/x` would otherwise ignore common_dir
// entirely on POSIX.
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

  const std::optional<std::string> line = ReadFirstLineOfRegularFile(git_marker);
  if (!line.has_value()) {
    return std::nullopt;
  }
  const std::string trimmed = util::TrimAsciiWhitespace(*line);
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

// For a linked worktree, branch refs live in the COMMON git directory, named by
// `<gitdir>/commondir`. Absent that file (an ordinary checkout), the gitdir IS the
// common dir. Returns the resolved common directory. (TD-2026-07-16-63.)
std::filesystem::path ResolveCommonDir(const std::filesystem::path& git_dir) {
  const std::optional<std::string> line = ReadFirstLineOfRegularFile(git_dir / "commondir");
  if (!line.has_value()) {
    return git_dir;
  }
  const std::string trimmed = util::TrimAsciiWhitespace(*line);
  if (trimmed.empty()) {
    return git_dir;
  }
  std::filesystem::path resolved(trimmed);
  if (resolved.is_relative()) {
    resolved = git_dir / resolved;
  }
  return resolved.lexically_normal();
}

// If HEAD is symbolic (`ref: refs/heads/<branch>`), return the ref path relative to the
// common gitdir (e.g. `refs/heads/main`). Returns nullopt for a detached HEAD (raw oid),
// where the HEAD file tick itself already tracks movement.
std::optional<std::string> ReadSymbolicHeadRef(const std::filesystem::path& head_path) {
  const std::optional<std::string> line = ReadFirstLineOfRegularFile(head_path);
  if (!line.has_value()) {
    return std::nullopt;
  }
  const std::string trimmed = util::TrimAsciiWhitespace(*line);
  constexpr std::string_view kPrefix = "ref:";
  if (std::string_view(trimmed).substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;  // detached HEAD (raw object id)
  }
  std::string ref = util::TrimAsciiWhitespace(std::string_view(trimmed).substr(kPrefix.size()));
  // Refuse anything that is not a safe relative ref name so `common_dir / ref`
  // cannot escape the git directory (absolute/rooted refs, `..` segments).
  if (!IsSafeRelativeRefName(ref)) {
    return std::nullopt;
  }
  return ref;
}

}  // namespace

std::optional<std::string> ReadHeadBranchName(const std::filesystem::path& project_root) {
  const std::optional<std::filesystem::path> git_dir = ResolveGitDir(project_root);
  if (!git_dir.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string> ref = ReadSymbolicHeadRef(*git_dir / "HEAD");
  if (!ref.has_value()) {
    return std::nullopt;  // detached HEAD — no branch name to show
  }
  std::string name = std::filesystem::path(*ref).filename().string();
  if (name.empty()) {
    return std::nullopt;
  }
  return name;
}

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
  // HEAD movement is any of: the HEAD file text/tick (branch switch, detached move),
  // the resolved branch ref advancing (ordinary same-branch commit), or packed-refs
  // changing (packed branch refs). (TD-2026-07-16-63.)
  if (current->head != baseline_->head || current->branch_ref != baseline_->branch_ref ||
      current->packed_refs != baseline_->packed_refs) {
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

  // Resolve the branch ref HEAD points at so an ordinary same-branch commit (which
  // leaves `.git/HEAD` text unchanged but advances `refs/heads/<branch>`) is detected.
  // The ref lives under the COMMON gitdir for linked worktrees, so resolve `commondir`.
  const std::filesystem::path common_dir = ResolveCommonDir(git_dir);
  if (const std::optional<std::string> ref = ReadSymbolicHeadRef(git_dir / "HEAD");
      ref.has_value()) {
    if (const auto ref_tick = FileModificationTick(common_dir / *ref); ref_tick.has_value()) {
      tick.branch_ref = *ref_tick;
    }
  }
  // packed-refs fallback: a branch ref stored packed (no loose file) still bumps this.
  if (const auto packed_tick = FileModificationTick(common_dir / "packed-refs");
      packed_tick.has_value()) {
    tick.packed_refs = *packed_tick;
  }
  return tick;
}

}  // namespace microide::project
