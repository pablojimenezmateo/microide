#include "project/GitRepositoryMetadataTracker.h"

#include <string>
#include <system_error>

#include "project/GitCommandUtil.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project {
namespace {

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

// For a linked worktree, branch refs live in the COMMON git directory, named by
// `<gitdir>/commondir`. Absent that file (an ordinary checkout), the gitdir IS the
// common dir. Returns the resolved common directory. (TD-2026-07-16-63.)
std::filesystem::path ResolveCommonDir(const std::filesystem::path& git_dir) {
  const std::optional<std::string> line = internal::ReadFirstLineOfGitFile(git_dir / "commondir");
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
// Split from the file read so a caller that already has HEAD's first line does not
// open the file a second time to ask what it points at.
std::optional<std::string> SymbolicRefFromHeadLine(const std::optional<std::string>& line) {
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
  const std::optional<std::filesystem::path> git_dir = internal::ResolveGitDirectory(project_root);
  if (!git_dir.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string> ref =
      SymbolicRefFromHeadLine(internal::ReadFirstLineOfGitFile(*git_dir / "HEAD"));
  if (!ref.has_value()) {
    return std::nullopt;  // detached HEAD — no branch name to show
  }
  // The short name git prints (`git status --porcelain=v2` `# branch.head`,
  // `git branch --show-current`) is the ref minus its `refs/heads/` namespace,
  // and it keeps its slashes: `feature/stable-sort`, not `stable-sort`. Taking
  // the path's leaf here made the status bar flip from one to the other when the
  // first snapshot landed. A HEAD outside `refs/heads/` (a `refs/remotes/...`
  // checkout is not a branch) is reported by its full ref, as git does.
  constexpr std::string_view kHeadsPrefix = "refs/heads/";
  std::string_view name = *ref;
  if (name.starts_with(kHeadsPrefix)) {
    name.remove_prefix(kHeadsPrefix.size());
  }
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
  // The repository appearing or disappearing IS head movement, and the tracker
  // used to swallow it: `ReadCurrentFingerprint` returns nullopt when there is no
  // usable `.git`, and both transitions (an in-session `git init`, an `rm -rf
  // .git`) hit the "no baseline to compare" branch below, which re-baselines and
  // reports nothing. Every consumer of a repository change — the sidebar's
  // staleness mark, the status bar's cached availability probe — therefore kept
  // the pre-init answer until something unrelated forced a refresh.
  if (current.has_value() != baseline_.has_value()) {
    baseline_ = current;
    return {RepositoryChange{.kind = RepositoryChangeKind::HeadChanged}};
  }
  if (!current.has_value() || !baseline_.has_value()) {
    baseline_ = current;
    return {};
  }

  std::vector<RepositoryChange> changes;
  // HEAD movement is any of: the HEAD file text/tick (branch switch, detached move),
  // the resolved branch ref advancing (ordinary same-branch commit), or packed-refs
  // changing (packed branch refs). (TD-2026-07-16-63.)
  if (current->head_text != baseline_->head_text ||
      current->branch_ref_text != baseline_->branch_ref_text ||
      current->packed_refs != baseline_->packed_refs) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::HeadChanged});
  }
  if (current->index != baseline_->index) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::IndexChanged});
  }
  baseline_ = current;
  return changes;
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
  // HEAD by content: one line, read ONCE and used for both the fingerprint and the
  // branch-ref resolution below. It replaces a `last_write_time` stat with a read
  // of a ~30-byte file — not free, but it is what stops a rewrite with identical
  // bytes (`git checkout` of the branch already checked out, a tool that rewrites
  // HEAD) from reporting a change and spawning a `git status` with nothing to find,
  // which costs a process. An earlier version of this comment claimed the read was
  // already happening and therefore free; it was not — it had added a second open
  // of the same file.
  const std::optional<std::string> head_line = internal::ReadFirstLineOfGitFile(git_dir / "HEAD");
  if (head_line.has_value()) {
    fingerprint.head_text = util::TrimAsciiWhitespace(*head_line);
  }
  if (const auto index_tick = util::FileModificationTick(git_dir / "index"); index_tick.has_value()) {
    fingerprint.index = *index_tick;
  }

  // The branch ref HEAD points at, so an ordinary same-branch commit (which leaves
  // `.git/HEAD` text unchanged but advances `refs/heads/<branch>`) is detected. The
  // ref lives under the COMMON gitdir for linked worktrees, so resolve `commondir`.
  // By content too: it is a single object id, and that id is exactly the question.
  const std::filesystem::path common_dir = ResolveCommonDir(git_dir);
  if (const std::optional<std::string> ref = SymbolicRefFromHeadLine(head_line);
      ref.has_value()) {
    if (const std::optional<std::string> ref_line =
            internal::ReadFirstLineOfGitFile(common_dir / *ref);
        ref_line.has_value()) {
      fingerprint.branch_ref_text = util::TrimAsciiWhitespace(*ref_line);
    }
  }
  // packed-refs fallback: a branch ref stored packed (no loose file) still bumps this.
  if (const auto packed_tick = util::FileModificationTick(common_dir / "packed-refs");
      packed_tick.has_value()) {
    fingerprint.packed_refs = *packed_tick;
  }
  return fingerprint;
}

}  // namespace microide::project
