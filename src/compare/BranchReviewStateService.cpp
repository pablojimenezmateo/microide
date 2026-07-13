#include "compare/BranchReviewStateService.h"

#include <algorithm>
#include <chrono>

namespace microide::compare {

namespace {

std::filesystem::path NormalizeReviewPath(const std::filesystem::path& path) {
  return path.lexically_normal();
}

bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right) {
  return NormalizeReviewPath(left) == NormalizeReviewPath(right);
}

bool HunkIdentitiesEqual(const BranchReviewHunkIdentity& left,
                         const BranchReviewHunkIdentity& right) {
  return PathsEqual(left.path, right.path) && left.old_start == right.old_start &&
         left.old_count == right.old_count && left.new_start == right.new_start &&
         left.new_count == right.new_count && left.content_hash == right.content_hash;
}

}  // namespace

std::uint64_t BranchReviewStateService::NowUnixMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

BranchReviewTargetState* BranchReviewStateService::FindOrCreateTarget(
    const BranchReviewTargetIdentity& target) {
  // Mutation entry point for every Mark*/Note operation. Bump the revision here so
  // cache consumers rebuild; over-bumping on a no-op re-mark is harmless.
  ++revision_;
  for (BranchReviewTargetState& existing : targets_) {
    if (existing.target == target) {
      TouchTarget(existing);
      return &existing;
    }
  }
  targets_.push_back(BranchReviewTargetState{
      .target = target,
      .reviewed_files = {},
      .reviewed_hunks = {},
      .notes = {},
      .last_accessed_unix_ms = NowUnixMs(),
  });
  return &targets_.back();
}

const BranchReviewTargetState* BranchReviewStateService::FindTarget(
    const BranchReviewTargetIdentity& target) const {
  for (const BranchReviewTargetState& existing : targets_) {
    if (existing.target == target) {
      return &existing;
    }
  }
  return nullptr;
}

BranchReviewTargetState* BranchReviewStateService::FindMutableTarget(
    const BranchReviewTargetIdentity& target) {
  for (BranchReviewTargetState& existing : targets_) {
    if (existing.target == target) {
      return &existing;
    }
  }
  return nullptr;
}

void BranchReviewStateService::TouchTarget(BranchReviewTargetState& target_state) {
  target_state.last_accessed_unix_ms = NowUnixMs();
}

void BranchReviewStateService::PruneTarget(BranchReviewTargetState& target_state) {
  // Prune by RECENCY, not insertion order. Re-reviewing a file/hunk or editing a
  // note refreshes its timestamp without moving it in the vector; pruning the
  // front (oldest-inserted) would drop a recently-touched entry that happened to
  // be inserted early. Stable-sort newest-first, then drop the tail past the cap.
  const auto prune = [](auto& entries, std::size_t cap, auto timestamp_of) {
    if (entries.size() <= cap) {
      return;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const auto& a, const auto& b) { return timestamp_of(a) > timestamp_of(b); });
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(cap), entries.end());
  };
  prune(target_state.reviewed_files, kMaxFileEntriesPerTarget,
        [](const BranchReviewFileReviewEntry& e) { return e.reviewed_at_unix_ms; });
  prune(target_state.reviewed_hunks, kMaxHunkEntriesPerTarget,
        [](const BranchReviewHunkReviewEntry& e) { return e.reviewed_at_unix_ms; });
  prune(target_state.notes, kMaxNotesPerTarget,
        [](const BranchReviewNote& e) { return e.updated_at_unix_ms; });
}

void BranchReviewStateService::MarkFileReviewed(const BranchReviewTargetIdentity& target,
                                                const std::filesystem::path& path) {
  BranchReviewTargetState* target_state = FindOrCreateTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(path);
  for (BranchReviewFileReviewEntry& entry : target_state->reviewed_files) {
    if (PathsEqual(entry.path, normalized)) {
      entry.reviewed_snapshot_generation = target.snapshot_generation;
      entry.reviewed_at_unix_ms = NowUnixMs();
      PruneTarget(*target_state);
      return;
    }
  }
  target_state->reviewed_files.push_back(BranchReviewFileReviewEntry{
      .path = normalized,
      .reviewed_snapshot_generation = target.snapshot_generation,
      .reviewed_at_unix_ms = NowUnixMs(),
  });
  PruneTarget(*target_state);
}

