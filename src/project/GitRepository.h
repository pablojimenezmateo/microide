#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCommandUtil.h"
#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

class GitRepository {
 public:
  explicit GitRepository(std::filesystem::path root);

  const std::filesystem::path& root() const { return root_; }
  bool IsValid() const;

  std::optional<std::filesystem::path> ToRelative(const std::filesystem::path& absolute_path) const;
  std::filesystem::path ToAbsolute(const std::filesystem::path& relative_path) const;

  struct CommandResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
    bool truncated = false;
    bool success() const { return exit_code == 0; }
  };

  // `timeout_ms` bounds the invocation (see internal::kGitReadTimeoutMs /
  // kGitWriteTimeoutMs). Read commands use the default; write/long ops (commit,
  // apply) pass the generous write cap so a slow pre-commit hook is not killed.
  CommandResult Execute(std::initializer_list<std::string_view> arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  CommandResult Execute(const std::vector<std::string>& arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  // Runs git with `stdin_text` fed to the child's stdin. Used for commands that
  // take content on stdin (e.g. `commit -F -`), avoiding argv exposure and argv
  // length limits for large payloads.
  CommandResult ExecuteWithStdin(const std::vector<std::string>& arguments,
                                 std::string stdin_text,
                                 bool silence_stderr = true,
                                 int timeout_ms = internal::kGitReadTimeoutMs) const;
  bool ExecuteSucceeds(std::initializer_list<std::string_view> arguments,
                       bool silence_stderr = true) const;
  bool ExecuteSucceeds(const std::vector<std::string>& arguments,
                       bool silence_stderr = true) const;

  std::unordered_map<std::string, GitFileStatus> GetStatuses() const;
  std::vector<GitWorkingTreeEntry> GetWorkingTreeEntries() const;
  GitFileHistoryResult GetFileHistory(const std::filesystem::path& relative_path) const;
  bool FileExistsAtRevision(const std::filesystem::path& relative_path,
                            std::string_view revision = "HEAD") const;

  // A blob read at a revision plus whether it was clipped. `truncated` is true when
  // the blob exceeded the subprocess capture ceiling and git was killed mid-read, so
  // `content` holds only a partial prefix that callers must NOT diff/save as truth.
  struct BlobAtRevision {
    std::string content;
    bool truncated = false;
  };

  // Interprets a `git show <rev>:<path>` result into a blob outcome. A real failure
  // (missing revision, non-zero exit that was NOT a capture-ceiling kill) is nullopt;
  // a success or a capture-ceiling truncation returns the (possibly partial) bytes
  // with `truncated` set accordingly. Pure and static so the truncation contract can
  // be unit-tested without spawning git.
  static std::optional<BlobAtRevision> InterpretBlobResult(const CommandResult& result);

  // The outcome of resolving `<revision>:<relative_path>` to a blob. `exists` is
  // false when git resolved the name to nothing (or to a tree/tag rather than a
  // file); `content`/`truncated` mean what they do on BlobAtRevision.
  struct BlobLookup {
    bool exists = false;
    std::string content;
    bool truncated = false;
  };

  // Existence AND content in ONE git spawn, via `cat-file --batch` with the object
  // name on stdin. The pair this replaces (`cat-file -e` then `show`) cost two
  // process spawns per side of every compare/merge/review tab — ~12 ms of shell
  // thread per file on a warm cache here — and opening a review is N of those in a
  // row, so the stall scaled with the size of the change being reviewed.
  //
  // `--batch` reports a missing name in its own output and still exits 0, so this
  // separates "no such file at that revision" (exists=false) from "git itself
  // failed" (nullopt) more sharply than the exit-code-only probe did: `cat-file -e`
  // failing because git was missing or the repo was broken read as "file absent".
  // Feeding the name on stdin also keeps it out of argv entirely.
  //
  // nullopt when git failed, the reply was unparseable, or the object name cannot
  // be expressed on `--batch`'s newline-delimited stdin (a path containing a
  // newline; `--batch -z` is too new to require).
  std::optional<BlobLookup> LookupBlobAtRevision(const std::filesystem::path& relative_path,
                                                 std::string_view revision = "HEAD") const;

  // Parses one `git cat-file --batch` reply. Pure and static so the framing
  // contract (header, exact byte count, missing marker, truncation) is unit
  // testable without spawning git.
  static std::optional<BlobLookup> InterpretBatchBlobResult(const CommandResult& result);

  // Consumes ONE reply from `output` starting at `offset`, advancing `offset` past
  // it. False when the remaining bytes do not hold a whole reply — the end of the
  // stream, or a capture-ceiling kill that clipped one mid-payload.
  static bool ReadNextBatchBlob(std::string_view output, std::size_t& offset, BlobLookup& out);

  // One `<revision, relative path>` blob request and its answer.
  struct BlobRequest {
    std::string revision;
    std::filesystem::path relative_path;
  };

  // Resolves MANY blobs per git spawn: `cat-file --batch` reads every object name
  // from one stdin stream and answers them in order. This is what makes opening a
  // review cheap — a per-file lookup is one fork+exec each, and a conflict review
  // asks for three stages of every conflicted file.
  //
  // The returned vector is index-aligned with `requests`; an entry is nullopt when
  // that request could not be answered (its name could not be sent, or the stream
  // ended early because git was killed at the capture ceiling). Callers treat a
  // nullopt as "not prefetched" and fall back to the single-blob path, so this stays
  // a pure optimization: no outcome depends on the batch succeeding.
  std::vector<std::optional<BlobLookup>> LookupBlobsAtRevisions(
      const std::vector<BlobRequest>& requests) const;

  std::optional<BlobAtRevision> ReadBlobAtRevision(const std::filesystem::path& relative_path,
                                                   std::string_view revision = "HEAD") const;
  // Content-only convenience wrapper over ReadBlobAtRevision; drops the truncation
  // flag, so callers that must reject partial blobs use ReadBlobAtRevision instead.
  std::optional<std::string> ReadFileAtRevision(const std::filesystem::path& relative_path,
                                                std::string_view revision = "HEAD") const;
  std::optional<std::string> ResolveHeadId() const;
  bool HasHeadCommit() const;
  bool FileIsTracked(const std::filesystem::path& relative_path) const;
  bool FileIsWorkingTreeClean(const std::filesystem::path& relative_path) const;

  bool Stage(const std::filesystem::path& relative_path) const;
  // `may_be_staged_rename` gates the StagedRenameSource probe (a whole-index
  // `git diff --cached --name-status -M -z`). The sidebar passes false for the
  // common non-rename case so a single-file unstage/discard skips that diff; it
  // defaults true so callers without the hint stay correct.
  bool Unstage(const std::filesystem::path& relative_path,
               bool may_be_staged_rename = true) const;
  bool Discard(const std::filesystem::path& relative_path,
               bool may_be_staged_rename = true) const;
  bool StageAll() const;
  // `remove_untracked` gates the `git clean -fd` step that permanently deletes
  // untracked files. The sidebar passes false and trashes untracked files itself
  // (recoverable), mirroring the single-file untracked-discard policy; it defaults
  // true so other callers keep the full-clean behavior.
  bool DiscardAll(bool remove_untracked = true) const;

 private:
  // If `dest_relative` is the destination of a currently-staged rename/copy,
  // returns the source path; otherwise nullopt. Lets Unstage/Discard operate on
  // both sides of a rename instead of orphaning the source's staged deletion.
  std::optional<std::filesystem::path> StagedRenameSource(
      const std::filesystem::path& dest_relative) const;

  std::filesystem::path root_;
};

}  // namespace microide::project
