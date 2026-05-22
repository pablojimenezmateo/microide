#include "workspace/BranchReviewStateBridge.h"

namespace microide::workspace {

namespace {

PersistedBranchReviewHunkIdentity ToPersistedHunkIdentity(
    const compare::BranchReviewHunkIdentity& identity) {
  return PersistedBranchReviewHunkIdentity{
      .path = identity.path,
      .old_start = identity.old_start,
      .old_count = identity.old_count,
      .new_start = identity.new_start,
      .new_count = identity.new_count,
      .content_hash = identity.content_hash,
  };
}

compare::BranchReviewHunkIdentity FromPersistedHunkIdentity(
    const PersistedBranchReviewHunkIdentity& identity) {
  return compare::BranchReviewHunkIdentity{
      .path = identity.path,
      .old_start = identity.old_start,
      .old_count = identity.old_count,
      .new_start = identity.new_start,
      .new_count = identity.new_count,
      .content_hash = identity.content_hash,
  };
}

PersistedBranchReviewTarget ToPersistedTarget(const compare::BranchReviewTargetState& target_state) {
  PersistedBranchReviewTarget persisted{
      .repository_root = target_state.target.repository_root,
      .base_commit = target_state.target.base_commit,
      .head_commit = target_state.target.head_commit,
      .merge_base_commit = target_state.target.merge_base_commit,
      .snapshot_generation = target_state.target.snapshot_generation,
      .last_accessed_unix_ms = target_state.last_accessed_unix_ms,
  };
  for (const compare::BranchReviewFileReviewEntry& file : target_state.reviewed_files) {
    persisted.reviewed_files.push_back(PersistedBranchReviewFileEntry{
        .path = file.path,
        .reviewed_snapshot_generation = file.reviewed_snapshot_generation,
        .reviewed_at_unix_ms = file.reviewed_at_unix_ms,
    });
  }
  for (const compare::BranchReviewHunkReviewEntry& hunk : target_state.reviewed_hunks) {
    persisted.reviewed_hunks.push_back(PersistedBranchReviewHunkEntry{
        .identity = ToPersistedHunkIdentity(hunk.identity),
        .reviewed_at_unix_ms = hunk.reviewed_at_unix_ms,
    });
  }
  for (const compare::BranchReviewNote& note : target_state.notes) {
    persisted.notes.push_back(PersistedBranchReviewNote{
        .scope = note.scope == compare::BranchReviewNoteScope::File ? "file" : "hunk",
        .path = note.path,
        .hunk_identity =
            note.hunk_identity.has_value()
                ? std::optional<PersistedBranchReviewHunkIdentity>(
                      ToPersistedHunkIdentity(*note.hunk_identity))
                : std::nullopt,
        .text = note.text,
        .updated_at_unix_ms = note.updated_at_unix_ms,
    });
  }
  return persisted;
}

compare::BranchReviewTargetState FromPersistedTarget(const PersistedBranchReviewTarget& persisted) {
  compare::BranchReviewTargetState target_state{
      .target =
          compare::BranchReviewTargetIdentity{
              .repository_root = persisted.repository_root,
              .base_commit = persisted.base_commit,
              .head_commit = persisted.head_commit,
              .merge_base_commit = persisted.merge_base_commit,
              .snapshot_generation = persisted.snapshot_generation,
          },
      .last_accessed_unix_ms = persisted.last_accessed_unix_ms,
  };
  for (const PersistedBranchReviewFileEntry& file : persisted.reviewed_files) {
    target_state.reviewed_files.push_back(compare::BranchReviewFileReviewEntry{
        .path = file.path,
        .reviewed_snapshot_generation = file.reviewed_snapshot_generation,
        .reviewed_at_unix_ms = file.reviewed_at_unix_ms,
    });
  }
  for (const PersistedBranchReviewHunkEntry& hunk : persisted.reviewed_hunks) {
    target_state.reviewed_hunks.push_back(compare::BranchReviewHunkReviewEntry{
        .identity = FromPersistedHunkIdentity(hunk.identity),
        .reviewed_at_unix_ms = hunk.reviewed_at_unix_ms,
    });
  }
  for (const PersistedBranchReviewNote& note : persisted.notes) {
    std::optional<compare::BranchReviewHunkIdentity> hunk_identity;
    if (note.hunk_identity.has_value()) {
      hunk_identity = FromPersistedHunkIdentity(*note.hunk_identity);
    }
    target_state.notes.push_back(compare::BranchReviewNote{
        .scope = note.scope == "hunk" ? compare::BranchReviewNoteScope::Hunk
                                      : compare::BranchReviewNoteScope::File,
        .path = note.path,
        .hunk_identity = hunk_identity,
        .text = note.text,
        .updated_at_unix_ms = note.updated_at_unix_ms,
    });
  }
  return target_state;
}

}  // namespace

PersistedBranchReviewState ToPersistedBranchReviewState(
    const compare::BranchReviewStateService& service) {
  PersistedBranchReviewState persisted;
  for (const compare::BranchReviewTargetState& target_state : service.targets()) {
    persisted.targets.push_back(ToPersistedTarget(target_state));
  }
  return persisted;
}

void LoadBranchReviewStateFromPersisted(const PersistedBranchReviewState& persisted,
                                        compare::BranchReviewStateService* service) {
  if (service == nullptr) {
    return;
  }
  service->targets().clear();
  for (const PersistedBranchReviewTarget& target : persisted.targets) {
    service->targets().push_back(FromPersistedTarget(target));
  }
}

std::optional<compare::BranchReviewTargetIdentity> OutgoingBranchReviewTarget(
    const GitSidebarState& git_state,
    const std::filesystem::path& repository_root) {
  if (!git_state.repo_available || git_state.base_ref.empty() || repository_root.empty()) {
    return std::nullopt;
  }
  return compare::MakeBranchReviewTargetIdentity(repository_root, git_state.base_ref, "HEAD",
                                                 git_state.base_ref, git_state.snapshot_generation);
}

}  // namespace microide::workspace
