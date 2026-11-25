#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitPatchApply.h"
#include "project/GitRepository.h"
#include "project/GitRepositoryState.h"
#include "project/PatchGenerator.h"
#include "project/PatchApplyTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/git/PatchApplyService.h"
#include "workspace/state/WorkspaceTabState.h"
#include "util/Parse.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
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

std::string ReadFileOrEmptyIfDeleted(const std::filesystem::path& path) {
  return std::filesystem::exists(path) ? ReadFile(path) : std::string{};
}

std::string ExpectedSingleHunkResult(const CompareModel& model,
                                     const microide::compare::CompareHunk& hunk) {
  std::vector<std::string_view> lines;
  for (std::size_t row = 0; row < model.rows.size(); ++row) {
    const CompareRow& compare_row = model.rows[row];
    const bool in_hunk = static_cast<int>(row) >= hunk.start_row &&
                         static_cast<int>(row) <= hunk.end_row;
    if (in_hunk) {
      if (compare_row.right_line > 0) {
        lines.push_back(compare_row.right_text);
      }
    } else if (compare_row.left_line > 0) {
      lines.push_back(compare_row.left_text);
    }
  }
  const std::string_view terminator = model.left_uses_crlf ? "\r\n" : "\n";
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out.append(terminator);
    }
    out.append(lines[i]);
  }
  return out;
}

void GenerateComparePair(std::uint64_t& seed, std::string* left, std::string* right) {
  const auto next = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(seed >> 33);
  };
  left->clear();
  right->clear();
  const bool crlf = next() % 5 == 0;
  const bool left_final_newline = next() % 4 != 0;
  const bool right_final_newline = next() % 4 != 0;
  const std::string_view terminator = crlf ? "\r\n" : "\n";
  std::vector<std::string> left_lines;
  std::vector<std::string> right_lines;
  const std::size_t line_count = 1 + next() % 30;
  for (std::size_t i = 0; i < line_count; ++i) {
    const std::string line = "line" + std::to_string(next() % 6) + "_" + std::to_string(i % 4);
    left_lines.push_back(line);
    switch (next() % 6) {
      case 0:
        break;
      case 1:
        right_lines.push_back(line + "_edited");
        break;
      case 2:
        right_lines.push_back("inserted" + std::to_string(next() % 5));
        right_lines.push_back(line);
        break;
      default:
        right_lines.push_back(line);
        break;
    }
  }
  const auto join = [&](const std::vector<std::string>& lines, bool final_newline,
                        std::string* out) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      out->append(lines[i]);
      if (i + 1 < lines.size() || final_newline) {
        out->append(terminator);
      }
    }
  };
  join(left_lines, left_final_newline, left);
  join(right_lines, right_final_newline, right);
}

void TestGeneratedPatchesApplyWithRealGit() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "seed.txt", "seed\n");
  CommitAll(repo_path, "seed", "seed");

  const std::filesystem::path target = repo_path / "f.txt";
  const std::filesystem::path patch_path = temp_dir.path() / "generated.patch";
  std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
  std::size_t applied_hunks = 0;
  std::size_t full_applies = 0;

  for (int iteration = 0; iteration < 120; ++iteration) {
    std::string left;
    std::string right;
    GenerateComparePair(seed, &left, &right);
    if (left == right) {
      continue;
    }
    const CompareModel model = BuildCompareModel(left, right);
    if (model.hunks.empty()) {
      continue;
    }

    for (const microide::compare::CompareHunk& hunk : model.hunks) {
      const auto patch = GenerateComparePatch(model, "f.txt", hunk.index);
      if (!patch.has_value()) {
        continue;
      }
      WriteFile(target, left);
      WriteFile(patch_path, *patch);
      Expect(RunGitCommand(repo_path, {"apply", "--check", patch_path.string()}) == 0,
             "git apply --check must accept a generated hunk patch");
      Expect(RunGitCommand(repo_path, {"apply", patch_path.string()}) == 0,
             "git apply must apply a generated hunk patch");
      if (ReadFileOrEmptyIfDeleted(target) != ExpectedSingleHunkResult(model, hunk)) {
        std::fprintf(stderr,
                     "hunk %d of %zu (rows %d..%d)\n--- LEFT ---\n%s\n--- RIGHT ---\n%s\n"
                     "--- PATCH ---\n%s\n--- GIT PRODUCED ---\n%s\n--- EXPECTED ---\n%s\n",
                     hunk.index, model.hunks.size(), hunk.start_row, hunk.end_row, left.c_str(),
                     right.c_str(), patch->c_str(), ReadFileOrEmptyIfDeleted(target).c_str(),
                     ExpectedSingleHunkResult(model, hunk).c_str());
        Expect(false, "applying one hunk must change exactly that hunk's lines");
      }
      ++applied_hunks;
    }

    WriteFile(target, left);
    bool all_applied = true;
    for (const microide::compare::CompareHunk& hunk : model.hunks) {
      const auto patch = GenerateComparePatch(model, "f.txt", hunk.index);
      if (!patch.has_value()) {
        all_applied = false;
        break;
      }
      WriteFile(patch_path, *patch);
      if (RunGitCommand(repo_path, {"apply", patch_path.string()}) != 0) {
        all_applied = false;
        break;
      }
    }
    if (all_applied) {
      Expect(ReadFileOrEmptyIfDeleted(target) == right,
             "applying every hunk must reproduce the right side exactly");
      ++full_applies;
    }
  }

  Expect(applied_hunks >= 100,
         "the generator must produce a substantial number of applicable hunks");
  Expect(full_applies >= 20, "and a substantial number of complete applies");
}

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

