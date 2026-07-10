#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitPatchApply.h"
#include "project/GitRepository.h"
#include "project/GitRepositoryState.h"
#include "project/PatchGenerator.h"
#include "project/PatchApplyTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/GitRepositoryService.h"
#include "workspace/PatchApplyService.h"
#include "workspace/WorkspaceTabState.h"

#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

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

void TestPatchStageNewFileUsesDevNull() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "seed.txt", "seed\n");
  CommitAll(repo_path, "base", "base");

  // An untracked file diffs empty-vs-content; staging its hunk must create the
  // file in the index via a `/dev/null` patch.
  const auto file_path = repo_path / "created.txt";
  WriteFile(file_path, "alpha\nbeta\n");
  const CompareModel model = BuildCompareModel("", "alpha\nbeta\n");
  const auto patch = GenerateComparePatch(model, "created.txt", 0);
  Expect(patch.has_value(), "new-file patch should be generated");
  Expect(patch->find("--- /dev/null") != std::string::npos,
         "new-file patch old side must be /dev/null");
  Expect(patch->find("@@ -0,0 +1,2 @@") != std::string::npos,
         "new-file hunk header must use a zero-length old range");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("created.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "new-file patch should apply to the index");
  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"show", ":created.txt"});
  Expect(staged.success() && staged.output == "alpha\nbeta\n",
         "staged new file must match the worktree content");
}

void TestPatchStageDeletedFileUsesDevNull() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "doomed.txt", "gamma\ndelta\n");
  CommitAll(repo_path, "base", "base");

  const CompareModel model = BuildCompareModel("gamma\ndelta\n", "");
  const auto patch = GenerateComparePatch(model, "doomed.txt", 0);
  Expect(patch.has_value(), "deleted-file patch should be generated");
  Expect(patch->find("+++ /dev/null") != std::string::npos,
         "deleted-file patch new side must be /dev/null");
  Expect(patch->find("@@ -1,2 +0,0 @@") != std::string::npos,
         "deleted-file hunk header must use a zero-length new range");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("doomed.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "deleted-file patch should apply to the index");
  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"show", ":doomed.txt"});
  Expect(!staged.success(), "deleted file must be gone from the index");
}

void TestPatchStagePreservesMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "a\nb\nc");  // no trailing newline
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "a\nb\nC");  // edit last line, still no trailing newline

  const CompareModel model = BuildCompareModel("a\nb\nc", "a\nb\nC");
  Expect(model.left_final_newline_missing && model.right_final_newline_missing,
         "both sides should be flagged as missing a final newline");
  const auto patch = GenerateComparePatch(model, "nonl.txt", 0);
  Expect(patch.has_value(), "no-newline patch should be generated");
  Expect(patch->find("\\ No newline at end of file") != std::string::npos,
         "patch must carry git's no-newline marker");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("nonl.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "no-newline patch should apply to the index");
  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "a\nb\nC",
         "staged blob must preserve the missing final newline (no added '\\n')");
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

