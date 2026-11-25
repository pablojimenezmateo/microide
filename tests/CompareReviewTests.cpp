#include "TestSupport.h"
#include "support/ActionContextTestServices.h"

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/git/PatchApplyService.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/WorkspaceLayout.h"

#include <algorithm>
#include <filesystem>

namespace microide::tests {

using microide::compare::BuildComparePresentationModel;
using microide::compare::BuildCompareModel;
using microide::compare::ComparePresentationCollapseState;
using microide::compare::ComparePresentationOptions;
using microide::compare::ComparePresentationRowKind;
using microide::compare::ComparePresentationRowForModelRow;
using microide::compare::ComparePresentationToModelRow;
using microide::compare::CompareReviewMode;
using microide::compare::CompareSemanticFileKind;
using microide::compare::CompareSemanticMetadataInput;
using microide::compare::InferCompareReviewMode;
using microide::compare::InferCompareSemanticFileMetadata;
using microide::compare::InferWorkingTreeStagingView;
using microide::compare::WorkingTreeStagingView;
using microide::project::PatchOperationKind;
using microide::workspace::CompareCollapsedContextAction;
using microide::workspace::CompareTabState;
using microide::workspace::PatchApplyService;
using microide::workspace::ExpandCompareCollapsedContext;
using microide::workspace::RefreshCompareReviewHeader;

static void TestCompareReviewLabelsRoundTrip() {
  for (const CompareReviewMode mode :
       {CompareReviewMode::WorkingTree, CompareReviewMode::Commit, CompareReviewMode::Branch,
        CompareReviewMode::Conflict, CompareReviewMode::Plain}) {
    Expect(microide::compare::ParseCompareReviewMode(
               microide::compare::CompareReviewModeLabel(mode)) == mode,
           "review mode label should round-trip: " + microide::compare::CompareReviewModeLabel(mode));
  }
  for (const WorkingTreeStagingView view :
       {WorkingTreeStagingView::Combined, WorkingTreeStagingView::Unstaged,
        WorkingTreeStagingView::Staged}) {
    Expect(microide::compare::ParseWorkingTreeStagingView(
               microide::compare::WorkingTreeStagingViewLabel(view)) == view,
           "staging view label should round-trip: " +
               microide::compare::WorkingTreeStagingViewLabel(view));
  }
  Expect(microide::compare::ParseCompareReviewMode("garbage") == CompareReviewMode::WorkingTree &&
             microide::compare::ParseWorkingTreeStagingView("garbage") ==
                 WorkingTreeStagingView::Combined,
         "unknown labels read as the defaults");
}

void RegisterCompareReviewTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CompareReview/LabelsRoundTrip", TestCompareReviewLabelsRoundTrip);
  tests.push_back(
      {"CompareReview/UnmarkCommandsReachTheReviewService",
       [] {
         using microide::workspace::ActionCoordinator;
         using microide::workspace::ActionId;
         using microide::workspace::ActionSource;
         using microide::workspace::ProjectCatalogState;
         using microide::workspace::ProjectWorkspaceState;
         using microide::workspace::WorkspaceActionContext;

         ProjectCatalogState catalog;
         ProjectWorkspaceState project_state;
         float ui_scale = 1.0f;

         WorkspaceActionContext::Operations operations;
         microide::workspace::CompareTabState branch_tab;
         branch_tab.review_mode = microide::compare::CompareReviewMode::Branch;
         branch_tab.path = "f.txt";
         branch_tab.model = BuildCompareModel("a\n", "b\n");
         microide::workspace::CompareTabState* active_tab = nullptr;
         operations.active_compare_tab = [&]() { return active_tab; };
         std::string feedback;
         operations.reject_action = [&](ActionSource, std::string text) {
           feedback = std::move(text);
           return false;
         };

         microide::tests::ActionContextTestServices services(catalog, project_state, ui_scale);
         services.review_active_tab = [&]() { return active_tab; };
         WorkspaceActionContext context(catalog, project_state, ui_scale, services.Services(),
                                        std::move(operations));
         ActionCoordinator coordinator(context);

         const std::vector<std::string> no_args;
         Expect(!coordinator.Execute(ActionId::MarkBranchFileReviewed, no_args, ActionSource::Command) &&
                    services.review_persists == 0 && !feedback.empty(),
                "with no branch-review diff open the mark is refused with a reason, not run");
         active_tab = &branch_tab;
         Expect(coordinator.Execute(ActionId::MarkBranchFileReviewed, no_args,
                                    ActionSource::Command) &&
                    services.review_persists == 1,
                "mark-branch-file-reviewed should reach the review service");
         Expect(coordinator.Execute(ActionId::UnmarkBranchFileReviewed, no_args,
                                    ActionSource::Command) &&
                    services.review_persists == 2,
                "unmark-branch-file-reviewed should reach the review service");
         Expect(coordinator.Execute(ActionId::MarkBranchHunkReviewed, no_args,
                                    ActionSource::Command) &&
                    services.review_persists == 3,
                "mark-branch-hunk-reviewed should reach the review service");
         Expect(coordinator.Execute(ActionId::UnmarkBranchHunkReviewed, no_args,
                                    ActionSource::Command) &&
                    services.review_persists == 4,
                "unmark-branch-hunk-reviewed should reach the review service");
       }});