void TestPatchGeneratorEnforcesByteBudget() {
  std::string right = "alpha\n";
  for (int i = 0; i < 500; ++i) {
    right += "added line " + std::to_string(i) + "\n";
  }
  const CompareModel model = BuildCompareModel("alpha\n", right);
  Expect(!model.hunks.empty(), "compare model should contain a hunk");

  microide::project::PatchGenerationOptions tiny;
  tiny.max_patch_bytes = 128;
  Expect(!GenerateComparePatch(model, "file.txt", 0, tiny).has_value(),
         "an over-budget patch should be refused (nullopt)");

  microide::project::PatchGenerationOptions generous;
  generous.max_patch_bytes = 64u * 1024 * 1024;
  Expect(GenerateComparePatch(model, "file.txt", 0, generous).has_value(),
         "the same patch generates within a generous budget");
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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success, "git apply should succeed");

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"diff", "--cached", "--", "tracked.txt"});
  Expect(staged.success(), "staged diff should be readable");
  Expect(staged.output.find("+line2") != std::string::npos,
         "staged diff should contain added line");
}

void TestPatchGeneratorOneSidedPhantomTrailingLineApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "f.txt";
  WriteFile(file_path, "c\n");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "c\n\nd\n");
  const CompareModel model = BuildCompareModel("c\n", "c\n\nd\n");
  const auto patch = GenerateComparePatch(model, "f.txt", 0);
  Expect(patch.has_value(), "one-sided-phantom patch should be generated");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("f.txt"),
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "git apply should accept the appended-blank-line patch");
}

void TestPatchGeneratorNonTerminalHunkNoNewlineMarkerApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "f.txt";
  WriteFile(file_path, "p\nalpha\nq");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "P\nalpha");
  const CompareModel model = BuildCompareModel("p\nalpha\nq", "P\nalpha");
  Expect(model.hunks.size() >= 2, "fixture should produce at least two hunks");

  const auto patch = GenerateComparePatch(model, "f.txt", 0);
  Expect(patch.has_value(), "non-terminal hunk patch should be generated");
  Expect(patch->find("\\ No newline at end of file") == std::string::npos,
         "a non-terminal isolated hunk must not emit an EOF no-newline marker");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("f.txt"),
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "git apply should accept the non-terminal hunk without a stray no-newline marker");
}

void TestPatchFailureCategoryForAMissingRepository() {
  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = "/nonexistent",
          .relative_path = "missing.txt",
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, "not a real patch\n");
  Expect(result.category == PatchApplyResultCategory::PatchDidNotApply ||
             result.category == PatchApplyResultCategory::UnsupportedTarget,
         "invalid repository should fail safely");
}

