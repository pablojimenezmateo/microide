#include "TestSupport.h"

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "workspace/CompareTabReview.h"

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
using microide::workspace::CompareCollapsedContextAction;
using microide::workspace::CompareTabState;
using microide::workspace::ExpandCompareCollapsedContext;
using microide::workspace::RefreshCompareReviewHeader;

void RegisterCompareReviewTests(std::vector<TestCase>& tests) {
  // Regression: branch-review markers/notes must actually render on the compare
  // rows. The marker-application loop gated on ComparePresentationRow::hunk_index,
  // which the presentation builder never populates (stays -1), so the whole
  // "reviewed"/note-indicator half of branch review was dead code. Markers must
  // resolve the hunk from the underlying model row instead.
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
                     // Regression: the pointer classifier required size 41, which is
                     // impossible ('-' + 40 hex + '\n' is 42), so submodule changes
                     // were never detected.
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
                         model, semantic, ComparePresentationOptions{}, ComparePresentationCollapseState{},
                         1);
                     Expect(!presentation.rows.empty(), "presentation should include rows");
                     Expect(presentation.rows.front().kind == ComparePresentationRowKind::Metadata,
                            "rename metadata should add a metadata row");
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

  tests.push_back({"CompareReview/InlineCacheTracksGeneration",
                   [] {
                     const auto model = BuildCompareModel("old\n", "new\n");
                     auto presentation = BuildComparePresentationModel(
                         model, InferCompareSemanticFileMetadata(CompareSemanticMetadataInput{
                                      .path = "f.txt",
                                      .left_content = "old\n",
                                      .right_content = "new\n",
                                      .git_entry = std::nullopt,
                                      .old_path = {},
                                  }),
                         ComparePresentationOptions{}, ComparePresentationCollapseState{}, 7);
                     Expect(presentation.inline_cache.model_generation == 7,
                            "inline cache should record model generation");
                     Expect(!presentation.inline_cache.left_spans_by_row.empty(),
                            "inline cache should retain left spans");
                   }});

  tests.push_back({"CompareReview/HeaderShowsWorkingTreeStageHints",
                   [] {
                     CompareTabState compare_tab;
                     compare_tab.review_mode = CompareReviewMode::WorkingTree;
                     compare_tab.staging_view = WorkingTreeStagingView::Combined;
                     compare_tab.right_editable = true;
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
                     Expect(compare_tab.review_header.summary_line.find("Working tree review") !=
                                std::string::npos &&
                                compare_tab.review_header.summary_line.find("combined") !=
                                    std::string::npos,
                            "working-tree header should expose the review mode and staging lens");
                     Expect(compare_tab.review_header.action_hint_line.find("a stage hunk") !=
                                std::string::npos &&
                                compare_tab.review_header.action_hint_line.find("D discard lines") !=
                                    std::string::npos,
                            "editable working-tree compares should surface stage and discard hints");
                   }});

  tests.push_back({"CompareReview/HeaderShowsUnstageHintsForStagedView",
                   [] {
                     CompareTabState compare_tab;
                     compare_tab.review_mode = CompareReviewMode::WorkingTree;
                     compare_tab.staging_view = WorkingTreeStagingView::Staged;
                     compare_tab.right_editable = true;
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
                     Expect(compare_tab.review_header.action_hint_line.find("c unstage hunk") !=
                                std::string::npos &&
                                compare_tab.review_header.action_hint_line.find("C unstage lines") !=
                                    std::string::npos,
                            "staged working-tree compares should swap stage hints for unstage hints");
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
                     Expect(compare_tab.review_header.action_hint_line.find("a stage hunk") ==
                                std::string::npos &&
                                compare_tab.review_header.action_hint_line.find("c unstage hunk") ==
                                    std::string::npos,
                            "non-editable commit compares should omit mutation hints");
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
                         model, semantic, ComparePresentationOptions{}, ComparePresentationCollapseState{},
                         1);
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
                         ComparePresentationOptions{}, ComparePresentationCollapseState{}, 1);

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
                         ComparePresentationOptions{}, ComparePresentationCollapseState{}, 1);

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
                         ComparePresentationOptions{}, ComparePresentationCollapseState{}, 1);

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
                         ComparePresentationOptions{}, ComparePresentationCollapseState{}, 1);

                     // Locate the middle collapsed run and a model row hidden inside it.
                     std::optional<std::size_t> collapsed_row;
                     std::size_t hidden_model_row = 0;
                     for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
                       const auto& row = presentation.rows[i];
                       if (row.kind == ComparePresentationRowKind::CollapsedContext &&
                           row.collapsed_run_length > 2) {
                         collapsed_row = i;
                         // A row squarely inside the hidden interior.
                         hidden_model_row =
                             row.collapsed_run_start_model_row + row.collapsed_run_length / 2;
                         break;
                       }
                     }
                     Expect(collapsed_row.has_value(),
                            "fixture should expose a middle collapsed run");

                     // A hidden model row must resolve to the collapse placeholder, never a
                     // raw (out-of-range) model index in presentation space.
                     const std::size_t resolved =
                         ComparePresentationRowForModelRow(presentation, hidden_model_row);
                     Expect(resolved == *collapsed_row,
                            "hidden model row should map to its CollapsedContext placeholder");
                     Expect(resolved < presentation.rows.size(),
                            "resolved presentation row must be in range");

                     // A visible model row must resolve to its exact presentation row and
                     // round-trip back to the same model row.
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

}  // namespace microide::tests
