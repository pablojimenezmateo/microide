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

  tests.push_back({"BranchReviewState/HunkContentHashSeparatesLeftAndRight",
                   [] {
                     // Two hunks with identical row kind and line ranges whose per-row
                     // left+right byte concatenation is the same, but split differently:
                     // ("hello","world") vs ("hell","oworld"). The old undelimited hash
                     // collided on these; the length-prefixed hash must tell them apart.
                     auto make_model = [](std::string left, std::string right) {
                       compare::CompareModel model;
                       compare::CompareRow row;
                       row.kind = compare::CompareRowKind::Modified;
                       row.left_text = std::move(left);
                       row.right_text = std::move(right);
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