// Regression: staging a hunk whose context includes an interior blank line
// after an earlier insertion shifted the line numbering. The blank Unchanged
// row then has left_line != right_line (both > 0); the old `real_blank_line`
// gate required equality, so it dropped the blank from the body while the @@
// header still counted it — desyncing the header line numbers against the real
// file. `git apply --check` (run by the preflight) then rejected the hunk. The
// fix keys phantom-trailing-EOF suppression on the globally-last row instead of
// line-number equality, so genuine interior blanks are emitted.
void TestPatchStageHunkWithShiftedBlankContextApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "tracked.txt";
  // Committed content has an interior blank line (line 2) before the target.
  const std::string committed = "keep\n\ntarget_old\n";
  WriteFile(file_path, committed);
  CommitAll(repo_path, "base", "base");

  // Working content inserts a line at the very top (shifting every following
  // line's right-side number by one) AND changes the target line.
  const std::string working = "inserted\nkeep\n\ntarget_new\n";
  WriteFile(file_path, working);

  const CompareModel model = BuildCompareModel(committed, working);
  // Locate the hunk that carries the target modification (not the top insert).
  int target_hunk = -1;
  for (const CompareRow& row : model.rows) {
    if (row.kind == CompareRowKind::Modified && row.left_text == "target_old") {
      target_hunk = row.hunk;
      break;
    }
  }
  Expect(target_hunk >= 0, "expected a modified hunk for the target line");

  const auto patch = GenerateComparePatch(model, "tracked.txt", target_hunk);
  Expect(patch.has_value(), "target-hunk patch should be generated");
  // The interior blank line must survive as a space-only context line...
  Expect(patch->find("\n \n") != std::string::npos,
         "shifted interior blank line must be emitted as a context line");
  // ...and the phantom trailing-EOL blank must NOT be appended as a bogus line.
  Expect(PatchHunkCountsMatchBody(*patch), "hunk header counts should match the emitted body");

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
  Expect(result.category == PatchApplyResultCategory::Success,
         "staging a hunk with a shifted interior blank line must apply cleanly");

  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"diff", "--cached", "--", "tracked.txt"});
  Expect(staged.success() && staged.output.find("+target_new") != std::string::npos,
         "staged diff should contain the changed target line");
}

// Appending lines to a file that has no trailing newline: the shared last line must
// become a delete/add pair so git's `\ No newline` marker lands on the old side's
// final line only. Regression for the corruption where the marker on a *context*
// line made git treat it as EOF and fuse the following added content onto it.
void TestPatchStageAppendAfterMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "line1\nline2");        // committed: no trailing newline
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "line1\nline2\nline3\n");  // append a line (now newline-terminated)

  const CompareModel model = BuildCompareModel("line1\nline2", "line1\nline2\nline3\n");
  const auto patch = GenerateComparePatch(model, "nonl.txt", 0);
  Expect(patch.has_value(), "append-after-no-newline patch should be generated");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("nonl.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "append-after-no-newline patch should apply to the index");
  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "line1\nline2\nline3\n",
         "staged blob must be exactly the appended content (line2 and line3 not fused)");
}

// Deleting the trailing lines of a newline-terminated file, leaving a new content
// with no final newline. Previously the marker was misplaced on a context line and
// git rejected the patch (could not stage/discard the hunk).
void TestPatchStageDeleteTrailingLeavesMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "line1\nline2\nline3\n");  // committed: newline-terminated
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "line1");                  // keep only line1, no trailing newline

  const CompareModel model = BuildCompareModel("line1\nline2\nline3\n", "line1");
  const auto patch = GenerateComparePatch(model, "nonl.txt", 0);
  Expect(patch.has_value(), "delete-trailing patch should be generated");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("nonl.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
      .model = model,
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "delete-trailing patch should apply to the index");
  GitRepository repo(repo_path);
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "line1",
         "staged blob must be exactly 'line1' with no added trailing newline");
}

// J37: the copy-to-clipboard "file patch" path (CopyCompareFilePatch) now routes
// through the real generator (whole-file row span) instead of the removed
// display-only exporter that emitted fake `@@ hunk N @@` headers. The copied
// text must be a genuine unified diff that git accepts.
void TestCopyFilePatchWholeFileIsRealUnifiedDiffAndApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "tracked.txt";
  WriteFile(file_path, "one\ntwo\nthree\n");
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "one\nTWO\nthree\n");

  const CompareModel model = BuildCompareModel("one\ntwo\nthree\n", "one\nTWO\nthree\n");
  // CopyCompareFilePatch spans every model row through the real generator.
  const auto patch =
      GenerateComparePatchForRows(model, "tracked.txt", 0, model.rows.size() - 1);
  Expect(patch.has_value(), "whole-file copy patch should be generated");
  Expect(patch->find("@@ hunk ") == std::string::npos,
         "copied patch must not use the removed exporter's fake '@@ hunk N @@' headers");
  Expect(patch->find("--- a/tracked.txt") != std::string::npos,
         "copied patch must carry a real --- header");
  Expect(patch->find("+++ b/tracked.txt") != std::string::npos,
         "copied patch must carry a real +++ header");
  Expect(std::regex_search(*patch, std::regex(R"(@@ -\d+,\d+ \+\d+,\d+ @@)")),
         "copied patch must carry a real unified @@ range header");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("tracked.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
  };
  const auto result = ApplyPatchRequest(request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "copied whole-file patch must be git-apply-able");
}