void BranchReviewStateService::MarkFileUnreviewed(const BranchReviewTargetIdentity& target,
                                                  const std::filesystem::path& path) {
  // Find-only: unreviewing a file for an unknown target must not create empty
  // state or bump the revision. Only a real removal bumps the revision.
  BranchReviewTargetState* target_state = FindMutableTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(path);
  auto& files = target_state->reviewed_files;
  const std::size_t before = files.size();
  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const BranchReviewFileReviewEntry& entry) {
                               return PathsEqual(entry.path, normalized);
                             }),
              files.end());
  if (files.size() != before) {
    ++revision_;
  }
}

void BranchReviewStateService::MarkHunkReviewed(const BranchReviewTargetIdentity& target,
                                                const BranchReviewHunkIdentity& identity) {
  BranchReviewTargetState* target_state = FindOrCreateTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const BranchReviewHunkIdentity normalized{
      .path = NormalizeReviewPath(identity.path),
      .old_start = identity.old_start,
      .old_count = identity.old_count,
      .new_start = identity.new_start,
      .new_count = identity.new_count,
      .content_hash = identity.content_hash,
  };
  for (BranchReviewHunkReviewEntry& entry : target_state->reviewed_hunks) {
    if (HunkIdentitiesEqual(entry.identity, normalized)) {
      entry.reviewed_at_unix_ms = NowUnixMs();
      PruneTarget(*target_state);
      return;
    }
  }
  target_state->reviewed_hunks.push_back(BranchReviewHunkReviewEntry{
      .identity = normalized,
      .reviewed_at_unix_ms = NowUnixMs(),
  });
  PruneTarget(*target_state);
}

void BranchReviewStateService::MarkHunkUnreviewed(const BranchReviewTargetIdentity& target,
                                                  const BranchReviewHunkIdentity& identity) {
  // Find-only: unreviewing a hunk for an unknown target is a clean no-op.
  BranchReviewTargetState* target_state = FindMutableTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const BranchReviewHunkIdentity normalized{
      .path = NormalizeReviewPath(identity.path),
      .old_start = identity.old_start,
      .old_count = identity.old_count,
      .new_start = identity.new_start,
      .new_count = identity.new_count,
      .content_hash = identity.content_hash,
  };
  auto& hunks = target_state->reviewed_hunks;
  const std::size_t before = hunks.size();
  hunks.erase(std::remove_if(hunks.begin(), hunks.end(),
                             [&](const BranchReviewHunkReviewEntry& entry) {
                               return HunkIdentitiesEqual(entry.identity, normalized);
                             }),
              hunks.end());
  if (hunks.size() != before) {
    ++revision_;
  }
}

void BranchReviewStateService::SetNote(const BranchReviewTargetIdentity& target,
                                       const BranchReviewNoteScope scope,
                                       const std::filesystem::path& path,
                                       const std::optional<BranchReviewHunkIdentity>& hunk_identity,
                                       const std::string_view text) {
  BranchReviewTargetState* target_state = FindOrCreateTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(path);
  std::optional<BranchReviewHunkIdentity> normalized_hunk;
  if (hunk_identity.has_value()) {
    normalized_hunk = *hunk_identity;
    normalized_hunk->path = NormalizeReviewPath(normalized_hunk->path);
  }
  for (BranchReviewNote& note : target_state->notes) {
    const bool same_scope = note.scope == scope;
    const bool same_path = PathsEqual(note.path, normalized);
    const bool same_hunk =
        (!note.hunk_identity.has_value() && !normalized_hunk.has_value()) ||
        (note.hunk_identity.has_value() && normalized_hunk.has_value() &&
         HunkIdentitiesEqual(*note.hunk_identity, *normalized_hunk));
    if (same_scope && same_path && same_hunk) {
      note.text = std::string(text);
      note.updated_at_unix_ms = NowUnixMs();
      PruneTarget(*target_state);
      return;
    }
  }
  target_state->notes.push_back(BranchReviewNote{
      .scope = scope,
      .path = normalized,
      .hunk_identity = normalized_hunk,
      .text = std::string(text),
      .updated_at_unix_ms = NowUnixMs(),
  });
  PruneTarget(*target_state);
}

