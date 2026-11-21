#include "project/GitWorkingTreeSnapshot.h"

#include <string_view>
#include <utility>
#include <vector>

#include "platform/Subprocess.h"
#include "project/GitCommandUtil.h"
#include "project/GitRepository.h"
#include "util/DurableFile.h"
#include "util/StringUtil.h"

namespace microide::project {
namespace {

std::string FirstLine(const std::string& output) {
  return std::string(util::TrimAsciiWhitespace(std::string_view(output).substr(0, output.find('\n'))));
}

GitSnapshotResult Failed(std::string why) {
  return GitSnapshotResult{.commit = {}, .error = std::move(why)};
}

// A save writes its bytes to a temporary beside the file and renames it into
// place (util/DurableFile), and a crashed save leaves its temporary behind until a
// scan sweeps it. Neither is the user's file: `add --all` in that window takes it
// in, and a review against the snapshot then names a file the app made and
// removed (soak 5f05c7931622). Every git call runs with --literal-pathspecs, so
// the temporaries cannot be excluded on the `add`; they are taken back out of the
// snapshot's index here. Only files added since HEAD can be one -- the diff of the
// index against HEAD reads no working tree -- and with no HEAD yet, the index.
[[nodiscard]] bool DropSaveTemporaries(const GitRepository& repo,
                                       const std::vector<platform::SubprocessEnvironmentOverride>& snapshot_index,
                                       bool has_head) {
  const auto added =
      has_head ? repo.ExecuteWithEnvironment({"diff", "--cached", "--name-only", "-z", "--no-renames",
                                              "--diff-filter=A", "HEAD", "--"},
                                             snapshot_index, internal::kGitWriteTimeoutMs)
               : repo.ExecuteWithEnvironment({"ls-files", "-z", "--cached"}, snapshot_index,
                                             internal::kGitWriteTimeoutMs);
  if (!added.success()) {
    return false;
  }
  std::vector<std::string> remove = {"rm", "--cached", "--quiet", "--ignore-unmatch", "--"};
  const std::size_t first_path = remove.size();
  std::string_view rest(added.output);
  while (!rest.empty()) {
    const std::size_t end = rest.find('\0');
    const std::string_view path = rest.substr(0, end);
    const std::size_t slash = path.rfind('/');
    if (util::IsTemporaryStagingFilename(slash == std::string_view::npos ? path : path.substr(slash + 1))) {
      remove.emplace_back(path);
    }
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
  }
  return remove.size() == first_path ||
         repo.ExecuteWithEnvironment(remove, snapshot_index, internal::kGitWriteTimeoutMs).success();
}

}  // namespace

GitSnapshotResult SnapshotGitWorkingTree(const std::filesystem::path& repository_root,
                                         const platform::ProcessLauncher& launcher) {
  const GitRepository repo(repository_root, launcher);
  if (!repo.IsValid()) {
    return Failed("not a git repository");
  }
  // The git directory as the repository's machine names it: a linked working
  // tree's own (`.git/worktrees/<name>`), where its index is.
  const auto git_dir = repo.Execute({"rev-parse", "--absolute-git-dir"});
  const std::string git_dir_path = git_dir.success() ? FirstLine(git_dir.output) : std::string();
  if (git_dir_path.empty()) {
    return Failed("git could not name the repository's git directory");
  }
  const std::string index = git_dir_path + "/microide-snapshot-index";
  // Seed the snapshot's index from the real one, once: it brings every tracked
  // file's stat data, so `add` reads only what changed instead of the whole tree.
  // `-n` leaves an index that already exists alone; no real index yet (an unborn
  // repository) fails here and `add` starts from nothing.
  (void)launcher.Run({"cp", "-n", "--", git_dir_path + "/index", index},
                     platform::SubprocessOptions{.cwd = repository_root.lexically_normal(),
                                                 .stdin_text = {},
                                                 .environment_overrides = {},
                                                 .capture_stdout = false,
                                                 .capture_stderr = false,
                                                 .silence_stderr = true,
                                                 .timeout_ms = internal::kGitReadTimeoutMs});
  const std::vector<platform::SubprocessEnvironmentOverride> snapshot_index = {{"GIT_INDEX_FILE", index}};
  // Hashing a large tree's changes can take a while: the write cap, not the read one.
  // `--ignore-errors`: a file git cannot read (no permission, gone under the walk)
  // is left out of the snapshot instead of failing it. Such a file cannot be
  // reviewed either way, and one of them anywhere in the tree would otherwise cost
  // the checkpoint of every other file. git then exits 1 with the rest added;
  // anything it could not go on from is another status.
  const auto added = repo.ExecuteWithEnvironment({"add", "--all", "--ignore-errors"}, snapshot_index,
                                                 internal::kGitWriteTimeoutMs);
  if (!added.success() && (added.exit_code != 1 || added.timed_out)) {
    return Failed("git could not read the working tree");
  }
  const bool has_head = repo.ExecuteSucceeds({"rev-parse", "--verify", "--quiet", "HEAD^{commit}"});
  if (!DropSaveTemporaries(repo, snapshot_index, has_head)) {
    return Failed("git could not leave the app's save temporaries out of the snapshot");
  }
  const auto tree = repo.ExecuteWithEnvironment({"write-tree"}, snapshot_index, internal::kGitWriteTimeoutMs);
  const std::string tree_id = tree.success() ? FirstLine(tree.output) : std::string();
  if (tree_id.empty()) {
    return Failed("git could not write the working tree's snapshot");
  }
  // An identity of its own: commit-tree refuses to run for a user who never set
  // one, and the snapshot is nobody's commit. HEAD as parent when there is one.
  std::vector<std::string> commit = {"-c", "user.name=microide", "-c", "user.email=microide@localhost",
                                     "commit-tree", tree_id, "-m", "microide working tree snapshot"};
  if (has_head) {
    commit.insert(commit.end(), {"-p", "HEAD"});
  }
  const auto committed = repo.Execute(commit);
  const std::string commit_id = committed.success() ? FirstLine(committed.output) : std::string();
  if (commit_id.empty()) {
    return Failed("git could not record the working tree's snapshot");
  }
  return GitSnapshotResult{.commit = commit_id, .error = {}};
}

GitSnapshotResult MarkGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher) {
  GitSnapshotResult snapshot = SnapshotGitWorkingTree(repository_root, launcher);
  if (!snapshot.ok()) {
    return snapshot;
  }
  const GitRepository repo(repository_root, launcher);
  if (!repo.ExecuteSucceeds({"update-ref", "-m", "microide: review checkpoint", std::string(kGitReviewCheckpointRef),
                             snapshot.commit})) {
    return Failed("git could not record the checkpoint");
  }
  return snapshot;
}

std::optional<std::string> ResolveGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                                      const platform::ProcessLauncher& launcher) {
  const GitRepository repo(repository_root, launcher);
  if (!repo.IsValid()) {
    return std::nullopt;
  }
  const auto resolved =
      repo.Execute({"rev-parse", "--verify", "--quiet", std::string(kGitReviewCheckpointRef) + "^{commit}"});
  const std::string commit = resolved.success() ? FirstLine(resolved.output) : std::string();
  return commit.empty() ? std::nullopt : std::optional<std::string>(commit);
}

std::vector<GitBranchFileEntry> CollectGitTreeDiffFiles(const std::filesystem::path& repository_root,
                                                        const platform::ProcessLauncher& launcher,
                                                        std::string_view from, std::string_view to) {
  const GitRepository repo(repository_root, launcher);
  if (from.empty() || to.empty() || !repo.IsValid()) {
    return {};
  }
  const auto result = repo.Execute({"diff", "--name-status", "-z", "--find-renames", "--end-of-options",
                                    std::string(from), std::string(to)});
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return ParseGitBranchDiffNameStatusZ(result.output);
}

}  // namespace microide::project