// J37: copied patches must be git-apply-able for the full edge matrix — an add,
// a delete, a path containing spaces, and a file missing its trailing newline.
void TestCopyPatchAppliesForAddDeleteSpacesAndNoNewline() {
  const auto make_repo = [](const TemporaryDirectory& temp) {
    const auto repo_path = temp.path() / "repo";
    std::filesystem::create_directories(repo_path);
    InitializeGitRepo(repo_path);
    WriteFile(repo_path / "seed.txt", "seed\n");
    CommitAll(repo_path, "base", "base");
    return repo_path;
  };

  // Add (whole-file copy of an untracked file uses a /dev/null old side).
  {
    TemporaryDirectory temp_dir;
    const auto repo_path = make_repo(temp_dir);
    WriteFile(repo_path / "created.txt", "alpha\nbeta\n");
    const CompareModel model = BuildCompareModel("", "alpha\nbeta\n");
    const auto patch = GenerateComparePatchForRows(model, "created.txt", 0, model.rows.size() - 1);
    Expect(patch.has_value(), "added-file copy patch should be generated");
    Expect(patch->find("--- /dev/null") != std::string::npos,
           "added-file copy patch old side must be /dev/null");
    PatchApplyRequest request{
        .operation = PatchOperationKind::StageHunk,
        .target{.repository_root = repo_path,
                .relative_path = std::filesystem::path("created.txt"),
                .hunk = std::nullopt,
                .line_selection = std::nullopt},
    };
    Expect(ApplyPatchRequest(request, *patch).category == PatchApplyResultCategory::Success,
           "copied add patch must apply");
  }

  // Delete (whole-file copy of a removed file uses a /dev/null new side).
  {
    TemporaryDirectory temp_dir;
    const auto repo_path = temp_dir.path() / "repo";
    std::filesystem::create_directories(repo_path);
    InitializeGitRepo(repo_path);
    WriteFile(repo_path / "doomed.txt", "gamma\ndelta\n");
    CommitAll(repo_path, "base", "base");
    const CompareModel model = BuildCompareModel("gamma\ndelta\n", "");
    const auto patch = GenerateComparePatchForRows(model, "doomed.txt", 0, model.rows.size() - 1);
    Expect(patch.has_value(), "deleted-file copy patch should be generated");
    Expect(patch->find("+++ /dev/null") != std::string::npos,
           "deleted-file copy patch new side must be /dev/null");
    PatchApplyRequest request{
        .operation = PatchOperationKind::StageHunk,
        .target{.repository_root = repo_path,
                .relative_path = std::filesystem::path("doomed.txt"),
                .hunk = std::nullopt,
                .line_selection = std::nullopt},
    };
    Expect(ApplyPatchRequest(request, *patch).category == PatchApplyResultCategory::Success,
           "copied delete patch must apply");
  }

  // Path with spaces (git leaves such paths unquoted; the header must still apply).
  {
    TemporaryDirectory temp_dir;
    const auto repo_path = temp_dir.path() / "repo";
    std::filesystem::create_directories(repo_path);
    InitializeGitRepo(repo_path);
    WriteFile(repo_path / "a b.txt", "x\n");
    CommitAll(repo_path, "base", "base");
    const CompareModel model = BuildCompareModel("x\n", "y\n");
    const auto patch = GenerateComparePatch(model, "a b.txt", 0);
    Expect(patch.has_value(), "spaces-path copy patch should be generated");
    Expect(patch->find("--- a/a b.txt") != std::string::npos,
           "a space-containing path stays unquoted, matching git");
    PatchApplyRequest request{
        .operation = PatchOperationKind::StageHunk,
        .target{.repository_root = repo_path,
                .relative_path = std::filesystem::path("a b.txt"),
                .hunk = std::nullopt,
                .line_selection = std::nullopt},
    };
    Expect(ApplyPatchRequest(request, *patch).category == PatchApplyResultCategory::Success,
           "copied spaces-path patch must apply");
  }

  // Missing trailing newline.
  {
    TemporaryDirectory temp_dir;
    const auto repo_path = temp_dir.path() / "repo";
    std::filesystem::create_directories(repo_path);
    InitializeGitRepo(repo_path);
    WriteFile(repo_path / "nonl.txt", "a\nb\nc");
    CommitAll(repo_path, "base", "base");
    const CompareModel model = BuildCompareModel("a\nb\nc", "a\nb\nC");
    const auto patch = GenerateComparePatchForRows(model, "nonl.txt", 0, model.rows.size() - 1);
    Expect(patch.has_value(), "no-newline copy patch should be generated");
    Expect(patch->find("\\ No newline at end of file") != std::string::npos,
           "copied no-newline patch must carry git's marker");
    PatchApplyRequest request{
        .operation = PatchOperationKind::StageHunk,
        .target{.repository_root = repo_path,
                .relative_path = std::filesystem::path("nonl.txt"),
                .hunk = std::nullopt,
                .line_selection = std::nullopt},
    };
    Expect(ApplyPatchRequest(request, *patch).category == PatchApplyResultCategory::Success,
           "copied no-newline patch must apply");
  }
}