void BranchReviewStateService::DeleteNote(const BranchReviewTargetIdentity& target,
                                          const BranchReviewNoteScope scope,
                                          const std::filesystem::path& path,
                                          const std::optional<BranchReviewHunkIdentity>& hunk_identity) {
  // Find-only: deleting a note for an unknown target is a clean no-op.
  BranchReviewTargetState* target_state = FindMutableTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(path);
  std::optional<BranchReviewHunkIdentity> normalized_hunk;
  if (hunk_identity.has_value()) {
    normalized_hunk = *hunk_identity;
    normalized_hunk->path = NormalizeReviewPath(normalized_hunk->path);
  }
  auto& notes = target_state->notes;
  const std::size_t before = notes.size();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [&](const BranchReviewNote& note) {
                               const bool same_scope = note.scope == scope;
                               const bool same_path = PathsEqual(note.path, normalized);
                               const bool same_hunk =
                                   (!note.hunk_identity.has_value() && !normalized_hunk.has_value()) ||
                                   (note.hunk_identity.has_value() && normalized_hunk.has_value() &&
                                    HunkIdentitiesEqual(*note.hunk_identity, *normalized_hunk));
                               return same_scope && same_path && same_hunk;
                             }),
              notes.end());
  if (notes.size() != before) {
    ++revision_;
  }
}

void BranchReviewStateService::ClearTarget(const BranchReviewTargetIdentity& target) {
  ++revision_;
  targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                [&](const BranchReviewTargetState& existing) {
                                  return existing.target == target;
                                }),
                     targets_.end());
}

void BranchReviewStateService::PruneForRepository(
    const std::filesystem::path& repository_root,
    const BranchReviewTargetIdentity* active_target) {
  ++revision_;
  const std::filesystem::path normalized_root = repository_root.lexically_normal();
  std::vector<BranchReviewTargetState> matching;
  matching.reserve(targets_.size());
  for (const BranchReviewTargetState& target_state : targets_) {
    if (target_state.target.repository_root.lexically_normal() == normalized_root) {
      matching.push_back(target_state);
    }
  }
  targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                [&](const BranchReviewTargetState& existing) {
                                  return existing.target.repository_root.lexically_normal() ==
                                         normalized_root;
                                }),
                     targets_.end());

  std::sort(matching.begin(), matching.end(),
            [](const BranchReviewTargetState& left, const BranchReviewTargetState& right) {
              return left.last_accessed_unix_ms > right.last_accessed_unix_ms;
            });

  if (matching.size() > kMaxTargetsPerRepository) {
    matching.resize(kMaxTargetsPerRepository);
  }
  if (active_target != nullptr) {
    const auto preserved_it = std::find_if(
        matching.begin(), matching.end(),
        [&](const BranchReviewTargetState& target_state) { return target_state.target == *active_target; });
    if (preserved_it == matching.end()) {
      if (matching.size() == kMaxTargetsPerRepository) {
        matching.back() = BranchReviewTargetState{
            .target = *active_target,
            .reviewed_files = {},
            .reviewed_hunks = {},
            .notes = {},
            .last_accessed_unix_ms = NowUnixMs(),
        };
      } else {
        matching.push_back(BranchReviewTargetState{
            .target = *active_target,
            .reviewed_files = {},
            .reviewed_hunks = {},
            .notes = {},
            .last_accessed_unix_ms = NowUnixMs(),
        });
      }
    } else if (preserved_it != matching.begin()) {
      std::rotate(matching.begin(), preserved_it, preserved_it + 1);
    }
  }
  for (BranchReviewTargetState& target_state : matching) {
    PruneTarget(target_state);
    targets_.push_back(std::move(target_state));
  }
}

