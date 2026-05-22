#include "TestSupport.h"

#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"

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
}

}  // namespace microide::tests