void TestClassifyGitApplyFailureSeparatesStaleFromCorrupt() {
  using microide::project::ClassifyGitApplyFailure;

  for (const std::string_view stale : {
           "error: patch failed: src/main.cpp:12\nerror: src/main.cpp: patch does not apply\n",
           "error: src/main.cpp: does not match index\n",
           "error: src/new.cpp: already exists in working directory\n",
           "error: src/gone.cpp: No such file or directory\n",
           "error: src/gone.cpp: does not exist in index\n",
       }) {
    Expect(ClassifyGitApplyFailure(stale) == PatchApplyResultCategory::StaleDiff,
           "a content/index mismatch must classify as StaleDiff so the user is told to refresh");
  }

  for (const std::string_view corrupt : {
           "error: corrupt patch at line 5\n",
           "error: No valid patches in input (allow with \"--allow-empty\")\n",
           "fatal: corrupt patch at line 7\n",
           "error: unrecognized input\n",
           "error: patch fragment without header at line 3\n",
       }) {
    Expect(ClassifyGitApplyFailure(corrupt) == PatchApplyResultCategory::PatchDidNotApply,
           "a corrupt/unparseable patch must NOT be reported as a stale diff");
  }

  Expect(ClassifyGitApplyFailure(
             "error: corrupt patch at line 4\nerror: a.cpp: patch does not apply\n") ==
             PatchApplyResultCategory::PatchDidNotApply,
         "a corrupt patch outranks a co-reported mismatch");

  Expect(ClassifyGitApplyFailure("") == PatchApplyResultCategory::PatchDidNotApply,
         "empty output falls back to PatchDidNotApply");
  Expect(ClassifyGitApplyFailure("error: something entirely new\n") ==
             PatchApplyResultCategory::PatchDidNotApply,
         "an unrecognized git error falls back to PatchDidNotApply");
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

struct ParsedPatchHunkHeader {
  int old_count{1};
  int new_count{1};
  std::size_t body_offset{0};
  bool explicit_counts{false};
};

std::optional<ParsedPatchHunkHeader> ParsePatchHunkHeader(std::string_view patch) {
  const std::size_t start = patch.find("@@ -");
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view rest = patch.substr(start + 3);
  const auto consume = [&rest](std::string_view token) {
    if (!rest.starts_with(token)) {
      return false;
    }
    rest.remove_prefix(token.size());
    return true;
  };
  const auto number = [&rest]() -> std::optional<int> {
    const std::size_t end = rest.find_first_not_of("0123456789");
    const std::size_t length = end == std::string_view::npos ? rest.size() : end;
    const auto value = util::ParseInt(rest.substr(0, length));
    rest.remove_prefix(length);
    return value;
  };
  ParsedPatchHunkHeader header;
  bool old_explicit = false;
  bool new_explicit = false;
  const auto range = [&](std::string_view sign, int& count, bool& explicit_count) {
    if (!consume(sign) || !number().has_value()) {
      return false;
    }
    if (consume(",")) {
      const auto parsed_count = number();
      if (!parsed_count.has_value()) {
        return false;
      }
      count = *parsed_count;
      explicit_count = true;
    }
    return true;
  };
  if (!range("-", header.old_count, old_explicit) || !consume(" ") ||
      !range("+", header.new_count, new_explicit) || !consume(" @@")) {
    return std::nullopt;
  }
  header.body_offset = patch.size() - rest.size();
  header.explicit_counts = old_explicit && new_explicit;
  return header;
}

bool PatchHunkCountsMatchBody(const std::string& patch) {
  const auto header = ParsePatchHunkHeader(patch);
  if (!header.has_value()) {
    return false;
  }
  const auto body = std::string_view(patch).substr(header->body_offset);
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
  return counted_old == header->old_count && counted_new == header->new_count;
}

void TestPatchHunkHeaderOracle() {
  Expect(PatchHunkCountsMatchBody("@@ -1 +1 @@\n-old\n+new\n"),
         "an omitted count means one line on each side");
  Expect(PatchHunkCountsMatchBody("@@ -0,0 +1,2 @@\n+one\n+two\n"),
         "an insertion can have an explicit zero old count");
  Expect(PatchHunkCountsMatchBody("@@ -1,2 +1,2 @@\n \n-old\n+new\n"),
         "a blank context line counts on both sides");
  Expect(!PatchHunkCountsMatchBody("@@ -1,2 +1,1 @@\n-old\n+new\n"),
         "the oracle must reject a header whose old count differs from its body");
  Expect(!ParsePatchHunkHeader("@@ -1, +1,1 @@\n").has_value(),
         "a comma needs a count");
  Expect(!ParsePatchHunkHeader("@@ -1,999999999999999999999 +1,1 @@\n").has_value(),
         "an overflowing count must be rejected without throwing");
  Expect(!ParsePatchHunkHeader("@@ -x,1 +1,1 @@\n").has_value(),
         "a range needs a numeric starting line");
  Expect(!ParsePatchHunkHeader("@@ -1,1 +1,1 @\n").has_value(),
         "a header needs its closing delimiter");
  Expect(!ParsePatchHunkHeader("not a patch").has_value(),
         "a patch without a hunk header must be rejected");
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
  Expect(model.left_uses_crlf && model.right_uses_crlf, "model should classify both sides as CRLF");
  const auto patch = GenerateComparePatch(model, "file.txt", 0);
  Expect(patch.has_value(), "CRLF compare patch should be generated");
  Expect(PatchHunkCountsMatchBody(*patch), "CRLF context should keep hunk counts aligned");
  Expect(patch->find(" keep\r\n") != std::string::npos,
         "CRLF context line must carry a carriage return");
  Expect(patch->find("-old\r\n") != std::string::npos,
         "CRLF deletion line must carry a carriage return");
  Expect(patch->find("+new\r\n") != std::string::npos,
         "CRLF addition line must carry a carriage return");
}

void TestPatchStageCrlfFileApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  RequireGitCommandSuccess(repo_path, {"config", "core.autocrlf", "false"}, "disable autocrlf");
  const auto file_path = repo_path / "crlf.txt";
  WriteFile(file_path, "line1\r\nline2\r\n");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "line1\r\nline2\r\nline3\r\n");
  const CompareModel model =
      BuildCompareModel("line1\r\nline2\r\n", "line1\r\nline2\r\nline3\r\n");
  Expect(model.right_uses_crlf, "model should detect a CRLF right side");
  const auto patch = GenerateComparePatch(model, "crlf.txt", 0);
  Expect(patch.has_value(), "CRLF stage patch should be generated");
  Expect(patch->find("+line3\r\n") != std::string::npos,
         "the added CRLF line must carry a carriage return");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("crlf.txt"),
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "git apply --cached should accept the CRLF patch");

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"show", ":crlf.txt"});
  Expect(staged.success(), "staged blob should be readable");
  Expect(staged.output == "line1\r\nline2\r\nline3\r\n",
         "staged blob must preserve CRLF endings on the added line");
}

