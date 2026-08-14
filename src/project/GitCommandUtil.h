#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::project::internal {

struct CommandResult {
  int exit_code = -1;
  std::string output;
  // The command exceeded its wall-clock timeout and was killed. Distinguishes a
  // spurious "failure" (a slow pre-commit hook, a stuck network/credential
  // prompt) from a real non-zero exit so callers can report it accurately.
  bool timed_out = false;
  // Captured output hit the per-stream ceiling and the command was killed; the
  // output is truncated. Callers that display command output (blob/diff reads)
  // should surface this rather than treating it as an outright failure.
  bool truncated = false;

  bool success() const { return exit_code == 0; }
};

// Wall-clock caps for git invocations. Local read commands (status, diff, blame,
// log, show) finish in well under a second, so a short cap catches a genuinely
// stuck git (credential/network stall, filesystem hang) without holding the shell
// hostage. Write/long ops (commit, apply) may legitimately run a slow pre-commit
// hook, so they get a far more generous cap rather than being killed at 60s.
inline constexpr int kGitReadTimeoutMs = 60'000;
inline constexpr int kGitWriteTimeoutMs = 300'000;

bool HasGitMarker(const std::filesystem::path& root);
std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
// The memo entry itself, which is what the form above copies out of. The copy is
// a `std::filesystem::path`, so it is an allocation (or two) per call on a path
// that runs three times per painted frame — and most callers only ask
// `has_value()` or read the result and drop it (TD-2026-08-14-223).
//
// The reference is valid until the next call to EITHER form on this thread: the
// memo is a four-entry LRU and a miss rotates its entries.
const std::optional<std::filesystem::path>& AbsoluteToRelativePathRef(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
std::optional<std::string> ResolveHeadId(const std::filesystem::path& root);

// The repository's git directory: `<root>/.git` when it is a real directory, or
// the path a `.git` *file* points at (`gitdir: …`, the linked-worktree and
// submodule layout). Nullopt when `root` has no usable marker.
std::optional<std::filesystem::path> ResolveGitDirectory(const std::filesystem::path& root);

// Object id of the in-progress merge's incoming side, read straight out of
// `<gitdir>/MERGE_HEAD` — no `git` subprocess. An octopus merge lists one id per
// line; the first is returned, matching `git rev-parse MERGE_HEAD`. Nullopt when
// no merge is in progress or the file is unreadable / not a plain object id.
//
// This exists so a shell-thread caller can label the incoming pane without
// forking git: the subprocess it replaced was bounded only by kGitReadTimeoutMs
// (60 s), so a stuck git stalled the UI for a cosmetic string.
std::optional<std::string> ReadPendingMergeHeadId(const std::filesystem::path& root);
CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr = true,
                                   int timeout_ms = kGitReadTimeoutMs);
CommandResult ReadGitCommandOutputWithStdin(const std::filesystem::path& root,
                                            std::vector<std::string> arguments,
                                            std::string stdin_text,
                                            bool silence_stderr = true,
                                            int timeout_ms = kGitReadTimeoutMs);
bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr = true);

}  // namespace microide::project::internal