// J38: patch header paths must be quoted/escaped exactly like git so that tabs,
// newlines, quotes, backslashes, and high-bit bytes cannot split or corrupt the
// `--- ` / `+++ ` / `diff --git` lines. Spaces and a leading dash under the
// `a/`/`b/` prefix stay unquoted, matching git.
void TestPatchHeaderQuotesUnsafePaths() {
  const CompareModel model = BuildCompareModel("x\n", "y\n");
  struct Case {
    std::filesystem::path path;
    std::string expected_minus_header;  // exact bytes expected on the `--- ` line
    const char* note;
  };
  const std::vector<Case> cases = {
      {std::filesystem::path("a b.txt"), "--- a/a b.txt", "space stays unquoted"},
      {std::filesystem::path("a\tb.txt"), "--- \"a/a\\tb.txt\"", "tab escaped and quoted"},
      {std::filesystem::path("a\nb.txt"), "--- \"a/a\\nb.txt\"", "newline escaped and quoted"},
      {std::filesystem::path("a\"b.txt"), "--- \"a/a\\\"b.txt\"", "double-quote escaped and quoted"},
      {std::filesystem::path("a\\b.txt"), "--- \"a/a\\\\b.txt\"", "backslash escaped and quoted"},
      {std::filesystem::path("-lead.txt"), "--- a/-lead.txt", "leading dash stays unquoted under a/"},
      {std::filesystem::path("caf\xc3\xa9.txt"), "--- \"a/caf\\303\\251.txt\"",
       "non-ASCII octal-escaped and quoted"},
  };
  for (const Case& test_case : cases) {
    const auto patch = GenerateComparePatch(model, test_case.path, 0);
    Expect(patch.has_value(), test_case.note);
    Expect(patch->find(test_case.expected_minus_header) != std::string::npos, test_case.note);
    // The diff --git line must carry the same quoted token on both sides.
    if (test_case.expected_minus_header.rfind("--- \"", 0) == 0) {
      Expect(patch->find("diff --git \"a/") != std::string::npos,
             "quoted path must also quote the diff --git a/ token");
    }
  }
}