void TestPatchApplyRejectsIgnoreWhitespace() {
  microide::project::ProjectBackgroundExecutor executor;
  microide::workspace::GitRepositoryService git_service(executor);
  microide::workspace::PatchApplyService service(executor, git_service);

  std::string feedback;
  microide::workspace::PatchApplyService::Callbacks callbacks;
  callbacks.current_repository_state = []() {
    return microide::project::GitRepositoryState{};
  };
  callbacks.set_command_feedback = [&feedback](std::string_view message) {
    feedback = std::string(message);
  };
  service.SetCallbacks(std::move(callbacks));

  microide::workspace::CompareTabState compare_tab;
  compare_tab.path = std::filesystem::path("/unused/file.txt");
  compare_tab.review_mode = microide::compare::CompareReviewMode::WorkingTree;
  compare_tab.right_ref = "WORKTREE";
  compare_tab.model = BuildCompareModel("a\n", "a\nb\n");
  compare_tab.build_options.ignore_whitespace = true;

  const auto expect_refused = [&](bool result, const char* label) {
    Expect(!result, label);
    Expect(feedback.find("Ignore Whitespace") != std::string::npos,
           "refusal message should name Ignore Whitespace");
    feedback.clear();
  };
  expect_refused(service.RequestStageHunk(compare_tab), "stage hunk must be refused");
  expect_refused(service.RequestStageSelectedLines(compare_tab), "stage lines must be refused");
  expect_refused(service.RequestUnstageHunk(compare_tab), "unstage hunk must be refused");
  expect_refused(service.RequestUnstageSelectedLines(compare_tab), "unstage lines must be refused");
  expect_refused(service.RequestDiscardHunkPreview(compare_tab), "discard hunk must be refused");
  expect_refused(service.RequestDiscardSelectedLinesPreview(compare_tab),
                 "discard lines must be refused");

  compare_tab.build_options.ignore_whitespace = false;
  feedback.clear();
  Expect(!service.RequestStageHunk(compare_tab),
         "stage still fails without a repository");
  Expect(feedback.find("Ignore Whitespace") == std::string::npos,
         "with ignore-whitespace off the guard must not fire");

  executor.Shutdown();
}

