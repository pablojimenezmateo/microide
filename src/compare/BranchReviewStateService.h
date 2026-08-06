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

  void MarkFileReviewed(const BranchReviewTargetIdentity& target,
                        const std::filesystem::path& path);
  void MarkFileUnreviewed(const BranchReviewTargetIdentity& target,
                          const std::filesystem::path& path);
  void MarkHunkReviewed(const BranchReviewTargetIdentity& target,
                        const BranchReviewHunkIdentity& identity);
  void MarkHunkUnreviewed(const BranchReviewTargetIdentity& target,
                          const BranchReviewHunkIdentity& identity);

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
  bool HasNote(const BranchReviewStateQueryInput& input, BranchReviewNoteScope scope) const;
  std::optional<std::string> NoteText(const BranchReviewStateQueryInput& input,
                                      BranchReviewNoteScope scope) const;

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
