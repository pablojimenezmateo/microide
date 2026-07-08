#include "TestSupport.h"

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareModel.h"
#include "workspace/BranchReviewPersistence.h"
#include "workspace/BranchReviewStateBridge.h"
#include "workspace/WorkspacePersistenceFormat.h"

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