void TestPatchApplyFinishingInTheBackgroundStaysWithItsProject() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "alpha";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "file.txt";
  WriteFile(file_path, "a\nb\n");
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "A\nb\n");

  microide::project::ProjectBackgroundExecutor executor;
  microide::workspace::GitRepositoryService git_service(executor);
  microide::workspace::PatchApplyService service(executor, git_service);
  std::filesystem::path front = repo_path;
  int front_project_touched = 0;
  std::filesystem::path deferred;
  std::string feedback;
  microide::workspace::PatchApplyService::Callbacks callbacks;
  callbacks.current_repository_state = [&]() {
    microide::project::GitRepositoryState state;
    state.repository_root = repo_path;
    state.repo_available = true;
    return state;
  };
  callbacks.request_git_refresh = [&]() { ++front_project_touched; };
  callbacks.reload_clean_editor_tabs_for_path = [&](const std::filesystem::path&) { ++front_project_touched; };
  callbacks.refresh_compare_tab_for_path = [&](const std::filesystem::path&) { ++front_project_touched; };
  callbacks.set_command_feedback = [&](std::string_view message) { feedback = std::string(message); };
  callbacks.front_project_root = [&]() { return front; };
  callbacks.is_in_front = [&](const std::filesystem::path& root) { return root == front; };
  callbacks.project_label = [](const std::filesystem::path& root) { return root.filename().string(); };
  callbacks.defer_to_project = [&](const std::filesystem::path& root) { deferred = root; };
  service.SetCallbacks(std::move(callbacks));

  microide::workspace::CompareTabState compare_tab;
  compare_tab.path = file_path;
  compare_tab.review_mode = microide::compare::CompareReviewMode::WorkingTree;
  compare_tab.right_ref = "WORKTREE";
  compare_tab.staging_view = microide::compare::WorkingTreeStagingView::Unstaged;
  compare_tab.model = BuildCompareModel("a\nb\n", "A\nb\n");
  Expect(service.RequestStageHunk(compare_tab), "the stage is dispatched in alpha");
  front = temp_dir.path() / "beta";
  Expect(WaitUntil([&] { return service.PendingCompletionCount() > 0; }), "the apply finishes");
  service.DrainCompletions();

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  Expect(repo.Execute({"diff", "--cached", "--", "file.txt"}).output.find("+A") != std::string::npos,
         "the hunk was staged in alpha");
  Expect(front_project_touched == 0, "the front project's git view, tabs and compares are left alone");
  Expect(deferred == repo_path, "alpha catches up when it is activated");
  Expect(feedback == "Patch applied in alpha", "and the feedback names it: " + feedback);
  executor.Shutdown();
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

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
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

void TestPatchStagePureInsertionWithNoContextApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "file.txt";
  WriteFile(file_path, "a\nb\n");
  CommitAll(repo_path, "base", "base");

  WriteFile(file_path, "a\nZ\nc\nd\n");
  const CompareModel model = BuildCompareModel("a\nb\n", "a\nZ\nc\nd\n");

  std::size_t first_added = model.rows.size();
  std::size_t last_added = 0;
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    if (model.rows[i].kind == CompareRowKind::Added &&
        (model.rows[i].right_text == "c" || model.rows[i].right_text == "d")) {
      first_added = std::min(first_added, i);
      last_added = std::max(last_added, i);
    }
  }
  Expect(first_added <= last_added && last_added < model.rows.size(),
         "the two appended lines should be present as Added rows");

  const auto patch = GenerateComparePatchForRows(model, "file.txt", first_added, last_added);
  Expect(patch.has_value(), "a pure-insertion patch should be generated");
  Expect(patch->find("@@ -2,0 ") != std::string::npos,
         "pure insertion after old line 2 must emit a zero-length old range at line 2");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{
          .repository_root = repo_path,
          .relative_path = std::filesystem::path("file.txt"),
          .hunk = std::nullopt,
          .line_selection = std::nullopt,
      },
      .model = model,
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "the pure-insertion patch must apply cleanly (no corrupt hunk header)");

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"diff", "--cached", "--", "file.txt"});
  Expect(staged.success() && staged.output.find("+c") != std::string::npos &&
             staged.output.find("+d") != std::string::npos,
         "staging the insertion should place both added lines in the index");
  Expect(staged.output.find("Z") == std::string::npos,
         "the unrelated modification must not be staged by an insertion-only patch");
}

void TestPatchStageNewFileUsesDevNull() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "seed.txt", "seed\n");
  CommitAll(repo_path, "base", "base");

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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "new-file patch should apply to the index");
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "deleted-file patch should apply to the index");
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"show", ":doomed.txt"});
  Expect(!staged.success(), "deleted file must be gone from the index");
}

void TestPatchStagePreservesMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "a\nb\nc");
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "a\nb\nC");

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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "no-newline patch should apply to the index");
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "a\nb\nC",
         "staged blob must preserve the missing final newline (no added '\\n')");
}

}

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

void TestPatchStageHunkWithShiftedBlankContextApplies() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "tracked.txt";
  const std::string committed = "keep\n\ntarget_old\n";
  WriteFile(file_path, committed);
  CommitAll(repo_path, "base", "base");

  const std::string working = "inserted\nkeep\n\ntarget_new\n";
  WriteFile(file_path, working);

  const CompareModel model = BuildCompareModel(committed, working);
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
  Expect(patch->find("\n \n") != std::string::npos,
         "shifted interior blank line must be emitted as a context line");
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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "staging a hunk with a shifted interior blank line must apply cleanly");

  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"diff", "--cached", "--", "tracked.txt"});
  Expect(staged.success() && staged.output.find("+target_new") != std::string::npos,
         "staged diff should contain the changed target line");
}

void TestPatchStageAppendAfterMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "line1\nline2");
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "line1\nline2\nline3\n");

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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "append-after-no-newline patch should apply to the index");
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "line1\nline2\nline3\n",
         "staged blob must be exactly the appended content (line2 and line3 not fused)");
}

