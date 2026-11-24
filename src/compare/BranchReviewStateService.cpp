#include "compare/BranchReviewStateService.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "util/PathMatch.h"

namespace microide::compare {

namespace {

bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right) {
  // Both sides are normalized already — see NormalizeReviewPath's contract — so
  // this is a string compare. It used to call lexically_normal() on BOTH sides on
  // every comparison, which is ~12 allocations a call inside loops that run per
  // reviewed-hunk entry per hunk per row: 79% of a branch-review marker pass.
  MICROIDE_ASSERT_NORMALIZED_REVIEW_PATH(left);
  MICROIDE_ASSERT_NORMALIZED_REVIEW_PATH(right);
  return left.native() == right.native();
}

bool HunkIdentitiesEqual(const BranchReviewHunkIdentity& left,
                         const BranchReviewHunkIdentity& right) {
  return PathsEqual(left.path, right.path) && left.old_start == right.old_start &&
         left.old_count == right.old_count && left.new_start == right.new_start &&
         left.new_count == right.new_count && left.content_hash == right.content_hash;
}

// A hunk's position in the diff — the pair both the "same hunk, new content" and
// the exact-match tests key on. Hunks own disjoint line ranges, so within one
// model this is unique; a pure insertion has old_start 0 and a pure deletion
// new_start 0, and those still differ in the other half.
std::uint64_t HunkPositionKey(const int old_start, const int new_start) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(old_start)) << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(new_start));
}

bool ContentKeysEqual(const BranchReviewHunkContentKey& key,
                      const BranchReviewHunkIdentity& identity) {
  return key.old_start == identity.old_start && key.old_count == identity.old_count &&
         key.new_start == identity.new_start && key.new_count == identity.new_count &&
         key.content_hash == identity.content_hash;
}

// The model's hunks, indexed by position, computed once per query instead of once
// per (stored entry x hunk) pair.
struct ModelHunkIndex {
  std::vector<BranchReviewHunkContentKey> keys;
  std::unordered_map<std::uint64_t, std::size_t> by_position;
};

// A file marked with its diff at hand, looked at with a diff whose changed text
// differs. Without either fingerprint nothing says it moved.
bool FileDiffMoved(const BranchReviewFileReviewEntry& entry, const CompareModel* model) {
  return model != nullptr && entry.reviewed_diff_fingerprint != 0 &&
         entry.reviewed_diff_fingerprint != DiffContentFingerprint(*model);
}

ModelHunkIndex BuildModelHunkIndex(const CompareModel& model) {
  ModelHunkIndex index;
  index.keys.reserve(model.hunks.size());
  index.by_position.reserve(model.hunks.size());
  for (std::size_t hunk_index = 0; hunk_index < model.hunks.size(); ++hunk_index) {
    index.keys.push_back(ComputeBranchReviewHunkContentKey(model, model.hunks[hunk_index]));
    // First wins, matching the linear scan this replaced, which returned on its
    // first positional match.
    index.by_position.emplace(HunkPositionKey(index.keys.back().old_start,
                                              index.keys.back().new_start),
                              hunk_index);
  }
  return index;
}

// Where a hunk note shows in the model: on the hunk at its position (outdated
// when that hunk's content is no longer what the note was written on), else on a
// hunk with the same content somewhere else -- an edit above it moved the lines --
// else nowhere (the change is gone).
struct NotePlacement {
  std::size_t hunk = 0;
  bool outdated = false;
};

std::optional<NotePlacement> PlaceNote(const ModelHunkIndex& index, const BranchReviewHunkIdentity& identity) {
  if (const auto it = index.by_position.find(HunkPositionKey(identity.old_start, identity.new_start));
      it != index.by_position.end()) {
    return NotePlacement{.hunk = it->second, .outdated = !ContentKeysEqual(index.keys[it->second], identity)};
  }
  for (std::size_t hunk = 0; hunk < index.keys.size(); ++hunk) {
    const BranchReviewHunkContentKey& key = index.keys[hunk];
    if (key.content_hash == identity.content_hash && key.old_count == identity.old_count &&
        key.new_count == identity.new_count) {
      return NotePlacement{.hunk = hunk, .outdated = false};
    }
  }
  return std::nullopt;
}