  tests.push_back(
      {"CompareReview/UnmarkClearsOnlyTheTargetedFile",
       [] {
         using microide::compare::BranchReviewMarkerStatus;
         using microide::compare::BranchReviewStateQueryInput;
         using microide::compare::BranchReviewStateService;
         using microide::compare::MakeBranchReviewTargetIdentity;

         BranchReviewStateService service;
         const auto target = MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
         const std::filesystem::path kept("kept.cpp");
         const std::filesystem::path undone("undone.cpp");

         service.MarkFileReviewed(target, kept);
         service.MarkFileReviewed(target, undone);
         const auto status = [&](const std::filesystem::path& path) {
           BranchReviewStateQueryInput input;
           input.target = target;
           input.path = path;
           return service.FileStatus(input);
         };
         Expect(status(kept) == BranchReviewMarkerStatus::Reviewed &&
                    status(undone) == BranchReviewMarkerStatus::Reviewed,
                "both files should start reviewed");

         const std::uint64_t before = service.revision();
         service.MarkFileUnreviewed(target, undone);
         Expect(service.revision() != before, "a real removal must bump the revision");
         Expect(status(undone) != BranchReviewMarkerStatus::Reviewed,
                "unmarking must clear the targeted file's reviewed status");
         Expect(status(kept) == BranchReviewMarkerStatus::Reviewed,
                "unmarking one file must not clear another file's review state");
       }});

  tests.push_back(
      {"CompareReview/BranchMarkersRenderFromModelRowHunk",
       [] {
         using microide::compare::BranchReviewMarkerStatus;
         using microide::compare::BranchReviewNoteScope;
         using microide::compare::BranchReviewStateService;
         using microide::compare::ComputeBranchReviewHunkIdentity;
         using microide::compare::MakeBranchReviewTargetIdentity;
         using microide::workspace::ApplyBranchReviewPresentationMarkers;
         using microide::workspace::RefreshCompareTabPresentation;

         CompareTabState compare_tab;
         compare_tab.review_mode = CompareReviewMode::Branch;
         compare_tab.path = std::filesystem::path("a.cpp");
         compare_tab.branch_target =
             MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
         compare_tab.model = BuildCompareModel("old line\n", "new line\n");
         compare_tab.semantic_file = InferCompareSemanticFileMetadata(
             CompareSemanticMetadataInput{
                 .path = "a.cpp",
                 .left_content = "old line\n",
                 .right_content = "new line\n",
                 .git_entry = std::nullopt,
                 .old_path = {},
             });
         RefreshCompareTabPresentation(compare_tab);

         BranchReviewStateService service;
         service.MarkHunkReviewed(
             compare_tab.branch_target,
             ComputeBranchReviewHunkIdentity(compare_tab.model, 0,
                                             std::filesystem::path("a.cpp")));

         ApplyBranchReviewPresentationMarkers(compare_tab, service);

         bool any_marked = false;
         for (const auto& row : compare_tab.presentation.rows) {
           if (row.kind == ComparePresentationRowKind::Model &&
               !row.review_marker_label.empty()) {
             any_marked = true;
           }
         }
         Expect(any_marked,
                "a reviewed hunk must place a review marker on its model rows");
       }});