void TestPatchStageDeleteTrailingLeavesMissingFinalNewline() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "nonl.txt";
  WriteFile(file_path, "line1\nline2\nline3\n");
  CommitAll(repo_path, "base", "base");
  WriteFile(file_path, "line1");

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
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "delete-trailing patch should apply to the index");
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());
  const auto staged = repo.Execute({"show", ":nonl.txt"});
  Expect(staged.success() && staged.output == "line1",
         "staged blob must be exactly 'line1' with no added trailing newline");
}

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
  const auto patch =
      GenerateComparePatchForRows(model, "tracked.txt", 0, model.rows.size() - 1);
  Expect(patch.has_value(), "whole-file copy patch should be generated");
  Expect(patch->find("@@ hunk ") == std::string::npos,
         "copied patch must not use the removed exporter's fake '@@ hunk N @@' headers");
  Expect(patch->find("--- a/tracked.txt") != std::string::npos,
         "copied patch must carry a real --- header");
  Expect(patch->find("+++ b/tracked.txt") != std::string::npos,
         "copied patch must carry a real +++ header");
  const auto header = ParsePatchHunkHeader(*patch);
  Expect(header.has_value() && header->explicit_counts,
         "copied patch must carry a real unified @@ range header");

  PatchApplyRequest request{
      .operation = PatchOperationKind::StageHunk,
      .target{.repository_root = repo_path,
              .relative_path = std::filesystem::path("tracked.txt"),
              .hunk = std::nullopt,
              .line_selection = std::nullopt},
  };
  const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
  Expect(result.category == PatchApplyResultCategory::Success,
         "copied whole-file patch must be git-apply-able");
}

void TestCopyPatchAppliesForAddDeleteSpacesAndNoNewline() {
  const auto make_repo = [](const TemporaryDirectory& temp) {
    const auto repo_path = temp.path() / "repo";
    std::filesystem::create_directories(repo_path);
    InitializeGitRepo(repo_path);
    WriteFile(repo_path / "seed.txt", "seed\n");
    CommitAll(repo_path, "base", "base");
    return repo_path;
  };

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
    Expect(ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch).category == PatchApplyResultCategory::Success,
           "copied add patch must apply");
  }

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
    Expect(ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch).category == PatchApplyResultCategory::Success,
           "copied delete patch must apply");
  }

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
    Expect(ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch).category == PatchApplyResultCategory::Success,
           "copied spaces-path patch must apply");
  }

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
    Expect(ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch).category == PatchApplyResultCategory::Success,
           "copied no-newline patch must apply");
  }
}

void TestPatchHeaderQuotesUnsafePaths() {
  const CompareModel model = BuildCompareModel("x\n", "y\n");
  struct Case {
    std::filesystem::path path;
    std::string expected_minus_header;
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
    if (test_case.expected_minus_header.rfind("--- \"", 0) == 0) {
      Expect(patch->find("diff --git \"a/") != std::string::npos,
             "quoted path must also quote the diff --git a/ token");
    }
  }
}

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
      compare_tab, PatchOperationKind::StageHunk, false);
  Expect(request.has_value(), "request should build for a working-tree text hunk");
  Expect(request->target.hunk.has_value(), "request should target a hunk");
  Expect(request->model.rows.empty(),
         "request must not deep-copy the compare model (cost bounded by hunk, not file)");
  Expect(compare_tab.model.rows.size() > 400, "source model must remain intact");

  executor.Shutdown();
}

void TestPatchLineScopeSelectionExcludesExclusiveEndLine() {
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

  const std::string left = "a\nb\nc\nd\n";
  const std::string right = "a\nX\nY\nd\n";
  microide::workspace::CompareTabState compare_tab;
  compare_tab.path = repo_path / "file.txt";
  compare_tab.right_ref = "WORKTREE";
  compare_tab.model = BuildCompareModel(left, right);
  compare_tab.right_view_active = true;

  const auto right_path = repo_path / "right.txt";
  WriteFile(right_path, right);
  Expect(compare_tab.right_viewport.OpenFile(right_path), "right viewport should open");

  compare_tab.right_viewport.MoveCursorTo(1, 0, false);
  compare_tab.right_viewport.MoveCursorTo(2, 0, true);
  const auto selection = compare_tab.right_viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == 1 && selection->end.line == 2 &&
             selection->end.column == 0,
         "fixture selection must be the exclusive-end whole-line gesture");

  const std::size_t row_line1 =
      microide::workspace::CompareTabModelRowForRightLine(compare_tab, 1);
  const std::size_t row_line2 =
      microide::workspace::CompareTabModelRowForRightLine(compare_tab, 2);
  Expect(row_line1 != row_line2, "fixture must map the two right lines to different model rows");

  const auto request = service.BuildRequestForTesting(
      compare_tab, PatchOperationKind::DiscardSelectedLines, true);
  Expect(request.has_value(), "line-scope discard request should build");
  Expect(request->target.line_selection.has_value(), "request should carry a line selection");
  Expect(request->target.line_selection->first_model_row == row_line1,
         "selection must start at the anchored line's model row");
  Expect(request->target.line_selection->last_model_row == row_line1,
         "selection must NOT include the exclusive end line (line 2 / Y)");

  executor.Shutdown();
}