// J39: building a patch-apply request must not deep-copy the whole compare model.
// The patch text is generated synchronously from the live model; only compact
// target metadata is dispatched, so staging one hunk costs O(hunk), not O(file).
void TestPatchApplyRequestDoesNotCopyModel() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path();

  microide::project::ProjectBackgroundExecutor executor;
  microide::workspace::GitRepositoryService git_service(executor);
  microide::workspace::PatchApplyService service(executor, git_service);

  microide::project::GitRepositoryState repo_state;
  repo_state.repository_root = repo_path;
  repo_state.repo_available = true;
  repo_state.generation = 1;
  microide::workspace::PatchApplyService::Callbacks callbacks;
  callbacks.current_repository_state = [repo_state]() { return repo_state; };
  service.SetCallbacks(std::move(callbacks));

  // A deliberately large diff. Staging a single hunk must not clone all of it.
  std::string left;
  std::string right;
  for (int i = 0; i < 500; ++i) {
    const std::string tag = std::to_string(i);
    left += "line" + tag + "\n";
    right += (i == 250 ? "CHANGED" + tag : "line" + tag);
    right += "\n";
  }
  microide::workspace::CompareTabState compare_tab;
  compare_tab.path = repo_path / "file.txt";
  compare_tab.right_ref = "WORKTREE";
  compare_tab.model = BuildCompareModel(left, right);
  Expect(compare_tab.model.rows.size() > 400, "expected a large compare model");
  std::size_t changed_row = compare_tab.model.rows.size();
  for (std::size_t i = 0; i < compare_tab.model.rows.size(); ++i) {
    if (compare_tab.model.rows[i].kind != CompareRowKind::Unchanged) {
      changed_row = i;
      break;
    }
  }
  Expect(changed_row < compare_tab.model.rows.size(), "expected a changed row in the model");
  compare_tab.selected_row = changed_row;

  const auto request = service.BuildRequestForTesting(
      compare_tab, PatchOperationKind::StageHunk, /*line_scope=*/false);
  Expect(request.has_value(), "request should build for a working-tree text hunk");
  Expect(request->target.hunk.has_value(), "request should target a hunk");
  // The fix: the dispatched request carries no copy of the (large) diff model.
  Expect(request->model.rows.empty(),
         "request must not deep-copy the compare model (cost bounded by hunk, not file)");
  // The source model is untouched — the emptiness above is the request's, not a
  // side effect that mutated the tab.
  Expect(compare_tab.model.rows.size() > 400, "source model must remain intact");

  executor.Shutdown();
}

void RegisterPatchApplyTests(std::vector<TestCase>& tests) {
  tests.push_back({"PatchApply/AppendAfterMissingFinalNewline",
                   TestPatchStageAppendAfterMissingFinalNewline});
  tests.push_back({"PatchApply/DeleteTrailingLeavesMissingFinalNewline",
                   TestPatchStageDeleteTrailingLeavesMissingFinalNewline});
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
  tests.push_back({"PatchApply/StageNewFileDevNull", TestPatchStageNewFileUsesDevNull});
  tests.push_back({"PatchApply/StageDeletedFileDevNull", TestPatchStageDeletedFileUsesDevNull});
  tests.push_back({"PatchApply/PreserveMissingFinalNewline",
                   TestPatchStagePreservesMissingFinalNewline});
  tests.push_back({"PatchApply/StageHunkShiftedBlankContext",
                   TestPatchStageHunkWithShiftedBlankContextApplies});
  tests.push_back({"PatchApply/CopyFilePatchIsRealUnifiedDiff",
                   TestCopyFilePatchWholeFileIsRealUnifiedDiffAndApplies});
  tests.push_back({"PatchApply/CopyPatchAddDeleteSpacesNoNewline",
                   TestCopyPatchAppliesForAddDeleteSpacesAndNoNewline});
  tests.push_back({"PatchApply/HeaderQuotesUnsafePaths", TestPatchHeaderQuotesUnsafePaths});
  tests.push_back({"PatchApply/RequestDoesNotCopyModel", TestPatchApplyRequestDoesNotCopyModel});
}

}  // namespace microide::tests