bool IsHunkNoteOf(const BranchReviewNote& note, const std::filesystem::path& normalized_path) {
  return note.scope == BranchReviewNoteScope::Hunk && note.hunk_identity.has_value() &&
         PathsEqual(note.path, normalized_path) && PathsEqual(note.hunk_identity->path, normalized_path);
}

// Does any reviewed-hunk entry for this file sit at a model hunk's position with
// different content? That is what demotes a reviewed FILE to "changed since
// reviewed". Walks the entry list once against the prebuilt model index.
bool AnyReviewedHunkContentMoved(const BranchReviewTargetState& target_state,
                                 const std::filesystem::path& normalized_path,
                                 const ModelHunkIndex& index) {
  for (const BranchReviewHunkReviewEntry& entry : target_state.reviewed_hunks) {
    if (!PathsEqual(entry.identity.path, normalized_path)) {
      continue;
    }
    const auto it =
        index.by_position.find(HunkPositionKey(entry.identity.old_start, entry.identity.new_start));
    if (it == index.by_position.end()) {
      continue;
    }
    if (!ContentKeysEqual(index.keys[it->second], entry.identity)) {
      return true;
    }
  }
  return false;
}

// `identity` with its path in the form the store keys hunks by. Built field by
// field rather than copied and patched: a copy would allocate the original path
// only to overwrite it, once per hunk the review touches.
BranchReviewHunkIdentity WithNormalizedPath(const std::filesystem::path& repository_root,
                                            const BranchReviewHunkIdentity& identity) {
  return BranchReviewHunkIdentity{
      .path = NormalizeReviewPath(repository_root, identity.path),
      .old_start = identity.old_start,
      .old_count = identity.old_count,
      .new_start = identity.new_start,
      .new_count = identity.new_count,
      .content_hash = identity.content_hash,
  };
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
  prune(target_state.notes, kMaxNotesPerTarget,
        [](const BranchReviewNote& e) { return e.updated_at_unix_ms; });

  // Hunk entries: the ones of a file that is marked whole go first. Losing one
  // of those costs nothing while the file stands as reviewed (the hunk falls back
  // to the file's mark); losing a hunk marked on its own unmarks it. Marking one
  // large file reviewed therefore never takes another file's marks.
  auto& hunks = target_state.reviewed_hunks;
  if (hunks.size() <= kMaxHunkEntriesPerTarget) {
    return;
  }
  std::vector<std::string_view> whole_files;
  whole_files.reserve(target_state.reviewed_files.size());
  for (const BranchReviewFileReviewEntry& file : target_state.reviewed_files) {
    whole_files.push_back(file.path.native());
  }
  std::ranges::sort(whole_files);
  const auto covered = [&](const BranchReviewHunkReviewEntry& entry) {
    return std::ranges::binary_search(whole_files, std::string_view(entry.identity.path.native()));
  };
  std::stable_sort(hunks.begin(), hunks.end(), [](const auto& a, const auto& b) {
    return a.reviewed_at_unix_ms > b.reviewed_at_unix_ms;
  });
  std::stable_partition(hunks.begin(), hunks.end(), [&](const auto& entry) { return !covered(entry); });
  hunks.erase(hunks.begin() + static_cast<std::ptrdiff_t>(kMaxHunkEntriesPerTarget), hunks.end());
}

