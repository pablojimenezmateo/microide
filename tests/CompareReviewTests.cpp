#include "TestSupport.h"

#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "workspace/CompareTabReview.h"

#include <algorithm>

namespace microide::tests {

using microide::compare::BuildComparePresentationModel;
using microide::compare::BuildCompareModel;
using microide::compare::ComparePresentationCollapseState;
using microide::compare::ComparePresentationOptions;
using microide::compare::ComparePresentationRowKind;
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

void RegisterCompareReviewTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