  tests.push_back(
      {"CompareReview/BranchMarkerCacheInvalidatesOnEveryInput",
       [] {
         using microide::compare::BranchReviewNoteScope;
         using microide::compare::BranchReviewStateService;
         using microide::compare::ComputeBranchReviewHunkIdentity;
         using microide::compare::MakeBranchReviewTargetIdentity;
         using microide::workspace::ApplyBranchReviewPresentationMarkers;
         using microide::workspace::RefreshCompareTabPresentation;

         const std::filesystem::path path("a.cpp");
         CompareTabState compare_tab;
         compare_tab.review_mode = CompareReviewMode::Branch;
         compare_tab.path = path;
         compare_tab.branch_target =
             MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
         compare_tab.model = BuildCompareModel("old line\n", "new line\n");
         RefreshCompareTabPresentation(compare_tab);

         const auto marked_rows = [&]() {
           std::size_t count = 0;
           for (const auto& row : compare_tab.presentation.rows) {
             if (row.kind == ComparePresentationRowKind::Model &&
                 !row.review_marker_label.empty()) {
               ++count;
             }
           }
           return count;
         };
         const auto noted_rows = [&]() {
           std::size_t count = 0;
           for (const auto& row : compare_tab.presentation.rows) {
             if (row.has_review_note) {
               ++count;
             }
           }
           return count;
         };

         BranchReviewStateService service;
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == 0, "no review state means no markers");

         service.MarkHunkReviewed(compare_tab.branch_target,
                                  ComputeBranchReviewHunkIdentity(compare_tab.model, 0, path));
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         const std::size_t reviewed_rows = marked_rows();
         Expect(reviewed_rows > 0, "marking a hunk reviewed must re-resolve the markers");

         service.MarkHunkUnreviewed(compare_tab.branch_target,
                                    ComputeBranchReviewHunkIdentity(compare_tab.model, 0, path));
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == 0, "unmarking must clear the markers, not serve a stale skip");

         service.SetNote(compare_tab.branch_target, BranchReviewNoteScope::Hunk, path,
                         ComputeBranchReviewHunkIdentity(compare_tab.model, 0, path), "look");
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(noted_rows() > 0, "adding a hunk note must re-resolve the markers");

         service.MarkHunkReviewed(compare_tab.branch_target,
                                  ComputeBranchReviewHunkIdentity(compare_tab.model, 0, path));
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == reviewed_rows, "the reviewed markers should be back");
         RefreshCompareTabPresentation(compare_tab);
         Expect(marked_rows() == 0, "a presentation rebuild clears the row markers");
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == reviewed_rows,
                "a presentation rebuild must invalidate the marker cache");

         compare_tab.branch_target =
             MakeBranchReviewTargetIdentity("/repo", "other-base", "HEAD", "other-base", 3);
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == 0,
                "switching the branch target must invalidate the marker cache");

         compare_tab.branch_target =
             MakeBranchReviewTargetIdentity("/repo", "base", "HEAD", "base", 3);
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == reviewed_rows, "switching back must re-resolve");
         for (auto& row : compare_tab.presentation.rows) {
           row.review_marker_label.clear();
         }
         ApplyBranchReviewPresentationMarkers(compare_tab, service);
         Expect(marked_rows() == 0,
                "with no input moved the pass must skip, leaving the rows untouched");
       }});

  tests.push_back({"CompareReview/WorkingTreeMode",
                   [] {
                     Expect(InferCompareReviewMode("HEAD", "WORKTREE", false) ==
                                CompareReviewMode::WorkingTree,
                            "HEAD vs worktree should be working-tree review");
                     Expect(InferWorkingTreeStagingView("HEAD", "WORKTREE") ==
                                WorkingTreeStagingView::Combined,
                            "default working-tree staging view is combined");
                   }});

  tests.push_back({"CompareReview/CommitMode",
                   [] {
                     Expect(InferCompareReviewMode("abc123", "WORKTREE", true) ==
                                CompareReviewMode::Commit,
                            "commit picker worktree compare should be commit review");
                   }});

  tests.push_back({"CompareReview/BranchMode",
                   [] {
                     Expect(InferCompareReviewMode("origin/main", "HEAD", false) ==
                                CompareReviewMode::Branch,
                            "base ref vs HEAD should be branch review");
                   }});

  tests.push_back({"CompareReview/ConflictMode",
                   [] {
                     Expect(InferCompareReviewMode(":2", ":3", false) == CompareReviewMode::Conflict,
                            "stage refs should be conflict review");
                   }});

  tests.push_back({"CompareReview/SemanticBinaryMetadata",
                   [] {
                     const auto metadata = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "bin.dat",
                         .left_content = std::string(32, '\0'),
                         .right_content = std::string(32, '\1'),
                         .git_entry = std::nullopt,
                         .old_path = {},
                     });
                     Expect(metadata.file_kind == CompareSemanticFileKind::Binary,
                            "NUL bytes should classify as binary");
                   }});

  tests.push_back({"CompareReview/SemanticSubmodulePointerMetadata",
                   [] {
                     const std::string left = "-" + std::string(40, 'a') + "\n";
                     const std::string right = "-" + std::string(40, 'b') + "\n";
                     const auto metadata = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "vendor/lib",
                         .left_content = left,
                         .right_content = right,
                         .git_entry = std::nullopt,
                         .old_path = {},
                     });
                     Expect(metadata.file_kind == CompareSemanticFileKind::Submodule,
                            "a '-'+40hex+newline pointer pair should classify as a submodule");
                     Expect(metadata.submodule_pointer_changed,
                            "differing submodule OIDs should mark the pointer as changed");
                     Expect(metadata.old_submodule_oid == std::string(40, 'a') &&
                                metadata.new_submodule_oid == std::string(40, 'b'),
                            "submodule OIDs should be extracted from the pointer body");
                   }});

  tests.push_back({"CompareReview/SemanticRenameAndModeMetadata",
                   [] {
                     const auto metadata = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "new.txt",
                         .left_content = "a\n",
                         .right_content = "a\n",
                         .git_entry = std::nullopt,
                         .old_path = "old.txt",
                         .old_executable = false,
                         .new_executable = true,
                     });
                     Expect(metadata.renamed, "explicit old path should mark rename");
                     Expect(metadata.mode_changed, "executable delta should mark mode change");
                   }});

  tests.push_back(
      {"CompareReview/SemanticLineEndingOnlyMetadata",
       [] {
         const auto classify = [](std::string_view left, std::string_view right) {
           return InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                      .path = "f.txt",
                      .left_content = left,
                      .right_content = right,
                      .git_entry = std::nullopt,
                      .old_path = {},
                  })
               .line_ending_only;
         };

         Expect(classify("a\r\nb\r\n", "a\nb\n"),
                "CRLF vs LF over identical text is a line-ending-only change");
         Expect(classify("a\rb\r", "a\nb\n"),
                "lone CR vs LF over identical text is a line-ending-only change");
         Expect(classify("a\r\nb\n", "a\nb\r\n"),
                "mixed endings that normalize equal are a line-ending-only change");
         Expect(!classify("a\nb\n", "a\nb\n"),
                "identical content is not a change at all");
         Expect(!classify("a\r\nb\r\n", "a\nc\n"),
                "differing text is not a line-ending-only change even across endings");
         Expect(!classify("a\r\nb\r\n", "a\nb\nc\n"),
                "a trailing extra line is not a line-ending-only change");
         Expect(!classify("a\r\nb\r\n", "a\nb"),
                "a missing trailing newline is not a line-ending-only change");
         Expect(!classify("", "a\n"), "content appearing is not a line-ending-only change");
         Expect(classify("a\r", "a\n"), "a trailing lone CR normalizes to a trailing LF");
       }});

  tests.push_back({"CompareReview/PresentationReviewNoteRows",
                   [] {
                     std::string left;
                     std::string right;
                     for (int line = 1; line <= 30; ++line) {
                       left += "line " + std::to_string(line) + "\n";
                       right += (line == 3 || line == 25 ? "changed " : "line ") + std::to_string(line) + "\n";
                     }
                     const auto model = BuildCompareModel(left, right);
                     Expect(model.hunks.size() == 2, "two changes");
                     const std::vector<microide::compare::BranchReviewHunkMarker> notes = {
                         {}, {.has_note = true, .note_text = "second change"}};
                     const auto presentation = BuildComparePresentationModel(
                         model, microide::compare::CompareSemanticFileMetadata{},
                         ComparePresentationOptions{.hunk_markers = &notes, .file_note = "whole file"},
                         ComparePresentationCollapseState{});
                     std::size_t note_rows = 0;
                     for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
                       const auto& row = presentation.rows[i];
                       if (row.kind != ComparePresentationRowKind::ReviewNote) {
                         continue;
                       }
                       ++note_rows;
                       if (row.hunk_index < 0) {
                         Expect(i == 0 && row.summary_text == "whole file", "the file's comment leads");
                         continue;
                       }
                       Expect(row.hunk_index == 1 && row.summary_text == "second change", "only the noted change");
                       const auto& above = presentation.rows[i - 1];
                       Expect(above.kind == ComparePresentationRowKind::Model &&
                                  above.model_row_index == static_cast<std::size_t>(model.hunks[1].end_row),
                              "right under the change's last row");
                     }
                     Expect(note_rows == 2, "one row per comment");
                   }});

  tests.push_back({"CompareReview/PresentationMetadataRow",
                   [] {
                     const auto model = BuildCompareModel("a\n", "b\n");
                     const auto semantic = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "f.txt",
                         .left_content = "a\n",
                         .right_content = "b\n",
                         .git_entry = std::nullopt,
                         .old_path = "g.txt",
                     });
                     const auto presentation = BuildComparePresentationModel(
                         model, semantic, ComparePresentationOptions{}, ComparePresentationCollapseState{});
                     Expect(!presentation.rows.empty(), "presentation should include rows");
                     Expect(presentation.rows.front().kind == ComparePresentationRowKind::Metadata,
                            "rename metadata should add a metadata row");
                   }});

  tests.push_back({"CompareReview/OverviewRunsSeparateMovedLines",
                   [] {
                     std::string middle;
                     for (int line = 0; line < 12; ++line) {
                       middle += "keep_" + std::to_string(line) + "();\n";
                     }
                     const std::string helper =
                         "void helper() {\n  do_work(alpha, beta);\n  finish(gamma);\n}\n";
                     const auto model = BuildCompareModel(helper + middle + "tail_old();\n",
                                                          middle + helper + "tail_new();\n");
                     const auto semantic = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "f.c", .left_content = "", .right_content = "", .git_entry = std::nullopt,
                         .old_path = {}});
                     const auto presentation = BuildComparePresentationModel(
                         model, semantic, ComparePresentationOptions{}, ComparePresentationCollapseState{});
                     std::vector<microide::workspace::CompareScrollbarRun> runs;
                     microide::workspace::BuildCompareScrollbarRunsInto(presentation, model, runs);
                     int moved_runs = 0;
                     int plain_runs = 0;
                     for (const auto& run : runs) {
                       (run.moved ? moved_runs : plain_runs) += 1;
                     }
                     Expect(moved_runs == 2, "the moved function is one run where it left and one where it "
                                             "landed (" + std::to_string(moved_runs) + ")");
                     Expect(plain_runs >= 1, "and the edited tail line is an ordinary run");
                   }});

  tests.push_back({"CompareReview/IgnoreWhitespaceRebuildsModel",
                   [] {
                     const std::string left = "alpha  \nbeta\n";
                     const std::string right = "alpha\nbeta\n";
                     const auto strict = microide::compare::BuildCompareModel(left, right);
                     const auto ignored = microide::compare::BuildCompareModel(
                         left, right, microide::compare::CompareBuildOptions{.ignore_whitespace = true});
                     Expect(strict.hunks.size() >= 1, "strict diff should keep whitespace hunks");
                     Expect(ignored.hunks.empty(),
                            "ignore-whitespace diff should treat trailing whitespace as equal");
                   }});

  const auto working_tree_tab = [](WorkingTreeStagingView staging_view) {
    CompareTabState compare_tab;
    compare_tab.review_mode = CompareReviewMode::WorkingTree;
    compare_tab.right_ref = "WORKTREE";
    compare_tab.staging_view = staging_view;
    compare_tab.right_editable = true;
    compare_tab.model = BuildCompareModel("old\n", "new\n");
    compare_tab.semantic_file = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
        .path = "f.txt",
        .left_content = "old\n",
        .right_content = "new\n",
        .git_entry = std::nullopt,
        .old_path = {},
    });
    return compare_tab;
  };

  tests.push_back({"CompareReview/HeaderShowsWorkingTreeStagingLens",
                   [working_tree_tab] {
                     CompareTabState compare_tab =
                         working_tree_tab(WorkingTreeStagingView::Combined);
                     RefreshCompareReviewHeader(compare_tab);
                     Expect(compare_tab.review_header.summary_line.find("Working tree review") !=
                                std::string::npos &&
                                compare_tab.review_header.summary_line.find("combined") !=
                                    std::string::npos,
                            "working-tree header should expose the review mode and staging lens");
                   }});

  tests.push_back({"CompareReview/CombinedWorkingTreeOffersStageAndDiscard",
                   [working_tree_tab] {
                     const CompareTabState compare_tab =
                         working_tree_tab(WorkingTreeStagingView::Combined);
                     Expect(PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::StageHunk) &&
                                PatchApplyService::CanApplyPatchToCompareTab(
                                    compare_tab, PatchOperationKind::DiscardSelectedLines),
                            "editable working-tree compares should offer stage and discard");
                   }});

  tests.push_back({"CompareReview/StagedViewSwapsStageForUnstage",
                   [working_tree_tab] {
                     const CompareTabState compare_tab =
                         working_tree_tab(WorkingTreeStagingView::Staged);
                     Expect(PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::UnstageHunk) &&
                                PatchApplyService::CanApplyPatchToCompareTab(
                                    compare_tab, PatchOperationKind::UnstageSelectedLines),
                            "the staged lens should offer the unstage verbs");
                     Expect(!PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::StageHunk),
                            "the staged lens should refuse a stage: there is nothing left to stage");
                   }});

  tests.push_back({"CompareReview/UnstagedViewRefusesUnstage",
                   [working_tree_tab] {
                     const CompareTabState compare_tab =
                         working_tree_tab(WorkingTreeStagingView::Unstaged);
                     Expect(PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::StageHunk),
                            "the unstaged lens should offer a stage");
                     Expect(!PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::UnstageHunk),
                            "the unstaged lens should refuse an unstage");
                   }});

  tests.push_back({"CompareReview/HeaderHidesMutationHintsForCommitReview",
                   [] {
                     CompareTabState compare_tab;
                     compare_tab.review_mode = CompareReviewMode::Commit;
                     compare_tab.right_editable = false;
                     compare_tab.model = BuildCompareModel("old\n", "new\n");
                     compare_tab.semantic_file = InferCompareSemanticFileMetadata(
                         CompareSemanticMetadataInput{
                             .path = "f.txt",
                             .left_content = "old\n",
                             .right_content = "new\n",
                             .git_entry = std::nullopt,
                             .old_path = {},
                         });

                     RefreshCompareReviewHeader(compare_tab);
                     Expect(compare_tab.review_header.summary_line.find("Commit review") !=
                                std::string::npos,
                            "commit compares should identify the review mode in the header");
                     Expect(!PatchApplyService::CanApplyPatchToCompareTab(
                                compare_tab, PatchOperationKind::StageHunk) &&
                                !PatchApplyService::CanApplyPatchToCompareTab(
                                    compare_tab, PatchOperationKind::UnstageHunk) &&
                                !PatchApplyService::CanApplyPatchToCompareTab(
                                    compare_tab, PatchOperationKind::DiscardHunk),
                            "a commit review is not a working-tree patch target: every mutation "
                            "verb must refuse, not just go unmentioned");
                   }});

  tests.push_back({"CompareReview/PresentationMapsModelRow",
                   [] {
                     const auto model = BuildCompareModel("unchanged\n\nleft\n", "unchanged\n\nright\n");
                     const auto semantic = InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                         .path = "f.txt",
                         .left_content = "unchanged\n\nleft\n",
                         .right_content = "unchanged\n\nright\n",
                         .git_entry = std::nullopt,
                         .old_path = {},
                     });
                     auto presentation = BuildComparePresentationModel(
                         model, semantic, ComparePresentationOptions{}, ComparePresentationCollapseState{});
                     std::optional<std::size_t> changed_presentation;
                     for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
                       if (presentation.rows[i].kind == ComparePresentationRowKind::Model &&
                           model.rows[presentation.rows[i].model_row_index].kind !=
                               microide::compare::CompareRowKind::Unchanged) {
                         changed_presentation = i;
                         break;
                       }
                     }
                     Expect(changed_presentation.has_value(), "fixture should include a changed row");
                     const std::size_t mapped =
                         ComparePresentationToModelRow(presentation, *changed_presentation);
                     Expect(model.rows[mapped].kind != microide::compare::CompareRowKind::Unchanged,
                            "presentation row should map to a changed model row");
                   }});

  tests.push_back({"CompareReview/CollapsedContextExpansionRevealsMoreRows",
                   [] {
                     const auto build_text = [](std::string_view changed_a,
                                                std::string_view changed_b) {
                       std::string text;
                       for (int i = 0; i < 24; ++i) {
                         text += "prefix " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_a) + "\n";
                       for (int i = 0; i < 500; ++i) {
                         text += "middle " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_b) + "\n";
                       for (int i = 0; i < 8; ++i) {
                         text += "suffix " + std::to_string(i) + "\n";
                       }
                       return text;
                     };

                     CompareTabState compare_tab;
                     compare_tab.model = BuildCompareModel(build_text("left a", "left b"),
                                                           build_text("right a", "right b"));
                     compare_tab.presentation = BuildComparePresentationModel(
                         compare_tab.model,
                         InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                             .path = "f.txt",
                             .left_content = build_text("left a", "left b"),
                             .right_content = build_text("right a", "right b"),
                             .git_entry = std::nullopt,
                             .old_path = {},
                         }),
                         ComparePresentationOptions{}, ComparePresentationCollapseState{});

                     std::optional<std::size_t> collapsed_row;
                     std::size_t collapsed_run_start = 0;
                     std::size_t collapsed_run_length = 0;
                     for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
                       const auto& row = compare_tab.presentation.rows[i];
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.previous_hunk_index >= 0 && row.next_hunk_index >= 0) {
                         collapsed_row = i;
                         collapsed_run_start = row.collapsed_run_start_model_row;
                         collapsed_run_length = row.collapsed_run_length;
                         break;
                       }
                     }
                     Expect(collapsed_row.has_value(),
                            "fixture should expose a middle collapsed context row");

                     const std::size_t initial_row_count = compare_tab.presentation.rows.size();
                     const int initial_hidden_lines =
                         compare_tab.presentation.rows[*collapsed_row].collapsed_line_count;
                     Expect(ExpandCompareCollapsedContext(compare_tab, *collapsed_row,
                                                         CompareCollapsedContextAction::ShowPrevious),
                            "ShowPrevious should expand a collapsed context row");
                     const std::size_t after_previous_count =
                         compare_tab.presentation.rows.size();
                     Expect(after_previous_count > initial_row_count,
                            "ShowPrevious should reveal more presentation rows");

                     std::optional<std::size_t> updated_collapsed_row;
                     for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
                       const auto& row = compare_tab.presentation.rows[i];
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.collapsed_run_start_model_row == collapsed_run_start &&
                           row.collapsed_run_length == collapsed_run_length) {
                         updated_collapsed_row = i;
                         break;
                       }
                     }
                     Expect(updated_collapsed_row.has_value(),
                            "collapsed row should remain after a partial expansion");
                     Expect(compare_tab.presentation.rows[*updated_collapsed_row].collapsed_line_count ==
                                initial_hidden_lines - 20,
                            "ShowPrevious should reduce the hidden-line count by exactly 20");
                     Expect(ExpandCompareCollapsedContext(compare_tab, *updated_collapsed_row,
                                                         CompareCollapsedContextAction::ShowAll),
                            "ShowAll should fully expand the remaining hidden lines");
                     Expect(compare_tab.presentation.rows.size() > after_previous_count,
                            "ShowAll should reveal the rest of the hidden rows");
                   }});

  tests.push_back({"CompareReview/CollapsedContextExpansionAboveKeepsViewportAnchor",
                   [] {
                     const auto build_text = [](std::string_view changed_a,
                                                std::string_view changed_b) {
                       std::string text;
                       for (int i = 0; i < 24; ++i) {
                         text += "prefix " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_a) + "\n";
                       for (int i = 0; i < 500; ++i) {
                         text += "middle " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_b) + "\n";
                       for (int i = 0; i < 8; ++i) {
                         text += "suffix " + std::to_string(i) + "\n";
                       }
                       return text;
                     };

                     CompareTabState compare_tab;
                     compare_tab.model = BuildCompareModel(build_text("left a", "left b"),
                                                           build_text("right a", "right b"));
                     compare_tab.presentation = BuildComparePresentationModel(
                         compare_tab.model,
                         InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                             .path = "f.txt",
                             .left_content = build_text("left a", "left b"),
                             .right_content = build_text("right a", "right b"),
                             .git_entry = std::nullopt,
                             .old_path = {},
                         }),
                         ComparePresentationOptions{}, ComparePresentationCollapseState{});

                     std::optional<std::size_t> collapsed_row;
                     std::size_t collapsed_run_start = 0;
                     std::size_t collapsed_run_length = 0;
                     for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
                       const auto& row = compare_tab.presentation.rows[i];
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.previous_hunk_index >= 0 && row.next_hunk_index >= 0) {
                         collapsed_row = i;
                         collapsed_run_start = row.collapsed_run_start_model_row;
                         collapsed_run_length = row.collapsed_run_length;
                         break;
                       }
                     }
                     Expect(collapsed_row.has_value(),
                            "fixture should expose a middle collapsed context row");

                     compare_tab.selected_row = *collapsed_row;
                     compare_tab.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
                     const int before_scroll = compare_tab.scroll_row;

                     Expect(ExpandCompareCollapsedContext(compare_tab, *collapsed_row,
                                                         CompareCollapsedContextAction::ShowPrevious),
                            "ShowPrevious should expand a collapsed context row");
                     Expect(compare_tab.scroll_row > before_scroll,
                            "expanding previous context should advance the scroll anchor");
                     Expect(compare_tab.selected_row > *collapsed_row,
                            "expanding previous context should keep selection with the collapsed block");
                     Expect(compare_tab.selected_row < compare_tab.presentation.rows.size(),
                            "expanded compare selection should stay in range");
                     Expect(compare_tab.presentation.rows[compare_tab.selected_row].kind ==
                                ComparePresentationRowKind::CollapsedContext &&
                                compare_tab.presentation.rows[compare_tab.selected_row]
                                        .collapsed_run_start_model_row == collapsed_run_start &&
                                compare_tab.presentation.rows[compare_tab.selected_row]
                                        .collapsed_run_length == collapsed_run_length,
                            "expanding previous context should keep the same collapsed block selected");
                   }});

  tests.push_back({"CompareReview/CollapsedContextExpansionPersistsAcrossRuns",
                   [] {
                     const auto build_text = [](std::string_view changed_a,
                                                std::string_view changed_b,
                                                std::string_view changed_c) {
                       std::string text;
                       for (int i = 0; i < 24; ++i) {
                         text += "prefix " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_a) + "\n";
                       for (int i = 0; i < 30; ++i) {
                         text += "middle-a " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_b) + "\n";
                       for (int i = 0; i < 30; ++i) {
                         text += "middle-b " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_c) + "\n";
                       for (int i = 0; i < 8; ++i) {
                         text += "suffix " + std::to_string(i) + "\n";
                       }
                       return text;
                     };

                     const std::string left = build_text("left a", "left b", "left c");
                     const std::string right = build_text("right a", "right b", "right c");

                     CompareTabState compare_tab;
                     compare_tab.model = BuildCompareModel(left, right);
                     compare_tab.presentation = BuildComparePresentationModel(
                         compare_tab.model,
                         InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                             .path = "f.txt",
                             .left_content = left,
                             .right_content = right,
                             .git_entry = std::nullopt,
                             .old_path = {},
                         }),
                         ComparePresentationOptions{}, ComparePresentationCollapseState{});

                     std::vector<std::pair<std::size_t, std::size_t>> middle_run_identities;
                     for (const auto& row : compare_tab.presentation.rows) {
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.previous_hunk_index >= 0 && row.next_hunk_index >= 0) {
                         middle_run_identities.push_back(
                             {row.collapsed_run_start_model_row, row.collapsed_run_length});
                       }
                     }
                     Expect(middle_run_identities.size() >= 2,
                            "fixture should expose multiple independently collapsed middle runs");

                     const auto find_run_row = [&](std::pair<std::size_t, std::size_t> identity) {
                       for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
                         const auto& row = compare_tab.presentation.rows[i];
                         if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                             row.collapsed_run_start_model_row == identity.first &&
                             row.collapsed_run_length == identity.second) {
                           return std::optional<std::size_t>(i);
                         }
                       }
                       return std::optional<std::size_t>{};
                     };

                     const std::optional<std::size_t> first_row =
                         find_run_row(middle_run_identities[0]);
                     Expect(first_row.has_value(),
                            "first collapsed middle run should be present before expansion");
                     Expect(ExpandCompareCollapsedContext(compare_tab, *first_row,
                                                         CompareCollapsedContextAction::ShowAll),
                            "ShowAll should fully expand the first collapsed middle run");
                     Expect(!find_run_row(middle_run_identities[0]).has_value(),
                            "fully expanding the first run should remove its collapsed row");

                     const std::optional<std::size_t> second_row =
                         find_run_row(middle_run_identities[1]);
                     Expect(second_row.has_value(),
                            "second collapsed middle run should remain available after the first expansion");
                     Expect(ExpandCompareCollapsedContext(compare_tab, *second_row,
                                                         CompareCollapsedContextAction::ShowAll),
                            "ShowAll should fully expand the second collapsed middle run");
                     Expect(!find_run_row(middle_run_identities[0]).has_value(),
                            "expanding a second run should not re-collapse the first");
                     Expect(!find_run_row(middle_run_identities[1]).has_value(),
                            "fully expanding the second run should also remove its collapsed row");
                   }});

  tests.push_back({"CompareReview/PresentationRowForModelRowResolvesCollapsedRuns",
                   [] {
                     const auto build_text = [](std::string_view changed_a,
                                                std::string_view changed_b) {
                       std::string text;
                       for (int i = 0; i < 24; ++i) {
                         text += "prefix " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_a) + "\n";
                       for (int i = 0; i < 500; ++i) {
                         text += "middle " + std::to_string(i) + "\n";
                       }
                       text += std::string(changed_b) + "\n";
                       for (int i = 0; i < 8; ++i) {
                         text += "suffix " + std::to_string(i) + "\n";
                       }
                       return text;
                     };

                     const auto model = BuildCompareModel(build_text("left a", "left b"),
                                                          build_text("right a", "right b"));
                     const auto presentation = BuildComparePresentationModel(
                         model,
                         InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                             .path = "f.txt",
                             .left_content = build_text("left a", "left b"),
                             .right_content = build_text("right a", "right b"),
                             .git_entry = std::nullopt,
                             .old_path = {},
                         }),
                         ComparePresentationOptions{}, ComparePresentationCollapseState{});

                     std::optional<std::size_t> collapsed_row;
                     std::size_t hidden_model_row = 0;
                     for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
                       const auto& row = presentation.rows[i];
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.collapsed_run_length > 2) {
                         collapsed_row = i;
                         hidden_model_row =
                             row.collapsed_run_start_model_row + row.collapsed_run_length / 2;
                         break;
                       }
                     }
                     Expect(collapsed_row.has_value(),
                            "fixture should expose a middle collapsed run");

                     const std::size_t resolved =
                         ComparePresentationRowForModelRow(presentation, hidden_model_row);
                     Expect(resolved == *collapsed_row,
                            "hidden model row should map to its CollapsedContext placeholder");
                     Expect(resolved < presentation.rows.size(),
                            "resolved presentation row must be in range");

                     std::optional<std::size_t> visible_presentation_row;
                     for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
                       if (presentation.rows[i].kind == ComparePresentationRowKind::Model) {
                         visible_presentation_row = i;
                         break;
                       }
                     }
                     Expect(visible_presentation_row.has_value(),
                            "presentation should contain visible model rows");
                     const std::size_t visible_model_row =
                         presentation.rows[*visible_presentation_row].model_row_index;
                     Expect(ComparePresentationRowForModelRow(presentation, visible_model_row) ==
                                *visible_presentation_row,
                            "a visible model row should map to its exact presentation row");
                   }});
}

}
