#pragma once

#include <filesystem>
#include <optional>

#include "project/GitCommandUtil.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::project {

// What the editor knows about a repository's `.git` WITHOUT running git: whether a
// tree is a repository at all, and where its git directory is so HEAD, MERGE_HEAD
// and the rebase markers can be read as files.
//
// Locally that is a stat under the project root. For a project whose files are a
// mirror of another machine's (dev-docs/design/remote-projects.md § 6.2) it is not:
// the mirror has no `.git`, so every probe that stats one answers "not a
// repository" and the git call it guards never reaches the host at all
// (TD-2026-10-06-319). The answer has to come from where git runs.
//
// So the source belongs to the same thing the launcher does -- the host -- and a
// launcher that runs processes on a machine whose `.git` the editor cannot stat
// ALSO implements this interface. GitMetadataFor() asks the launcher every git
// function already receives, which is why no git signature had to grow a third
// parameter. The local launcher does not implement it; local stat is the answer.
class GitMetadataSource {
 public:
  virtual ~GitMetadataSource() = default;

  // Three answers: a source that has not heard yet (a remote one before its first
  // `git/metadata`) says Unknown rather than "not a repository".
  virtual GitAvailability Availability(const std::filesystem::path& root) const = 0;

  // The git directory for the working tree at `root`, as a path THIS process can
  // read with plain file I/O; nullopt when there is none, or when the source
  // carries metadata some other way.
  virtual std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const = 0;

  bool IsRepository(const std::filesystem::path& root) const {
    return Availability(root) == GitAvailability::Repository;
  }
};

// Stat under the root, on this machine. Never Unknown.
const GitMetadataSource& LocalGitMetadataSource();

// The metadata source for trees whose processes `launcher` runs.
const GitMetadataSource& GitMetadataFor(const platform::ProcessLauncher& launcher);

}  // namespace microide::project
