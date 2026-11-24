#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareModel.h"

namespace microide::compare {

struct BranchReviewStateQueryInput {
  BranchReviewTargetIdentity target;
  std::filesystem::path path;
  const CompareModel* model = nullptr;
  int selected_hunk_index = -1;
};

class BranchReviewStateService {
 public:
  static constexpr std::size_t kMaxTargetsPerRepository = 12;
  static constexpr std::size_t kMaxFileEntriesPerTarget = 512;
  static constexpr std::size_t kMaxHunkEntriesPerTarget = 1024;
  static constexpr std::size_t kMaxNotesPerTarget = 256;

  const std::vector<BranchReviewTargetState>& targets() const { return targets_; }
  // Mutable access exists for the persistence bridge (its only caller), which
  // replaces the whole vector on load. Anything written through it must satisfy
  // the same ingress invariant the mutators do: every stored path normalized
  // through NormalizeReviewPath.
  std::vector<BranchReviewTargetState>& targets() { return targets_; }

  // Monotonic counter bumped on every state mutation. Consumers that derive cached
  // output from review state (e.g. the git sidebar Outgoing review markers) key
  // their cache on this so a mutation forces a rebuild. Over-bumping is safe (it
  // only forces an extra rebuild); a missed bump would show stale markers, so all
  // mutation entry points must funnel through the bump (FindOrCreateTarget covers
  // the Mark*/Note mutators; ClearTarget/PruneForRepository bump directly).
  std::uint64_t revision() const { return revision_; }

  BranchReviewTargetState* FindOrCreateTarget(const BranchReviewTargetIdentity& target);
  const BranchReviewTargetState* FindTarget(const BranchReviewTargetIdentity& target) const;

  // `diff_fingerprint`: DiffContentFingerprint of the file's diff as reviewed (0
  // when it is not at hand); a later diff that differs shows it as changed.
  void MarkFileReviewed(const BranchReviewTargetIdentity& target,
                        const std::filesystem::path& path,
                        std::uint64_t diff_fingerprint = 0);
  void MarkFileUnreviewed(const BranchReviewTargetIdentity& target,
                          const std::filesystem::path& path);
  void MarkHunkReviewed(const BranchReviewTargetIdentity& target,
                        const BranchReviewHunkIdentity& identity);
  void MarkHunkUnreviewed(const BranchReviewTargetIdentity& target,
                          const BranchReviewHunkIdentity& identity);
  // The review action on one hunk of a file's diff (`model`), which also keeps
  // the file's own mark in step: the hunk that leaves no hunk unreviewed marks the
  // file reviewed -- its row in the git view says so, as when the file is marked
  // whole -- and unmarking a hunk takes the file's mark back (TD-2026-10-22-381).
  void SetHunkReviewed(const BranchReviewTargetIdentity& target,
                       const std::filesystem::path& path,
                       const CompareModel& model,
                       int hunk_index,
                       bool reviewed);

  // The review action on a whole file's diff: every hunk of `model` and the file
  // itself marked (or unmarked) in one pass. Each hunk keeps an entry of its own,
  // so a later edit shows as changed on the hunk it touched, not on the file's
  // every hunk; past the per-review cap those entries are the first to go
  // (PruneTarget), never another file's marks.
  void SetFileReviewed(const BranchReviewTargetIdentity& target,
                       const std::filesystem::path& path,
                       const CompareModel& model,
                       bool reviewed);

  void SetNote(const BranchReviewTargetIdentity& target,
               BranchReviewNoteScope scope,
               const std::filesystem::path& path,
               const std::optional<BranchReviewHunkIdentity>& hunk_identity,
               std::string_view text);
  void DeleteNote(const BranchReviewTargetIdentity& target,
                  BranchReviewNoteScope scope,
                  const std::filesystem::path& path,
                  const std::optional<BranchReviewHunkIdentity>& hunk_identity);

  void ClearTarget(const BranchReviewTargetIdentity& target);
  void PruneForRepository(const std::filesystem::path& repository_root,
                          const BranchReviewTargetIdentity* active_target);

  BranchReviewMarkerStatus FileStatus(const BranchReviewStateQueryInput& input) const;
  BranchReviewMarkerStatus HunkStatus(const BranchReviewStateQueryInput& input) const;

  // Resolve the marker status and note flag for EVERY hunk of `input.model` in
  // one pass, into `out` indexed by hunk. `input.selected_hunk_index` is ignored.
  //
  // This exists because the whole-file marker pass (which runs on the compare
  // tab's derived-state refresh, i.e. on the edit path) wants all of them, and
  // asking one hunk at a time is quadratic twice over: HunkStatus scans the
  // target's reviewed-hunk list per hunk, and when a hunk has no entry of its own
  // it falls back to FileStatus, which walks that list against every model hunk
  // again. Here each list is walked once — O(hunks + entries + notes) — and no
  // per-hunk identity (and so no per-hunk path copy) is built at all.
  //
  // A hunk note shows on the hunk at its position -- `note_outdated` when that
  // hunk's content changed since -- or, when an edit above moved the lines, on
  // the hunk with the same content. The file's hunk notes that land on no hunk
  // (the change is gone) come back in `detached`, so none is ever out of sight.
  void ResolveHunkMarkers(const BranchReviewStateQueryInput& input,
                          std::vector<BranchReviewHunkMarker>* out,
                          std::vector<BranchReviewNote>* detached = nullptr) const;
  // The note ResolveHunkMarkers shows on hunk `input.selected_hunk_index`, or
  // null: what an edit or a delete of that hunk's note acts on (its stored
  // identity may be older than the hunk's).
  const BranchReviewNote* NoteOnHunk(const BranchReviewStateQueryInput& input) const;

  bool HasNote(const BranchReviewStateQueryInput& input, BranchReviewNoteScope scope) const;
  std::optional<std::string> NoteText(const BranchReviewStateQueryInput& input,
                                      BranchReviewNoteScope scope) const;
  // Every note of `target` as a line someone can act on -- an agent in a
  // terminal, a commit message: `src/a.cpp:12-18: text` for a hunk's note (its
  // lines in the new file), `src/a.cpp (removed lines 4-6): text` for a hunk
  // that only removes, `src/a.cpp: text` for a file's. One per note, by path and
  // then line, newline-separated with no trailing newline; empty when it has
  // none. Paths are as stored: relative to the repository.
  //
  // `scope` narrows it to one file or everything under one directory (a
  // repository-relative path, as NormalizeReviewPath makes it); empty is the
  // whole review. Counted by NoteCount on the same rule.
  std::string NotesAsReferences(const BranchReviewTargetIdentity& target,
                                const std::filesystem::path& scope = {}) const;
  std::size_t NoteCount(const BranchReviewTargetIdentity& target,
                        const std::filesystem::path& scope = {}) const;

  // The review of `repository_root` touched last (its marks or notes changed, or
  // it was opened), or null when the repository has none: what "the review" is
  // when no review diff is in front.
  const BranchReviewTargetState* MostRecentTarget(const std::filesystem::path& repository_root) const;

 private:
  static std::uint64_t NowUnixMs();
  // Find an existing target for mutation WITHOUT creating one or bumping the
  // revision. Delete/unmark paths use this so operating on an unknown target is a
  // clean no-op instead of creating empty state and persisting noise.
  BranchReviewTargetState* FindMutableTarget(const BranchReviewTargetIdentity& target);
  void TouchTarget(BranchReviewTargetState& target_state);
  void PruneTarget(BranchReviewTargetState& target_state);

  std::vector<BranchReviewTargetState> targets_;
  std::uint64_t revision_ = 0;
};

}  // namespace microide::compare