void TestPatchGeneratorRandomEditsStageThroughGit() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "f.txt";
  GitRepository repo(repo_path, microide::platform::LocalProcessLauncher());

  static constexpr const char* kVocabulary[] = {"alpha", "beta", "gamma", "", "alpha", "  x",
                                                "}", "{", "delta", "eps"};
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  const auto join = [](const std::vector<std::string>& lines, bool final_newline) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      text += lines[i];
      if (i + 1 < lines.size() || final_newline) {
        text += '\n';
      }
    }
    return text;
  };

  int trials_with_hunks = 0;
  for (int trial = 0; trial < 80; ++trial) {
    std::vector<std::string> base_lines;
    const std::size_t base_count = 1 + next(10);
    for (std::size_t i = 0; i < base_count; ++i) {
      base_lines.push_back(kVocabulary[next(std::size(kVocabulary))]);
    }
    std::vector<std::string> edited = base_lines;
    const std::size_t edits = 1 + next(4);
    for (std::size_t e = 0; e < edits; ++e) {
      const std::size_t kind = next(3);
      if (kind == 0 || edited.empty()) {
        edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(next(edited.size() + 1)),
                      kVocabulary[next(std::size(kVocabulary))]);
      } else if (kind == 1) {
        edited.erase(edited.begin() + static_cast<std::ptrdiff_t>(next(edited.size())));
      } else {
        edited[next(edited.size())] = kVocabulary[next(std::size(kVocabulary))];
      }
    }
    const bool base_newline = next(4) != 0;
    const bool edited_newline = next(4) != 0;
    const std::string base = join(base_lines, base_newline);
    const std::string modified = join(edited, edited_newline);
    if (base == modified) {
      continue;
    }

    WriteFile(file_path, base);
    CommitAll(repo_path, "base", "base");
    WriteFile(file_path, modified);
    microide::compare::CompareBuildOptions both_exist;
    both_exist.left_exists = true;
    both_exist.right_exists = true;
    const CompareModel model = BuildCompareModel(base, modified, both_exist);
    if (model.hunks.empty()) {
      continue;
    }
    ++trials_with_hunks;
    const std::string label = "trial " + std::to_string(trial) + " base=[" + base +
                              "] modified=[" + modified + "]";

    for (std::size_t hunk = 0; hunk < model.hunks.size(); ++hunk) {
      Expect(repo.Execute({"reset", "-q"}).success(), "index reset");
      const auto patch = GenerateComparePatch(model, "f.txt", static_cast<int>(hunk));
      Expect(patch.has_value(), ("a patch is generated for every hunk: " + label).c_str());
      if (!patch.has_value()) {
        continue;
      }
      PatchApplyRequest request{
          .operation = PatchOperationKind::StageHunk,
          .target{.repository_root = repo_path,
                  .relative_path = std::filesystem::path("f.txt"),
                  .hunk = std::nullopt,
                  .line_selection = std::nullopt},
          .model = model,
      };
      const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, *patch);
      Expect(result.category == PatchApplyResultCategory::Success,
             ("hunk " + std::to_string(hunk) + " stages alone: " + label + " patch=[" + *patch +
              "] error=[" + result.detail + "]")
                 .c_str());
    }

    Expect(repo.Execute({"reset", "-q"}).success(), "index reset");
    std::vector<std::size_t> order(model.hunks.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    if (next(2) == 1) {
      std::reverse(order.begin(), order.end());
    }
    for (const std::size_t hunk : order) {
      const auto& h = model.hunks[hunk];
      PatchApplyRequest request{
          .operation = PatchOperationKind::StageHunk,
          .target{.repository_root = repo_path,
                  .relative_path = std::filesystem::path("f.txt"),
                  .hunk = microide::project::PatchHunkTarget{.hunk_index = static_cast<int>(hunk)},
                  .line_selection = std::nullopt,
                  .change_span = microide::project::ChangeSpanForRows(
                      model, static_cast<std::size_t>(std::max(0, h.start_row)),
                      static_cast<std::size_t>(std::max(0, h.end_row))),
                  .working_tree_source = model.right_source},
      };
      const auto result = ApplyPatchRequest(microide::platform::LocalProcessLauncher(), request, "unused");
      Expect(result.category == PatchApplyResultCategory::Success,
             ("hunk " + std::to_string(hunk) + " stages after the hunks before it: " + label +
              " error=[" + result.detail + "]")
                 .c_str());
    }
    const auto staged = repo.Execute({"show", ":f.txt"});
    Expect(staged.success() && staged.output == modified,
           ("staging every hunk leaves the index equal to the edited file: " + label +
            " index=[" + staged.output + "]")
               .c_str());
    Expect(repo.Execute({"reset", "-q"}).success(), "index reset");
  }
  Expect(trials_with_hunks >= 60, "the generator produced enough distinct edits to mean anything");
}

