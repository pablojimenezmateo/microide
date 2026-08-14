#include "TestSupport.h"

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareModel.h"
#include "workspace/git/BranchReviewPersistence.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::tests {

using microide::compare::BranchReviewMarkerStatus;
using microide::compare::BranchReviewNoteScope;
using microide::compare::BranchReviewStateQueryInput;
using microide::compare::BranchReviewStateService;
using microide::compare::BranchReviewTargetIdentity;
using microide::compare::BuildCompareModel;
using microide::compare::ComputeBranchReviewHunkIdentity;
using microide::compare::MakeBranchReviewTargetIdentity;
using microide::workspace::DecodeProjectConfigRecord;
using microide::workspace::EncodeProjectConfigRecord;
using microide::workspace::LoadBranchReviewStateFromPersisted;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::ToPersistedBranchReviewState;

void RegisterBranchReviewStateTests(std::vector<TestCase>& tests) {
  // Regression: unreviewing / deleting for an unknown target must be a clean
  // no-op — it must not create empty target state or bump the revision.
  tests.push_back({"BranchReviewState/UnreviewMissingTargetIsNoOp",
                   [] {
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 1);
                     BranchReviewStateService service;
                     const std::uint64_t before = service.revision();
                     service.MarkFileUnreviewed(target, std::filesystem::path("nope.cpp"));
                     Expect(service.FindTarget(target) == nullptr,
                            "unreviewing an unknown target must not create state");
                     Expect(service.revision() == before,
                            "a no-op unreview must not bump the revision");

                     // A real unreview (after a review) DOES bump the revision.
                     service.MarkFileReviewed(target, std::filesystem::path("a.cpp"));
                     const std::uint64_t after_review = service.revision();
                     service.MarkFileUnreviewed(target, std::filesystem::path("a.cpp"));
                     Expect(service.revision() > after_review,
                            "removing a real review bumps the revision");
                   }});

  tests.push_back({"BranchReviewState/HunkIdentityAndChangedMarker",
                   [] {
                     const auto model = BuildCompareModel("old line\n", "new line\n");
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
                     const auto identity =
                         ComputeBranchReviewHunkIdentity(model, 0, std::filesystem::path("a.cpp"));

                     BranchReviewStateService service;
                     service.MarkHunkReviewed(target, identity);

                     BranchReviewStateQueryInput query{
                         .target = target,
                         .path = std::filesystem::path("a.cpp"),
                         .model = &model,
                         .selected_hunk_index = 0,
                     };
                     Expect(service.HunkStatus(query) == BranchReviewMarkerStatus::Reviewed,
                            "reviewed hunk should stay reviewed");

                     const auto changed_model = BuildCompareModel("old line\n", "changed line\n");
                     query.model = &changed_model;
                     Expect(service.HunkStatus(query) == BranchReviewMarkerStatus::ChangedSinceReviewed,
                            "edited hunk content should mark changed since reviewed");
                   }});

  // ResolveHunkMarkers is a batched rewrite of "call HunkStatus + HasNote once per
  // hunk". Diff it against that oracle across the state combinations that select
  // different branches inside it: no state at all, hunk-only reviews, a reviewed
  // file supplying the fallback, a reviewed file whose hunks then moved, a stale
  // snapshot generation, and hunk notes (one live, one whose hunk content moved).
  tests.push_back({"BranchReviewState/ResolveHunkMarkersMatchesPerHunkOracle",
                   [] {
                     std::string left;
                     std::string right;
                     for (int i = 0; i < 40; ++i) {
                       const std::string n = std::to_string(i);
                       left += "line " + n + " original\n";
                       right += (i % 5 == 0 ? "line " + n + " MODIFIED\n"
                                            : "line " + n + " original\n");
                     }
                     const auto model = BuildCompareModel(left, right);
                     Expect(model.hunks.size() >= 4, "the fixture needs several hunks");

                     const std::filesystem::path path("src/a.cpp");
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
                     const BranchReviewTargetIdentity stale_target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 4);

                     // A model whose hunk 1 content moved, so an identity recorded
                     // against `model` resolves as changed against this one.
                     std::string moved_right = right;
                     const std::string moved_marker = "line 5 MODIFIED";
                     const std::size_t at = moved_right.find(moved_marker);
                     Expect(at != std::string::npos, "the fixture line should exist");
                     moved_right.replace(at, moved_marker.size(), "line 5 EDITED-X");
                     const auto moved_model = BuildCompareModel(left, moved_right);

                     // Vacuity guard: a diff-against-oracle test passes trivially if
                     // every hunk in every case resolves Unreviewed with no note.
                     bool saw_reviewed = false;
                     bool saw_changed = false;
                     bool saw_unreviewed = false;
                     bool saw_note = false;

                     const auto check = [&](const char* what,
                                            const BranchReviewStateService& service,
                                            const BranchReviewTargetIdentity& query_target,
                                            const compare::CompareModel& query_model) {
                       std::vector<compare::BranchReviewHunkMarker> batched;
                       BranchReviewStateQueryInput query{
                           .target = query_target,
                           .path = path,
                           .model = &query_model,
                       };
                       service.ResolveHunkMarkers(query, &batched);
                       Expect(batched.size() == query_model.hunks.size(),
                              "the resolve must produce one marker per hunk");
                       for (std::size_t i = 0; i < query_model.hunks.size(); ++i) {
                         query.selected_hunk_index = static_cast<int>(i);
                         Expect(batched[i].status == service.HunkStatus(query),
                                std::string("batched hunk status must match HunkStatus: ") + what);
                         Expect(batched[i].has_note ==
                                    service.HasNote(query, BranchReviewNoteScope::Hunk),
                                std::string("batched note flag must match HasNote: ") + what);
                         saw_reviewed |= batched[i].status == BranchReviewMarkerStatus::Reviewed;
                         saw_changed |=
                             batched[i].status == BranchReviewMarkerStatus::ChangedSinceReviewed;
                         saw_unreviewed |=
                             batched[i].status == BranchReviewMarkerStatus::Unreviewed;
                         saw_note |= batched[i].has_note;
                       }
                     };

                     BranchReviewStateService service;
                     check("empty state", service, target, model);

                     service.MarkHunkReviewed(
                         target, ComputeBranchReviewHunkIdentity(model, 1, path));
                     service.MarkHunkReviewed(
                         target, ComputeBranchReviewHunkIdentity(model, 3, path));
                     check("hunk reviews only", service, target, model);
                     check("hunk reviews, content moved", service, target, moved_model);

                     service.MarkFileReviewed(target, path);
                     check("file review supplies the fallback", service, target, model);
                     check("file review, content moved", service, target, moved_model);
                     check("stale snapshot generation", service, stale_target, model);

                     service.SetNote(target, BranchReviewNoteScope::Hunk, path,
                                     ComputeBranchReviewHunkIdentity(model, 2, path), "look");
                     service.SetNote(target, BranchReviewNoteScope::Hunk, path,
                                     ComputeBranchReviewHunkIdentity(model, 1, path), "and here");
                     service.SetNote(target, BranchReviewNoteScope::File, path, std::nullopt,
                                     "file-scoped, must not leak onto a hunk");
                     check("hunk notes", service, target, model);
                     check("hunk notes, content moved", service, target, moved_model);

                     Expect(saw_reviewed && saw_changed && saw_unreviewed && saw_note,
                            "the fixture must exercise every marker status and a live note");
                   }});

  tests.push_back({"BranchReviewState/HunkContentHashSeparatesLeftAndRight",
                   [] {
                     // Two hunks with identical row kind and line ranges whose per-row
                     // left+right byte concatenation is the same, but split differently:
                     // ("hello","world") vs ("hell","oworld"). The old undelimited hash
                     // collided on these; the length-prefixed hash must tell them apart.
                     auto make_model = [](std::string_view left, std::string_view right) {
                       compare::CompareModel model;
                       // A row's text is a VIEW (TD-2026-08-14-232), so the bytes
                       // have to live somewhere that outlives the row. Put them in
                       // the model's own source buffers, exactly where the builder
                       // puts them — assigning a temporary std::string to
                       // `left_text` compiles and dangles.
                       model.left_source = compare::MakeCompareText(std::string(left));
                       model.right_source = compare::MakeCompareText(std::string(right));
                       compare::CompareRow row;
                       row.kind = compare::CompareRowKind::Modified;
                       row.left_text = *model.left_source;
                       row.right_text = *model.right_source;
                       row.left_line = 1;
                       row.right_line = 1;
                       model.rows.push_back(std::move(row));
                       model.hunks.push_back(compare::CompareHunk{.start_row = 0, .end_row = 0});
                       return model;
                     };
                     const auto model_a = make_model("hello", "world");
                     const auto model_b = make_model("hell", "oworld");
                     const auto id_a = ComputeBranchReviewHunkIdentity(
                         model_a, 0, std::filesystem::path("a.cpp"));
                     const auto id_b = ComputeBranchReviewHunkIdentity(
                         model_b, 0, std::filesystem::path("a.cpp"));
                     Expect(id_a.old_start == id_b.old_start && id_a.new_start == id_b.new_start &&
                                id_a.old_count == id_b.old_count && id_a.new_count == id_b.new_count,
                            "the two hunks share identical line ranges");
                     Expect(id_a.content_hash != id_b.content_hash,
                            "differently-split left/right text must not share a content hash");
                     Expect(!(id_a == id_b),
                            "the hunk identities must differ so a changed hunk is not stale-reviewed");
                   }});

  tests.push_back({"BranchReviewState/FileReviewSurvivesTargetReload",
                   [] {
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 1);
                     BranchReviewStateService service;
                     service.MarkFileReviewed(target, std::filesystem::path("src/a.cpp"));

                     PersistedProjectConfigState persisted{
                         .project_base_color = std::nullopt,
                         .settings = {},
                         .sidebar_policies = {},
                         .commit_draft = std::nullopt,
                         .branch_review = ToPersistedBranchReviewState(service),
                     };
                     std::vector<std::byte> encoded;
                     Expect(EncodeProjectConfigRecord(persisted, &encoded),
                            "project config with branch review should encode");

                     PersistedProjectConfigState decoded;
                     Expect(DecodeProjectConfigRecord(encoded, &decoded),
                            "project config with branch review should decode");
                     Expect(decoded.branch_review.targets.size() == 1,
                            "branch review target should round-trip");
                     Expect(decoded.branch_review.targets[0].reviewed_files.size() == 1,
                            "reviewed file entry should round-trip");

                     BranchReviewStateService reloaded;
                     LoadBranchReviewStateFromPersisted(decoded.branch_review, &reloaded);
                     BranchReviewStateQueryInput query{
                         .target = target,
                         .path = std::filesystem::path("src/a.cpp"),
                     };
                     Expect(reloaded.FileStatus(query) == BranchReviewMarkerStatus::Reviewed,
                            "reloaded review state should restore reviewed file");
                   }});

  // Regression: query scans compare stored paths as plain strings now, which is
  // only correct because every ingress normalizes. The persistence file is the one
  // ingress that does not run through a mutator — a config written by an older
  // build (or hand-edited) can hold "./src/a.cpp", and an un-normalized entry
  // would compare unequal to the query's "src/a.cpp" and silently read as
  // "never reviewed".
  tests.push_back({"BranchReviewState/PersistedUnnormalizedPathsNormalizeOnLoad",
                   [] {
                     using microide::workspace::PersistedBranchReviewFileEntry;
                     using microide::workspace::PersistedBranchReviewHunkEntry;
                     using microide::workspace::PersistedBranchReviewHunkIdentity;
                     using microide::workspace::PersistedBranchReviewNote;
                     using microide::workspace::PersistedBranchReviewState;
                     using microide::workspace::PersistedBranchReviewTarget;

                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
                     PersistedBranchReviewHunkIdentity hunk_identity{
                         .path = std::filesystem::path("./src/./a.cpp"),
                         .old_start = 1,
                         .old_count = 2,
                         .new_start = 1,
                         .new_count = 2,
                         .content_hash = 0x1234,
                     };
                     PersistedBranchReviewTarget persisted_target{
                         .repository_root = target.repository_root,
                         .base_commit = target.base_commit,
                         .head_commit = target.head_commit,
                         .merge_base_commit = target.merge_base_commit,
                         .snapshot_generation = target.snapshot_generation,
                         .last_accessed_unix_ms = 1,
                         .reviewed_files = {PersistedBranchReviewFileEntry{
                             .path = std::filesystem::path("./src/./a.cpp"),
                             .reviewed_snapshot_generation = target.snapshot_generation,
                             .reviewed_at_unix_ms = 1,
                         }},
                         .reviewed_hunks = {PersistedBranchReviewHunkEntry{
                             .identity = hunk_identity,
                             .reviewed_at_unix_ms = 1,
                         }},
                         .notes = {PersistedBranchReviewNote{
                             .scope = "file",
                             .path = std::filesystem::path("./src/./a.cpp"),
                             .hunk_identity = std::nullopt,
                             .text = "look again",
                             .updated_at_unix_ms = 1,
                         }},
                     };
                     PersistedBranchReviewState persisted;
                     persisted.targets.push_back(std::move(persisted_target));

                     BranchReviewStateService service;
                     LoadBranchReviewStateFromPersisted(persisted, &service);
                     const compare::BranchReviewTargetState* loaded = service.FindTarget(target);
                     Expect(loaded != nullptr, "the persisted target should load");
                     Expect(loaded->reviewed_files[0].path == std::filesystem::path("src/a.cpp"),
                            "a persisted file path must be normalized on load");
                     Expect(loaded->reviewed_hunks[0].identity.path ==
                                std::filesystem::path("src/a.cpp"),
                            "a persisted hunk identity path must be normalized on load");
                     Expect(loaded->notes[0].path == std::filesystem::path("src/a.cpp"),
                            "a persisted note path must be normalized on load");

                     const BranchReviewStateQueryInput query{
                         .target = target,
                         .path = std::filesystem::path("src/a.cpp"),
                     };
                     Expect(service.FileStatus(query) == BranchReviewMarkerStatus::Reviewed,
                            "a normalized query must match a persisted un-normalized entry");
                     Expect(service.HasNote(query, BranchReviewNoteScope::File),
                            "a normalized query must find a persisted un-normalized note");
                   }});

  tests.push_back({"BranchReviewState/HighBitContentHashSurvivesRoundTrip",
                   [] {
                     // Regression: content_hash is a full-range std::hash result, so
                     // its top bit is set ~half the time. It must survive binary
                     // encode/decode; a signed >= 0 guard on decode used to reject a
                     // high-bit hash and discard the ENTIRE project-config record.
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 7);
                     compare::BranchReviewHunkIdentity identity;
                     identity.path = std::filesystem::path("hi.cpp");
                     identity.old_start = 1;
                     identity.old_count = 2;
                     identity.new_start = 3;
                     identity.new_count = 4;
                     identity.content_hash = 0x8000000000000001ULL;  // top bit set

                     BranchReviewStateService service;
                     service.MarkHunkReviewed(target, identity);

                     PersistedProjectConfigState persisted{
                         .project_base_color = std::nullopt,
                         .settings = {},
                         .sidebar_policies = {},
                         .commit_draft = std::nullopt,
                         .branch_review = ToPersistedBranchReviewState(service),
                     };
                     std::vector<std::byte> encoded;
                     Expect(EncodeProjectConfigRecord(persisted, &encoded),
                            "project config with reviewed hunk should encode");

                     PersistedProjectConfigState decoded;
                     Expect(DecodeProjectConfigRecord(encoded, &decoded),
                            "high-bit content_hash must not fail whole-record decode");
                     Expect(decoded.branch_review.targets.size() == 1,
                            "target should round-trip");
                     Expect(decoded.branch_review.targets[0].reviewed_hunks.size() == 1,
                            "reviewed hunk should round-trip");
                     Expect(decoded.branch_review.targets[0].reviewed_hunks[0].identity.content_hash ==
                                0x8000000000000001ULL,
                            "high-bit content_hash must be preserved exactly");
                   }});

  // TD-2026-07-16-34: a persisted config with MORE targets / per-target files than the
  // live service caps must be truncated to the caps at decode (a corrupt/hostile
  // .microide config cannot bypass the runtime PruneTarget limits).
  tests.push_back({"BranchReviewState/PersistedStateDecodeHonorsCaps",
                   [] {
                     using microide::workspace::PersistedBranchReviewFileEntry;
                     using microide::workspace::PersistedBranchReviewState;
                     using microide::workspace::PersistedBranchReviewTarget;

                     PersistedBranchReviewState review;
                     // 2x the target cap, each with 2x the per-target file cap.
                     const std::size_t target_over =
                         BranchReviewStateService::kMaxTargetsPerRepository * 2;
                     const std::size_t file_over =
                         BranchReviewStateService::kMaxFileEntriesPerTarget + 5;
                     for (std::size_t t = 0; t < target_over; ++t) {
                       PersistedBranchReviewTarget target;
                       target.repository_root = std::filesystem::path("/repo");
                       target.base_commit = "base" + std::to_string(t);
                       target.head_commit = "HEAD";
                       for (std::size_t f = 0; f < file_over; ++f) {
                         target.reviewed_files.push_back(PersistedBranchReviewFileEntry{
                             .path = std::filesystem::path("f" + std::to_string(f) + ".cpp")});
                       }
                       review.targets.push_back(std::move(target));
                     }

                     PersistedProjectConfigState persisted;
                     persisted.branch_review = std::move(review);
                     std::vector<std::byte> encoded;
                     Expect(EncodeProjectConfigRecord(persisted, &encoded),
                            "oversized branch-review config should still encode");

                     PersistedProjectConfigState decoded;
                     Expect(DecodeProjectConfigRecord(encoded, &decoded),
                            "oversized branch-review config decodes (truncated, not rejected)");
                     Expect(decoded.branch_review.targets.size() ==
                                BranchReviewStateService::kMaxTargetsPerRepository,
                            "targets are capped at the per-repository limit on decode");
                     for (const auto& target : decoded.branch_review.targets) {
                       Expect(target.reviewed_files.size() <=
                                  BranchReviewStateService::kMaxFileEntriesPerTarget,
                              "per-target reviewed files are capped on decode");
                     }
                   }});

  tests.push_back({"BranchReviewState/ClearTargetPreservesOtherTargets",
                   [] {
                     const BranchReviewTargetIdentity active =
                         MakeBranchReviewTargetIdentity("/repo", "base-a", "HEAD", "base-a", 1);
                     const BranchReviewTargetIdentity other =
                         MakeBranchReviewTargetIdentity("/repo", "base-b", "HEAD", "base-b", 2);

                     BranchReviewStateService service;
                     service.MarkFileReviewed(active, std::filesystem::path("a.cpp"));
                     service.MarkFileReviewed(other, std::filesystem::path("b.cpp"));
                     service.ClearTarget(active);

                     BranchReviewStateQueryInput active_query{
                         .target = active,
                         .path = std::filesystem::path("a.cpp"),
                     };
                     BranchReviewStateQueryInput other_query{
                         .target = other,
                         .path = std::filesystem::path("b.cpp"),
                     };
                     Expect(service.FileStatus(active_query) == BranchReviewMarkerStatus::Unreviewed,
                            "cleared target should remove active review markers");
                     Expect(service.FileStatus(other_query) == BranchReviewMarkerStatus::Reviewed,
                            "clearing one target should not affect another");
                   }});

  tests.push_back({"BranchReviewState/PruneDropsOldestTarget",
                   [] {
                     BranchReviewStateService service;
                     const std::filesystem::path repo = "/repo";
                     for (std::size_t i = 0; i < BranchReviewStateService::kMaxTargetsPerRepository + 2;
                          ++i) {
                       const BranchReviewTargetIdentity target = MakeBranchReviewTargetIdentity(
                           repo, "base-" + std::to_string(i), "HEAD", "merge-" + std::to_string(i), i);
                       service.MarkFileReviewed(target, std::filesystem::path("file.cpp"));
                     }
                     const BranchReviewTargetIdentity preserved = MakeBranchReviewTargetIdentity(
                         repo, "base-preserve", "HEAD", "merge-preserve", 99);
                     service.PruneForRepository(repo, &preserved);
                     Expect(service.targets().size() == BranchReviewStateService::kMaxTargetsPerRepository,
                            "prune should enforce per-repository target retention");
                     Expect(service.FindTarget(preserved) != nullptr,
                            "active target should survive pruning");
                   }});

  tests.push_back({"BranchReviewState/NoteRoundTrip",
                   [] {
                     const BranchReviewTargetIdentity target =
                         MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 4);
                     const auto model = BuildCompareModel("a\n", "b\n");
                     const auto identity =
                         ComputeBranchReviewHunkIdentity(model, 0, std::filesystem::path("note.cpp"));

                     BranchReviewStateService service;
                     service.SetNote(target, BranchReviewNoteScope::Hunk, std::filesystem::path("note.cpp"),
                                     identity, "needs follow-up");

                     BranchReviewStateQueryInput query{
                         .target = target,
                         .path = std::filesystem::path("note.cpp"),
                         .model = &model,
                         .selected_hunk_index = 0,
                     };
                     const auto note = service.NoteText(query, BranchReviewNoteScope::Hunk);
                     Expect(note.has_value() && *note == "needs follow-up",
                            "note text should be queryable for hunk scope");
                   }});
}

}  // namespace microide::tests