BranchReviewMarkerStatus BranchReviewStateService::FileStatus(
    const BranchReviewStateQueryInput& input) const {
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return BranchReviewMarkerStatus::Unreviewed;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.path);
  const BranchReviewFileReviewEntry* file_entry = nullptr;
  for (const BranchReviewFileReviewEntry& entry : target_state->reviewed_files) {
    if (PathsEqual(entry.path, normalized)) {
      file_entry = &entry;
      break;
    }
  }
  if (file_entry == nullptr) {
    return BranchReviewMarkerStatus::Unreviewed;
  }
  if (file_entry->reviewed_snapshot_generation != input.target.snapshot_generation) {
    return BranchReviewMarkerStatus::ChangedSinceReviewed;
  }
  if (input.model != nullptr) {
    for (const BranchReviewHunkReviewEntry& hunk_entry : target_state->reviewed_hunks) {
      if (!PathsEqual(hunk_entry.identity.path, normalized)) {
        continue;
      }
      for (std::size_t hunk_index = 0; hunk_index < input.model->hunks.size(); ++hunk_index) {
        const BranchReviewHunkIdentity current = ComputeBranchReviewHunkIdentity(
            *input.model, static_cast<int>(hunk_index), normalized);
        if (current.old_start == hunk_entry.identity.old_start &&
            current.new_start == hunk_entry.identity.new_start &&
            !HunkIdentitiesEqual(hunk_entry.identity, current)) {
          return BranchReviewMarkerStatus::ChangedSinceReviewed;
        }
      }
    }
  }
  return BranchReviewMarkerStatus::Reviewed;
}

BranchReviewMarkerStatus BranchReviewStateService::HunkStatus(
    const BranchReviewStateQueryInput& input) const {
  if (input.model == nullptr || input.selected_hunk_index < 0) {
    return BranchReviewMarkerStatus::Unreviewed;
  }
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return BranchReviewMarkerStatus::Unreviewed;
  }
  const BranchReviewHunkIdentity current = ComputeBranchReviewHunkIdentity(
      *input.model, input.selected_hunk_index, NormalizeReviewPath(input.path));
  for (const BranchReviewHunkReviewEntry& entry : target_state->reviewed_hunks) {
    if (!PathsEqual(entry.identity.path, current.path)) {
      continue;
    }
    if (HunkIdentitiesEqual(entry.identity, current)) {
      return BranchReviewMarkerStatus::Reviewed;
    }
    if (entry.identity.old_start == current.old_start &&
        entry.identity.new_start == current.new_start) {
      return BranchReviewMarkerStatus::ChangedSinceReviewed;
    }
  }
  return FileStatus(input);
}

bool BranchReviewStateService::HasNote(const BranchReviewStateQueryInput& input,
                                       const BranchReviewNoteScope scope) const {
  return NoteText(input, scope).has_value();
}

std::optional<std::string> BranchReviewStateService::NoteText(
    const BranchReviewStateQueryInput& input,
    const BranchReviewNoteScope scope) const {
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.path);
  std::optional<BranchReviewHunkIdentity> current_hunk;
  if (scope == BranchReviewNoteScope::Hunk && input.model != nullptr &&
      input.selected_hunk_index >= 0) {
    current_hunk =
        ComputeBranchReviewHunkIdentity(*input.model, input.selected_hunk_index, normalized);
  }
  for (const BranchReviewNote& note : target_state->notes) {
    if (note.scope != scope || !PathsEqual(note.path, normalized)) {
      continue;
    }
    if (scope == BranchReviewNoteScope::File && !note.hunk_identity.has_value()) {
      return note.text;
    }
    if (scope == BranchReviewNoteScope::Hunk && note.hunk_identity.has_value() &&
        current_hunk.has_value() && HunkIdentitiesEqual(*note.hunk_identity, *current_hunk)) {
      return note.text;
    }
  }
  return std::nullopt;
}

}  // namespace microide::compare