void BranchReviewStateService::MarkFileReviewed(const BranchReviewTargetIdentity& target,
                                                const std::filesystem::path& path,
                                                const std::uint64_t diff_fingerprint) {
  BranchReviewTargetState* target_state = FindOrCreateTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(target.repository_root, path);
  for (BranchReviewFileReviewEntry& entry : target_state->reviewed_files) {
    if (PathsEqual(entry.path, normalized)) {
      entry.reviewed_diff_fingerprint = diff_fingerprint;
      entry.reviewed_at_unix_ms = NowUnixMs();
      PruneTarget(*target_state);
      return;
    }
  }
  target_state->reviewed_files.push_back(BranchReviewFileReviewEntry{
      .path = normalized,
      .reviewed_diff_fingerprint = diff_fingerprint,
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
  const std::filesystem::path normalized = NormalizeReviewPath(target.repository_root, path);
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
  const BranchReviewHunkIdentity normalized =
      WithNormalizedPath(target.repository_root, identity);
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
  const BranchReviewHunkIdentity normalized =
      WithNormalizedPath(target.repository_root, identity);
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
  const std::filesystem::path normalized = NormalizeReviewPath(target.repository_root, path);
  std::optional<BranchReviewHunkIdentity> normalized_hunk;
  if (hunk_identity.has_value()) {
    normalized_hunk = *hunk_identity;
    normalized_hunk->path = NormalizeReviewPath(target.repository_root, normalized_hunk->path);
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
  const std::filesystem::path normalized = NormalizeReviewPath(target.repository_root, path);
  std::optional<BranchReviewHunkIdentity> normalized_hunk;
  if (hunk_identity.has_value()) {
    normalized_hunk = *hunk_identity;
    normalized_hunk->path = NormalizeReviewPath(target.repository_root, normalized_hunk->path);
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
  // Emptied, not forgotten: a remote project's review state is merged between
  // the user's machines, the copy of a target changed last winning
  // (workspace/persistence/RoamingReview.h). A target that was simply gone here
  // would come back, marks and all, from the machine that still held it; emptied
  // at this time it is the newer copy. It ages out like any other target.
  if (BranchReviewTargetState* existing = FindMutableTarget(target)) {
    existing->reviewed_files.clear();
    existing->reviewed_hunks.clear();
    existing->notes.clear();
    TouchTarget(*existing);
  }
}

void BranchReviewStateService::PruneForRepository(
    const std::filesystem::path& repository_root,
    const BranchReviewTargetIdentity* active_target) {
  ++revision_;
  // Normalize the query once and reject a mismatching target with a string
  // compare: every stored repository_root arrived normalized, so re-normalizing
  // one per target (twice -- the partition and the erase both scanned) spent ~12
  // allocations apiece to confirm what the text already said (TD-2026-08-10-174).
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_root =
      util::PathTextNeedsNormalizing(repository_root.native())
          ? (normalized_storage = repository_root.lexically_normal())
          : repository_root;
  const auto matches_root = [&normalized_root](const BranchReviewTargetState& state) {
    return util::SameAsNormalizedPath(state.target.repository_root, normalized_root);
  };
  std::vector<BranchReviewTargetState> matching;
  matching.reserve(targets_.size());
  for (const BranchReviewTargetState& target_state : targets_) {
    if (matches_root(target_state)) {
      matching.push_back(target_state);
    }
  }
  targets_.erase(std::remove_if(targets_.begin(), targets_.end(), matches_root), targets_.end());

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

void BranchReviewStateService::SetHunkReviewed(const BranchReviewTargetIdentity& target,
                                               const std::filesystem::path& path,
                                               const CompareModel& model,
                                               int hunk_index,
                                               bool reviewed) {
  if (hunk_index < 0 || static_cast<std::size_t>(hunk_index) >= model.hunks.size()) {
    return;
  }
  const BranchReviewHunkIdentity identity = ComputeBranchReviewHunkIdentity(model, hunk_index, path);
  if (!reviewed) {
    MarkHunkUnreviewed(target, identity);
    MarkFileUnreviewed(target, path);
    return;
  }
  MarkHunkReviewed(target, identity);
  const BranchReviewStateQueryInput query{.target = target, .path = path, .model = &model};
  if (FileStatus(query) == BranchReviewMarkerStatus::Reviewed) {
    return;  // marked whole already
  }
  std::vector<BranchReviewHunkMarker> markers;
  ResolveHunkMarkers(query, &markers);
  const bool every_hunk_reviewed =
      std::all_of(markers.begin(), markers.end(), [](const BranchReviewHunkMarker& marker) {
        return marker.status == BranchReviewMarkerStatus::Reviewed;
      });
  if (every_hunk_reviewed) {
    MarkFileReviewed(target, path, DiffContentFingerprint(model));
  }
}

void BranchReviewStateService::SetFileReviewed(const BranchReviewTargetIdentity& target,
                                               const std::filesystem::path& path,
                                               const CompareModel& model,
                                               bool reviewed) {
  BranchReviewTargetState* target_state = reviewed ? FindOrCreateTarget(target) : FindMutableTarget(target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(target.repository_root, path);
  // The file's hunk entries are replaced wholesale: one pass out, one pass in,
  // one prune -- not a resolve of the whole file per hunk.
  const std::size_t removed = std::erase_if(target_state->reviewed_hunks, [&](const BranchReviewHunkReviewEntry& entry) {
    return PathsEqual(entry.identity.path, normalized);
  });
  if (!reviewed) {
    if (removed != 0) {
      ++revision_;
    }
    MarkFileUnreviewed(target, path);
    return;
  }
  const std::uint64_t now = NowUnixMs();
  target_state->reviewed_hunks.reserve(target_state->reviewed_hunks.size() + model.hunks.size());
  for (const CompareHunk& hunk : model.hunks) {
    target_state->reviewed_hunks.push_back(BranchReviewHunkReviewEntry{
        .identity = ComputeBranchReviewHunkIdentity(model, hunk, normalized),
        .reviewed_at_unix_ms = now,
    });
  }
  MarkFileReviewed(target, path, DiffContentFingerprint(model));  // prunes
}

BranchReviewMarkerStatus BranchReviewStateService::FileStatus(
    const BranchReviewStateQueryInput& input) const {
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return BranchReviewMarkerStatus::Unreviewed;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.target.repository_root, input.path);
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
  if (FileDiffMoved(*file_entry, input.model)) {
    return BranchReviewMarkerStatus::ChangedSinceReviewed;
  }
  if (input.model != nullptr &&
      AnyReviewedHunkContentMoved(*target_state, normalized, BuildModelHunkIndex(*input.model))) {
    // Was a nested walk that recomputed every model hunk's identity — content hash
    // and all — once per reviewed-hunk entry: O(entries x hunks) hashes of the
    // whole file's changed text, per call, on a path that is itself called per
    // hunk by HunkStatus's fallback.
    return BranchReviewMarkerStatus::ChangedSinceReviewed;
  }
  return BranchReviewMarkerStatus::Reviewed;
}

void BranchReviewStateService::ResolveHunkMarkers(
    const BranchReviewStateQueryInput& input,
    std::vector<BranchReviewHunkMarker>* out,
    std::vector<BranchReviewNote>* detached) const {
  if (detached != nullptr) {
    detached->clear();
  }
  if (out == nullptr) {
    return;
  }
  out->assign(input.model == nullptr ? 0 : input.model->hunks.size(), BranchReviewHunkMarker{});
  if (input.model == nullptr || out->empty()) {
    return;
  }
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.target.repository_root, input.path);
  const ModelHunkIndex index = BuildModelHunkIndex(*input.model);

  // Pass 1 — each hunk's own reviewed entry, if it has one. First entry at a
  // position wins, matching the per-hunk scan this replaced.
  std::vector<bool> has_own_entry(out->size(), false);
  for (const BranchReviewHunkReviewEntry& entry : target_state->reviewed_hunks) {
    if (!PathsEqual(entry.identity.path, normalized)) {
      continue;
    }
    const auto it =
        index.by_position.find(HunkPositionKey(entry.identity.old_start, entry.identity.new_start));
    if (it == index.by_position.end() || has_own_entry[it->second]) {
      continue;
    }
    has_own_entry[it->second] = true;
    (*out)[it->second].status = ContentKeysEqual(index.keys[it->second], entry.identity)
                                    ? BranchReviewMarkerStatus::Reviewed
                                    : BranchReviewMarkerStatus::ChangedSinceReviewed;
  }

  // Pass 2 — the file-level fallback for every hunk without its own entry. It is
  // the same answer for all of them, so resolve it once.
  const bool needs_file_fallback =
      std::find(has_own_entry.begin(), has_own_entry.end(), false) != has_own_entry.end();
  if (needs_file_fallback) {
    BranchReviewMarkerStatus file_status = BranchReviewMarkerStatus::Unreviewed;
    const BranchReviewFileReviewEntry* file_entry = nullptr;
    for (const BranchReviewFileReviewEntry& entry : target_state->reviewed_files) {
      if (PathsEqual(entry.path, normalized)) {
        file_entry = &entry;
        break;
      }
    }
    if (file_entry != nullptr) {
      file_status = FileDiffMoved(*file_entry, input.model) ||
                            AnyReviewedHunkContentMoved(*target_state, normalized, index)
                        ? BranchReviewMarkerStatus::ChangedSinceReviewed
                        : BranchReviewMarkerStatus::Reviewed;
    }
    if (file_status != BranchReviewMarkerStatus::Unreviewed) {
      for (std::size_t i = 0; i < out->size(); ++i) {
        if (!has_own_entry[i]) {
          (*out)[i].status = file_status;
        }
      }
    }
  }

  // Pass 3 — hunk-scoped notes, each on the hunk PlaceNote finds for it. One
  // note a hunk: a second that lands on the same hunk, and one whose change is
  // gone, are `detached` -- still the file's, still shown and exported.
  for (const BranchReviewNote& note : target_state->notes) {
    if (!IsHunkNoteOf(note, normalized)) {
      continue;
    }
    const std::optional<NotePlacement> placement = PlaceNote(index, *note.hunk_identity);
    if (placement.has_value() && !(*out)[placement->hunk].has_note) {
      BranchReviewHunkMarker& marker = (*out)[placement->hunk];
      marker.has_note = true;
      marker.note_outdated = placement->outdated;
      marker.note_text = note.text;
    } else if (detached != nullptr) {
      detached->push_back(note);
    }
  }
}

const BranchReviewNote* BranchReviewStateService::NoteOnHunk(const BranchReviewStateQueryInput& input) const {
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr || input.model == nullptr || input.selected_hunk_index < 0 ||
      static_cast<std::size_t>(input.selected_hunk_index) >= input.model->hunks.size()) {
    return nullptr;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.target.repository_root, input.path);
  const ModelHunkIndex index = BuildModelHunkIndex(*input.model);
  // The first note placed on the hunk, as ResolveHunkMarkers shows it.
  for (const BranchReviewNote& note : target_state->notes) {
    if (!IsHunkNoteOf(note, normalized)) {
      continue;
    }
    if (const std::optional<NotePlacement> placement = PlaceNote(index, *note.hunk_identity);
        placement.has_value() && placement->hunk == static_cast<std::size_t>(input.selected_hunk_index)) {
      return &note;
    }
  }
  return nullptr;
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
      *input.model, input.selected_hunk_index, NormalizeReviewPath(input.target.repository_root, input.path));
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

namespace {

// Whether a note on `path` falls under `scope`: the file itself, or a file below
// the directory it names. Both are repository-relative and normalized; an empty
// scope is the whole review.
bool NoteInScope(const std::filesystem::path& path, const std::filesystem::path& scope) {
  if (scope.empty() || scope == ".") {
    return true;
  }
  const std::string_view note = path.native();
  std::string_view prefix = scope.native();
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.remove_suffix(1);
  }
  return note.starts_with(prefix) && (note.size() == prefix.size() || note[prefix.size()] == '/');
}

}  // namespace

std::size_t BranchReviewStateService::NoteCount(const BranchReviewTargetIdentity& target,
                                                const std::filesystem::path& scope) const {
  const BranchReviewTargetState* target_state = FindTarget(target);
  if (target_state == nullptr) {
    return 0;
  }
  const std::filesystem::path normalized_scope = NormalizeReviewPath(target.repository_root, scope);
  return static_cast<std::size_t>(std::ranges::count_if(target_state->notes, [&](const BranchReviewNote& note) {
    return NoteInScope(note.path, normalized_scope);
  }));
}

const BranchReviewTargetState* BranchReviewStateService::MostRecentTarget(
    const std::filesystem::path& repository_root) const {
  const BranchReviewTargetState* best = nullptr;
  const std::filesystem::path root = repository_root.lexically_normal();
  for (const BranchReviewTargetState& state : targets_) {
    if (state.target.repository_root.lexically_normal() != root) {
      continue;
    }
    if (best == nullptr || state.last_accessed_unix_ms > best->last_accessed_unix_ms) {
      best = &state;
    }
  }
  return best;
}

std::string BranchReviewStateService::NotesAsReferences(const BranchReviewTargetIdentity& target,
                                                        const std::filesystem::path& scope) const {
  const BranchReviewTargetState* target_state = FindTarget(target);
  if (target_state == nullptr || target_state->notes.empty()) {
    return {};
  }
  const std::filesystem::path normalized_scope = NormalizeReviewPath(target.repository_root, scope);
  // Where a note points: 0 for a file's, else its hunk's first line (the new
  // file's, or the old file's for a hunk that only removes).
  const auto line_of = [](const BranchReviewNote& note) {
    if (note.scope != BranchReviewNoteScope::Hunk || !note.hunk_identity.has_value()) {
      return 0;
    }
    return note.hunk_identity->new_count > 0 ? note.hunk_identity->new_start : note.hunk_identity->old_start;
  };
  std::vector<const BranchReviewNote*> notes;
  notes.reserve(target_state->notes.size());
  for (const BranchReviewNote& note : target_state->notes) {
    if (NoteInScope(note.path, normalized_scope)) {
      notes.push_back(&note);
    }
  }
  std::ranges::stable_sort(notes, [&](const BranchReviewNote* left, const BranchReviewNote* right) {
    if (left->path != right->path) {
      return left->path.native() < right->path.native();
    }
    return line_of(*left) < line_of(*right);
  });
  const auto span = [](int start, int count) {
    return count > 1 ? std::to_string(start) + "-" + std::to_string(start + count - 1) : std::to_string(start);
  };
  std::string text;
  for (const BranchReviewNote* note : notes) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += note->path.generic_string();
    if (note->scope == BranchReviewNoteScope::Hunk && note->hunk_identity.has_value()) {
      const BranchReviewHunkIdentity& hunk = *note->hunk_identity;
      if (hunk.new_count > 0) {
        text += ":" + span(hunk.new_start, hunk.new_count);
      } else if (hunk.old_count > 0) {
        text += " (removed " + std::string(hunk.old_count > 1 ? "lines " : "line ") +
                span(hunk.old_start, hunk.old_count) + ")";
      }
    }
    text += ": ";
    // One line a note, whatever it was typed as.
    for (const char c : note->text) {
      text.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
  }
  return text;
}

std::optional<std::string> BranchReviewStateService::NoteText(
    const BranchReviewStateQueryInput& input,
    const BranchReviewNoteScope scope) const {
  const BranchReviewTargetState* target_state = FindTarget(input.target);
  if (target_state == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path normalized = NormalizeReviewPath(input.target.repository_root, input.path);
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
