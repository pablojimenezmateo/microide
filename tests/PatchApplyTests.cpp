#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitPatchApply.h"
#include "project/GitRepository.h"
#include "project/PatchGenerator.h"
#include "project/PatchApplyTypes.h"

#include <filesystem>
#include <regex>
#include <string>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRow;
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
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
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
      .target{
          .repository_root = "/nonexistent",
          .relative_path = "missing.txt",
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
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

CompareModel MakePatchModel(std::initializer_list<CompareRow> rows, int hunk_start, int hunk_end) {
  CompareModel model;
  model.rows.assign(rows);
  model.hunks.push_back({.index = 0, .start_row = hunk_start, .end_row = hunk_end});
  return model;
}

bool PatchHunkCountsMatchBody(const std::string& patch) {
  static const std::regex header_pattern(
      R"(@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");
  std::smatch match;
  if (!std::regex_search(patch, match, header_pattern)) {
    return false;
  }
  const int old_count = match[2].matched ? std::stoi(match[2].str()) : 1;
  const int new_count = match[4].matched ? std::stoi(match[4].str()) : 1;
  const auto header_pos = patch.find(match[0]);
  const auto body = patch.substr(header_pos + match[0].length());
  int counted_old = 0;
  int counted_new = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '\n' && (i == 0 || body[i - 1] == '\n')) {
      if (body[i] == ' ' || body[i] == '-') {
        ++counted_old;
      }
      if (body[i] == ' ' || body[i] == '+') {
        ++counted_new;
      }
    }
  }
  return counted_old == old_count && counted_new == new_count;
}

void TestPatchGeneratorPreservesBlankContextLines() {
  const CompareModel model = MakePatchModel(
      {
          {.left_text = "before", .right_text = "before", .left_line = 1, .right_line = 1,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "", .right_text = "", .left_line = 2, .right_line = 2,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "old", .right_text = "new", .left_line = 3, .right_line = 3,
           .kind = CompareRowKind::Modified, .left_changed_spans = {}, .right_changed_spans = {}},
      },
      1, 2);
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "patch with blank context should be generated");
  Expect(patch->find("\n \n") != std::string::npos || patch->find("\n+ \n") != std::string::npos ||
             patch->find(" \n-") != std::string::npos,
         "blank context lines must emit as space-prefixed patch lines");
  Expect(PatchHunkCountsMatchBody(*patch), "hunk header counts should match emitted body");
}

void TestPatchGeneratorPreservesRepeatedContextLines() {
  const CompareModel model = MakePatchModel(
      {
          {.left_text = "same", .right_text = "same", .left_line = 1, .right_line = 1,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "same", .right_text = "same", .left_line = 2, .right_line = 2,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "old", .right_text = "new", .left_line = 3, .right_line = 3,
           .kind = CompareRowKind::Modified, .left_changed_spans = {}, .right_changed_spans = {}},
      },
      0, 2);
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "patch with repeated context should be generated");
  Expect(patch->find(" same\n same\n") != std::string::npos,
         "repeated identical context lines must not be deduplicated");
  Expect(PatchHunkCountsMatchBody(*patch), "hunk header counts should match repeated context");
}

void TestPatchGeneratorSelectedLinesBesideBlankContext() {
  const CompareModel model = MakePatchModel(
      {
          {.left_text = "keep", .right_text = "keep", .left_line = 1, .right_line = 1,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "", .right_text = "", .left_line = 2, .right_line = 2,
           .kind = CompareRowKind::Unchanged, .left_changed_spans = {}, .right_changed_spans = {}},
          {.left_text = "old", .right_text = "new", .left_line = 3, .right_line = 3,
           .kind = CompareRowKind::Modified, .left_changed_spans = {}, .right_changed_spans = {}},
      },
      2, 2);
  const auto patch = GenerateComparePatchForRows(model, "file.txt", 2, 2);
  Expect(patch.has_value(), "selected-line patch beside blank context should be generated");
  Expect(patch->find("\n \n") != std::string::npos || patch->find("+\n \n") != std::string::npos,
         "selected-line patch should retain adjacent blank context");
}

void TestPatchGeneratorCrlfContextLines() {
  const CompareModel model = BuildCompareModel("keep\r\n\r\nold\r\n", "keep\r\n\r\nnew\r\n");
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "CRLF compare patch should be generated");
  Expect(PatchHunkCountsMatchBody(*patch), "CRLF context should keep hunk counts aligned");
}

void TestPatchUnstageSelectedLinesWithStagedAndUnstagedChanges() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "file.txt";
  WriteFile(file_path, "a\nb\nc\n");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "a\nB\nc\nd\n");
  RequireGitCommandSuccess(repo_path, {"add", "file.txt"}, "stage staged changes");
  WriteFile(file_path, "a\nB2\nc\nd\n");

  GitRepository repo(repo_path);
  const auto head_blob = repo.Execute({"show", "HEAD:file.txt"});
  const auto index_blob = repo.Execute({"show", ":file.txt"});
  Expect(head_blob.success() && index_blob.success(), "HEAD and index blobs should be readable");
  const CompareModel model = BuildCompareModel(head_blob.output, index_blob.output);

  std::size_t added_row = model.rows.size();
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    if (model.rows[i].kind == CompareRowKind::Added &&
        model.rows[i].right_text == "d") {
      added_row = i;
      break;
    }
  }
  Expect(added_row < model.rows.size(), "staged added line should be present");
  const auto patch = GenerateComparePatchForRows(model, "file.txt", added_row, added_row);
  Expect(patch.has_value(), "unstage patch should be generated for staged line");

  PatchApplyRequest request{
      .operation = PatchOperationKind::UnstageSelectedLines,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("file.txt"),
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success, "unstage patch should apply");

  const auto staged = repo.Execute({"diff", "--cached", "--", "file.txt"});
  Expect(staged.success(), "staged diff should be readable after unstage");
  Expect(staged.output.find("B2") == std::string::npos,
         "unstage should not touch unrelated unstaged worktree edits");
  Expect(staged.output.find("+d") == std::string::npos,
         "selected staged line should be removed from the index");
  Expect(ReadFile(file_path) == "a\nB2\nc\nd\n",
         "unrelated unstaged worktree edits should remain");
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
  tests.push_back({"PatchApply/BlankContextLines", TestPatchGeneratorPreservesBlankContextLines});
  tests.push_back({"PatchApply/RepeatedContextLines",
                   TestPatchGeneratorPreservesRepeatedContextLines});
  tests.push_back({"PatchApply/SelectedLinesBlankContext",
                   TestPatchGeneratorSelectedLinesBesideBlankContext});
  tests.push_back({"PatchApply/CrlfContextLines", TestPatchGeneratorCrlfContextLines});
  tests.push_back({"PatchApply/UnstageMixedIndexWorktree",
                   TestPatchUnstageSelectedLinesWithStagedAndUnstagedChanges});
}

}  // namespace microide::tests