void RegisterPatchApplyTests(std::vector<TestCase>& tests) {
  tests.push_back({"PatchApply/HeaderOracle", TestPatchHunkHeaderOracle});
  tests.push_back({"PatchApply/AppendAfterMissingFinalNewline",
                   TestPatchStageAppendAfterMissingFinalNewline});
  tests.push_back({"PatchApply/DeleteTrailingLeavesMissingFinalNewline",
                   TestPatchStageDeleteTrailingLeavesMissingFinalNewline});
  tests.push_back({"PatchApply/UnifiedDiff", TestPatchGeneratorProducesUnifiedDiff});
  tests.push_back({"PatchApply/GeneratorByteBudget", TestPatchGeneratorEnforcesByteBudget});
  tests.push_back({"PatchApply/SelectedLines", TestPatchGeneratorSelectedLinesIncludeContext});
  tests.push_back({"PatchApply/StageHunk", TestPatchStageHunkInRepository});
  tests.push_back({"PatchApply/RandomEditsStageThroughGit",
                   TestPatchGeneratorRandomEditsStageThroughGit});
  tests.push_back({"PatchApply/OneSidedPhantomTrailingLine",
                   TestPatchGeneratorOneSidedPhantomTrailingLineApplies});
  tests.push_back({"PatchApply/NonTerminalHunkNoNewlineMarker",
                   TestPatchGeneratorNonTerminalHunkNoNewlineMarkerApplies});
  tests.push_back({"PatchApply/FailureCategory", TestPatchFailureCategoryForAMissingRepository});
  tests.push_back({"PatchApply/LineSelection", TestPatchLineSelectionHelpers});
  tests.push_back({"PatchApply/Utf8Paths", TestPatchGeneratorUtf8Paths});
  tests.push_back({"PatchApply/AlignmentRows", TestPatchGeneratorIgnoresSpuriousAlignmentRows});
  tests.push_back({"PatchApply/BlankContextLines", TestPatchGeneratorPreservesBlankContextLines});
  tests.push_back({"PatchApply/RepeatedContextLines",
                   TestPatchGeneratorPreservesRepeatedContextLines});
  tests.push_back({"PatchApply/SelectedLinesBlankContext",
                   TestPatchGeneratorSelectedLinesBesideBlankContext});
  tests.push_back({"PatchApply/CrlfContextLines", TestPatchGeneratorCrlfContextLines});
  tests.push_back({"PatchApply/StageCrlfFileApplies", TestPatchStageCrlfFileApplies});
  tests.push_back({"PatchApply/FinishingInTheBackgroundStaysWithItsProject",
                   TestPatchApplyFinishingInTheBackgroundStaysWithItsProject});
  tests.push_back({"PatchApply/RejectsIgnoreWhitespace", TestPatchApplyRejectsIgnoreWhitespace});
  tests.push_back({"PatchApply/UnstageMixedIndexWorktree",
                   TestPatchUnstageSelectedLinesWithStagedAndUnstagedChanges});
  tests.push_back({"PatchApply/StagePureInsertionNoContextApplies",
                   TestPatchStagePureInsertionWithNoContextApplies});
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
  tests.push_back({"PatchApply/ClassifyGitApplyFailureSeparatesStaleFromCorrupt",
                   TestClassifyGitApplyFailureSeparatesStaleFromCorrupt});
  tests.push_back({"PatchApply/HeaderQuotesUnsafePaths", TestPatchHeaderQuotesUnsafePaths});
  tests.push_back({"PatchApply/RequestDoesNotCopyModel", TestPatchApplyRequestDoesNotCopyModel});
  tests.push_back({"PatchApply/LineScopeExcludesExclusiveEndLine",
                   TestPatchLineScopeSelectionExcludesExclusiveEndLine});
  tests.push_back({"PatchApply/GeneratedPatchesApplyWithRealGit",
                   TestGeneratedPatchesApplyWithRealGit});
}

}
