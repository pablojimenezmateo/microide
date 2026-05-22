#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitPatchApply.h"
#include "project/GitRepository.h"
#include "project/PatchGenerator.h"
#include "project/PatchApplyTypes.h"

#include <filesystem>
#include <string>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRowKind;
using microide::project::GenerateComparePatch;
using microide::project::GenerateComparePatchForRows;
using microide::project::GitRepository;
using microide::project::PatchApplyRequest;
using microide::project::PatchApplyResultCategory;
using microide::project::PatchLineSelectionFromModelRows;
using microide::project::PatchLineSelectionHasChanges;
using microide::project::PatchOperationKind;

void TestPatchGeneratorProducesUnifiedDiff() {
  const CompareModel model = BuildCompareModel("alpha\n", "alpha\nbeta\n");
  Expect(!model.hunks.empty(), "compare model should contain a hunk");
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "hunk patch should be generated");
  Expect(patch->find("diff --git a/file.txt b/file.txt") != std::string::npos,
         "patch should include diff header");
  Expect(patch->find("@@ -") != std::string::npos, "patch should include hunk header");
  Expect(patch->find("+beta") != std::string::npos, "patch should include added line");
}

void TestPatchGeneratorSelectedLinesIncludeContext() {
  CompareModel model = BuildCompareModel("one\n", "one\ntwo\nthree\n");
  Expect(model.rows.size() >= 2, "expected multiple compare rows");
  std::size_t changed_row = 0;
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    if (model.rows[i].kind != CompareRowKind::Unchanged) {
      changed_row = i;
      break;
    }
  }
  const auto patch =
      GenerateComparePatchForRows(model, "file.txt", changed_row, changed_row);
  Expect(patch.has_value(), "selected-line patch should be generated");
  Expect(patch->find("+two") != std::string::npos || patch->find("+three") != std::string::npos,
         "selected-line patch should include changed content");
}

void TestPatchStageHunkInRepository() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "tracked.txt";
  WriteFile(file_path, "line1\n");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "line1\nline2\n");
  const CompareModel model = BuildCompareModel("line1\n", "line1\nline2\n");
  const auto patch = GenerateComparePatch(model, "tracked.txt", 0);
  Expect(patch.has_value(), "stage patch should be generated");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("tracked.txt"),
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success, "git apply should succeed");

  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"diff", "--cached", "--", "tracked.txt"});
  Expect(staged.success(), "staged diff should be readable");
  Expect(staged.output.find("+line2") != std::string::npos,
         "staged diff should contain added line");
}

void TestPatchStaleGenerationCategory() {
  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = "/nonexistent", .relative_path = "missing.txt"},
  };
  const auto result = ApplyPatchRequest(request, "not a real patch\n");
  Expect(result.category == PatchApplyResultCategory::PatchDidNotApply ||
             result.category == PatchApplyResultCategory::UnsupportedTarget,
         "invalid repository should fail safely");
}

void TestPatchLineSelectionHelpers() {
  const CompareModel model = BuildCompareModel("a\n", "a\nb\n");
  const auto selection = PatchLineSelectionFromModelRows(model, 0, model.rows.size() - 1);
  Expect(selection.has_value(), "selection helper should succeed");
  Expect(PatchLineSelectionHasChanges(model, *selection),
         "selection spanning the hunk should include changes");
}

}  // namespace

void TestPatchGeneratorUtf8Paths() {
  const CompareModel model = BuildCompareModel("café\n", "café\nnaïve\n");
  const auto patch = GenerateComparePatch(model, "café.txt", 0);
  Expect(patch.has_value(), "UTF-8 path patch should be generated");
  Expect(patch->find("naïve") != std::string::npos, "UTF-8 content should be preserved");
}

void TestPatchGeneratorIgnoresSpuriousAlignmentRows() {
  const CompareModel model = BuildCompareModel("a\n", "a\nb\n");
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "patch should be generated");
  Expect(patch->find("@@ -1,1 +1,2 @@") != std::string::npos ||
             patch->find("@@ -1,1 +2,2 @@") != std::string::npos,
         "hunk header counts should match stageable lines");
}

void RegisterPatchApplyTests(std::vector<TestCase>& tests) {
  tests.push_back({"PatchApply/UnifiedDiff", TestPatchGeneratorProducesUnifiedDiff});
  tests.push_back({"PatchApply/SelectedLines", TestPatchGeneratorSelectedLinesIncludeContext});
  tests.push_back({"PatchApply/StageHunk", TestPatchStageHunkInRepository});
  tests.push_back({"PatchApply/FailureCategory", TestPatchStaleGenerationCategory});
  tests.push_back({"PatchApply/LineSelection", TestPatchLineSelectionHelpers});
  tests.push_back({"PatchApply/Utf8Paths", TestPatchGeneratorUtf8Paths});
  tests.push_back({"PatchApply/AlignmentRows", TestPatchGeneratorIgnoresSpuriousAlignmentRows});
}

}  // namespace microide::tests
