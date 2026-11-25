#include "perf/PerfHarness.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "compare/MergeModel.h"
#include "platform/Subprocess.h"
#include "project/GitStatusService.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests::perf {
namespace {

using TA = microide::workspace::WorkspaceShell::TestAccess;

constexpr const char* kLargeStatusFixture = "tests/perf/fixtures/git_1000_changed_project";
constexpr const char* kManyUntrackedFixture = "tests/perf/fixtures/git_many_untracked_project";
constexpr const char* k1000ChangedFixture = "tests/perf/fixtures/git_1000_changed_project";
constexpr const char* kLargeDiffFixture = "tests/perf/fixtures/git_large_diff_project";
constexpr const char* kLargeStagedFixture = "tests/perf/fixtures/git_large_staged_project";
constexpr const char* kManyConflictsFixture = "tests/perf/fixtures/git_many_conflicts_project";

void PrimeGitWorkstationFixture(ScenarioContext& context, const std::filesystem::path& fixture,
                                const char* scenario_label) {
  if (!DirectoryExistsNoThrow(fixture)) {
    throw std::runtime_error(std::string(scenario_label) + ": missing fixture " + fixture.string());
  }
  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::absolute(fixture, error).lexically_normal();
  if (error || resolved.empty()) {
    throw std::runtime_error(std::string(scenario_label) + ": invalid fixture path");
  }
  TA::PerfPrimeGitRepository(context.Shell(), resolved);
  context.PumpFrames(2);
}

void PrimeGitWorkstationProject(ScenarioContext& context, const std::filesystem::path& fixture,
                                const char* scenario_label) {
  if (!DirectoryExistsNoThrow(fixture)) {
    throw std::runtime_error(std::string(scenario_label) + ": missing fixture " + fixture.string());
  }
  if (!context.Open(fixture)) {
    throw std::runtime_error(std::string(scenario_label) + ": failed to open fixture project");
  }
  context.PumpFrames(2);
  PrimeGitWorkstationFixture(context, fixture, scenario_label);
}

void PumpGitRefreshCycle(ScenarioContext& context, std::size_t min_entries) {
  TA::OnFramePresented(context.Shell());
  context.ShowGitSidebar();
  context.PumpFrames(2);
  context.Measure("git.refresh_dispatch", [&] {
    TA::PerfRunGitSidebarRefreshSync(context.Shell());
    if (TA::GitSidebarEntries(context.Shell()).size() < min_entries) {
      throw std::runtime_error("git sidebar refresh did not publish expected entries");
    }
  });
  context.PumpFrames(3);
}

void RunGitSidebarRefreshLargeRepo(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeStatusFixture, "git_sidebar_refresh_large_repo");
  PumpGitRefreshCycle(context, 500);
}

void RunGitSidebarRefreshManyUntracked(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kManyUntrackedFixture, "git_sidebar_refresh_many_untracked");
  PumpGitRefreshCycle(context, 1000);
}

void RunDiffOpen1000FileChanges(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, k1000ChangedFixture, "diff_open_1000_file_changes");
  context.ShowGitSidebar();
  context.PumpFrames(2);
  context.Measure("diff.open_first_changed_file", [&] {
    TA::PerfRunGitSidebarRefreshSync(context.Shell());
    const auto& entries = TA::GitSidebarEntries(context.Shell());
    if (entries.empty()) {
      throw std::runtime_error("diff_open_1000_file_changes: expected changed sidebar entries");
    }
    if (!TA::OpenWorkingTreeComparison(context.Shell(), entries.front().path, "HEAD", "HEAD")) {
      throw std::runtime_error("diff_open_1000_file_changes: compare open failed");
    }
    context.PumpFrames(4);
  });
}

void RunDiffNextHunkLargeFile(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeDiffFixture, "diff_next_hunk_large_file");
  const std::filesystem::path source =
      TA::ProjectRoot(context.Shell()) / "src" / "large.cpp";
  context.Measure("diff.open_large_compare", [&] {
    if (!TA::OpenWorkingTreeComparison(context.Shell(), source, "HEAD", "HEAD")) {
      throw std::runtime_error("diff_next_hunk_large_file: compare open failed");
    }
    context.PumpFrames(4);
  });
  auto& compare = TA::ActiveCompare(context.Shell());
  if (compare.model.hunks.size() < 2) {
    throw std::runtime_error("diff_next_hunk_large_file: expected a multi-hunk diff, got " +
                             std::to_string(compare.model.hunks.size()) + " hunk(s)");
  }
  for (int i = 0; i < 40; ++i) {
    context.JumpCompareHunk(-1);
  }
  const std::size_t selected_row_before = TA::ActiveCompare(context.Shell()).selected_row;
  const std::size_t first_hunk_row =
      workspace::CompareFirstChangePresentationRow(TA::ActiveCompare(context.Shell()));
  if (selected_row_before != first_hunk_row) {
    throw std::runtime_error(
        "diff_next_hunk_large_file: 40 previous-hunk jumps left the selection on row " +
        std::to_string(selected_row_before) + ", not the first hunk's row " +
        std::to_string(first_hunk_row) + "; hunk navigation is not reaching the compare tab");
  }
  context.Measure("diff.next_hunk_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.JumpCompareHunk(1);
      context.PumpFrames(1);
    }
  });
  const std::vector<compare::CompareHunk>& hunks = TA::ActiveCompare(context.Shell()).model.hunks;
  const std::size_t expected_hunk = std::min<std::size_t>(24, hunks.size() - 1);
  const auto expected_row =
      workspace::CompareTabPresentationRowForHunk(TA::ActiveCompare(context.Shell()),
                                                  hunks[expected_hunk].index);
  const std::size_t landed_row = TA::ActiveCompare(context.Shell()).selected_row;
  if (!expected_row.has_value() || landed_row != *expected_row) {
    throw std::runtime_error(
        "diff_next_hunk_large_file: 24 next-hunk jumps from hunk 0 landed on row " +
        std::to_string(landed_row) + ", not hunk " + std::to_string(expected_hunk) +
        "'s row " + (expected_row.has_value() ? std::to_string(*expected_row) : "<none>") +
        " (of " + std::to_string(hunks.size()) + " hunks, " +
        std::to_string(TA::ActiveCompare(context.Shell()).presentation.rows.size()) +
        " presentation rows); the burst did not navigate");
  }
}

void RunDiffStageHunkLargePatch(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeDiffFixture, "diff_stage_hunk_large_patch");
  context.PumpFrames(2);
  const std::filesystem::path source =
      TA::ProjectRoot(context.Shell()) / "src" / "large.cpp";
  context.Measure("diff.open_large_patch", [&] {
    if (!TA::OpenWorkingTreeComparison(context.Shell(), source, "HEAD", "HEAD")) {
      throw std::runtime_error("diff_stage_hunk_large_patch: compare open failed");
    }
    context.PumpFrames(4);
  });
  context.Measure("diff.stage_hunk", [&] {
    context.StageCompareHunk();
    TA::PerfRunGitSidebarRefreshSync(context.Shell());
    context.PumpFrames(2);
  });
}

void RunDiffStageSelectedLines(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeDiffFixture, "diff_stage_selected_lines");
  const std::filesystem::path source =
      TA::ProjectRoot(context.Shell()) / "src" / "large.cpp";
  if (!TA::OpenWorkingTreeComparison(context.Shell(), source, "HEAD", "HEAD")) {
    throw std::runtime_error("diff_stage_selected_lines: compare open failed");
  }
  context.PumpFrames(3);
  auto& compare = TA::ActiveCompare(context.Shell());
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(40, 0, false);
  compare.right_viewport.MoveCursorTo(80, 0, true);
  context.Measure("diff.stage_selected_lines", [&] {
    context.StageCompareSelectedLines();
    TA::PerfRunGitSidebarRefreshSync(context.Shell());
    context.PumpFrames(2);
  });
}

void RunMergeOpenManyConflicts(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kManyConflictsFixture, "merge_open_many_conflicts");
  context.PumpFrames(2);
  context.CloseActiveTab();
  context.Measure("merge.open_many_conflicts", [&] {
    if (!context.ExecuteCommand(
            "merge base.cpp incoming.cpp current.cpp result.cpp")) {
      throw std::runtime_error("merge_open_many_conflicts: merge command failed");
    }
    context.PumpFrames(4);
  });
  const auto& merge = TA::ActiveMerge(context.Shell());
  if (merge.model.hunks.size() < 100) {
    throw std::runtime_error("merge_open_many_conflicts: expected many merge hunks");
  }
}

void RunMergeNextConflictLargeFile(ScenarioContext& context) {
  constexpr int kBlocks = 420;
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-merge-next-conflict";
  std::error_code error;
  std::filesystem::remove_all(temp_root, error);
  std::filesystem::create_directories(temp_root, error);

  auto write = [&](const char* name, const std::string& text) {
    std::ofstream output(temp_root / name, std::ios::binary | std::ios::trunc);
    output << text;
  };

  std::string base;
  std::string incoming;
  std::string current;
  for (int i = 0; i < kBlocks; ++i) {
    base += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
            " = " + std::to_string(i) + ";\n  sink(value_" + std::to_string(i) + ");\n}\n\n";
    incoming += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
                " = " + std::to_string(i + 1000) + ";\n  sink(value_" + std::to_string(i) +
                ");\n}\n\n";
    current += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
               " = " + std::to_string(i + 2000) + ";\n  sink(value_" + std::to_string(i) +
               ");\n}\n\n";
  }
  write("base.cpp", base);
  write("incoming.cpp", incoming);
  write("current.cpp", current);

  if (!context.Open(temp_root)) {
    throw std::runtime_error("merge_next_conflict_large_file: failed to open temp project");
  }
  context.Measure("merge.open_interleaved", [&] {
    if (!context.ExecuteCommand("merge base.cpp incoming.cpp current.cpp result.cpp")) {
      throw std::runtime_error("merge_next_conflict_large_file: merge command failed");
    }
    context.PumpFrames(4);
  });
  const auto& merge = TA::ActiveMerge(context.Shell());
  if (merge.conflicts.size() < 200) {
    throw std::runtime_error("merge_next_conflict_large_file: expected hundreds of conflicts");
  }
  context.Measure("merge.next_conflict_burst", [&] {
    for (int i = 0; i < 64; ++i) {
      context.MoveMergeConflict(1);
    }
  });
}

void RunMergeAcceptHunkInterleaved(ScenarioContext& context) {
  constexpr int kBlocks = 320;
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-merge-accept-interleaved";
  std::error_code error;
  std::filesystem::remove_all(temp_root, error);
  std::filesystem::create_directories(temp_root, error);

  auto write = [&](const char* name, const std::string& text) {
    std::ofstream output(temp_root / name, std::ios::binary | std::ios::trunc);
    output << text;
  };

  std::string base;
  std::string incoming;
  std::string current;
  for (int i = 0; i < kBlocks; ++i) {
    base += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
            " = " + std::to_string(i) + ";\n  sink(value_" + std::to_string(i) + ");\n}\n\n";
    incoming += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
                " = " + std::to_string(i + (i % 3 == 0 ? 1000 : 0)) + ";\n  sink(value_" +
                std::to_string(i) + ");\n}\n\n";
    current += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
               " = " + std::to_string(i + (i % 3 == 1 ? 2000 : 0)) + ";\n  sink(value_" +
               std::to_string(i) + ");\n}\n\n";
  }
  write("base.cpp", base);
  write("incoming.cpp", incoming);
  write("current.cpp", current);

  if (!context.Open(temp_root)) {
    throw std::runtime_error("merge_accept_hunk_interleaved: failed to open temp project");
  }
  if (!context.ExecuteCommand("merge base.cpp incoming.cpp current.cpp result.cpp")) {
    throw std::runtime_error("merge_accept_hunk_interleaved: merge command failed");
  }
  context.PumpFrames(3);
  context.Measure("merge.accept_interleaved_burst", [&] {
    const compare::MergeChoice choices[] = {compare::MergeChoice::Incoming,
                                            compare::MergeChoice::Current,
                                            compare::MergeChoice::Both,
                                            compare::MergeChoice::Base};
    for (int i = 0; i < 96; ++i) {
      context.ApplyMergeChoice(choices[i % 4]);
      if ((i & 3) == 3) {
        context.MoveMergeConflict(1);
      }
    }
  });
}

void RunMergeEditResultThenScroll(ScenarioContext& context) {
  constexpr int kBlocks = 280;
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-merge-edit-scroll";
  std::error_code error;
  std::filesystem::remove_all(temp_root, error);
  std::filesystem::create_directories(temp_root, error);

  auto write = [&](const char* name, const std::string& text) {
    std::ofstream output(temp_root / name, std::ios::binary | std::ios::trunc);
    output << text;
  };

  std::string base;
  std::string incoming;
  std::string current;
  for (int i = 0; i < kBlocks; ++i) {
    base += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
            " = " + std::to_string(i) + ";\n  sink(value_" + std::to_string(i) + ");\n}\n\n";
    incoming += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
                " = " + std::to_string(i + 500) + ";\n  sink(value_" + std::to_string(i) +
                ");\n}\n\n";
    current += "void unit_" + std::to_string(i) + "() {\n  int value_" + std::to_string(i) +
               " = " + std::to_string(i + 900) + ";\n  sink(value_" + std::to_string(i) +
               ");\n}\n\n";
  }
  write("base.cpp", base);
  write("incoming.cpp", incoming);
  write("current.cpp", current);

  if (!context.Open(temp_root)) {
    throw std::runtime_error("merge_edit_result_then_scroll: failed to open temp project");
  }
  if (!context.ExecuteCommand("merge base.cpp incoming.cpp current.cpp result.cpp")) {
    throw std::runtime_error("merge_edit_result_then_scroll: merge command failed");
  }
  context.PumpFrames(3);
  for (int i = 0; i < 24; ++i) {
    context.ApplyMergeChoice(compare::MergeChoice::Incoming);
    context.MoveMergeConflict(1);
  }
  context.Measure("merge.edit_result_typing", [&] {
    context.Type("// perf merge edit\n");
    context.PumpFrames(8);
  });
  context.Measure("merge.edit_result_scroll", [&] {
    for (int i = 0; i < 64; ++i) {
      context.Scroll((i & 1) == 0 ? -3 : 3);
      context.PumpFrames(1);
    }
  });
}

void RunCompareTypeInWrappedDiff(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeDiffFixture, "compare_type_in_wrapped_diff");
  const std::filesystem::path source =
      TA::ProjectRoot(context.Shell()) / "src" / "large.cpp";
  if (!TA::OpenWorkingTreeComparison(context.Shell(), source, "HEAD", "HEAD")) {
    throw std::runtime_error("compare_type_in_wrapped_diff: compare open failed");
  }
  context.SetSetting("editor.wrap", "word");
  context.PumpFrames(4);

  auto& compare = TA::ActiveCompare(context.Shell());
  if (!compare.right_editable) {
    throw std::runtime_error(
        "compare_type_in_wrapped_diff: the working-tree pane is not editable, so this "
        "scenario would measure nothing");
  }
  if (!compare.wrap_layout.active()) {
    throw std::runtime_error(
        "compare_type_in_wrapped_diff: editor.wrap=word did not reach the compare surface");
  }
  const std::size_t model_rows = compare.model.rows.size();
  if (model_rows < 500) {
    throw std::runtime_error(
        "compare_type_in_wrapped_diff: expected a large diff, got " +
        std::to_string(model_rows) + " rows");
  }

  context.Measure("compare.type_in_wrapped_diff", [&] {
    for (int i = 0; i < 16; ++i) {
      context.Type("x");
      context.PumpFrames(1);
    }
  });
}

void RunCommitOpenWithLargeStagedSet(ScenarioContext& context) {
  PrimeGitWorkstationFixture(context, kLargeStagedFixture, "commit_open_with_large_staged_set");
  context.PumpFrames(2);
  context.Measure("commit.open_staged_sidebar", [&] {
    PumpGitRefreshCycle(context, 400);
    const auto& entries = TA::GitSidebarEntries(context.Shell());
    if (entries.size() < 400) {
      throw std::runtime_error(
          "commit_open_with_large_staged_set: expected a large staged sidebar set");
    }
  });
}

void RunExternalChangeRefreshOpenDiff(ScenarioContext& context) {
  PrimeGitWorkstationProject(context, kLargeDiffFixture, "external_change_refresh_open_diff");
  const std::filesystem::path source =
      TA::ProjectRoot(context.Shell()) / "src" / "large.cpp";
  if (!TA::OpenWorkingTreeComparison(context.Shell(), source, "HEAD", "HEAD")) {
    throw std::runtime_error("external_change_refresh_open_diff: compare open failed");
  }
  context.PumpFrames(3);
  const auto hunk_count_before = TA::ActiveCompare(context.Shell()).model.hunks.size();
  context.Measure("external.refresh_open_diff", [&] {
    context.SimulateExternalFileChange("src/large.cpp", "\n// external refresh diff\n");
    context.PumpFrames(6);
  });
  if (TA::ActiveCompare(context.Shell()).model.hunks.size() < hunk_count_before) {
    throw std::runtime_error("external_change_refresh_open_diff: compare model shrank unexpectedly");
  }
}

void RunExternalChangeRefreshOpenMerge(ScenarioContext& context) {
  PrimeGitWorkstationProject(context, kManyConflictsFixture, "external_change_refresh_open_merge");
  if (!context.ExecuteCommand("merge base.cpp incoming.cpp current.cpp result.cpp")) {
    throw std::runtime_error("external_change_refresh_open_merge: merge command failed");
  }
  context.PumpFrames(3);
  const auto conflict_count_before = TA::ActiveMerge(context.Shell()).conflicts.size();
  context.Measure("external.refresh_open_merge", [&] {
    context.SimulateExternalFileChange("current.cpp", "\n// external refresh merge\n");
    context.PumpFrames(6);
  });
  if (TA::ActiveMerge(context.Shell()).conflicts.size() != conflict_count_before) {
    throw std::runtime_error(
        "external_change_refresh_open_merge: conflict tracking changed unexpectedly");
  }
}

#define REGISTER_GIT_WORKSTATION_SCENARIO(NAME, RUNNER)                       \
  const ScenarioRegistration g_perf_git_workstation_##NAME({                  \
      .name = #NAME,                                                          \
      .smoke = false,                                                         \
      .baseline_gated = true,                                                 \
      .run_by_default = true,                                                 \
      .run = RUNNER,                                                          \
  })

REGISTER_GIT_WORKSTATION_SCENARIO(git_sidebar_refresh_large_repo, RunGitSidebarRefreshLargeRepo);
REGISTER_GIT_WORKSTATION_SCENARIO(git_sidebar_refresh_many_untracked,
                                 RunGitSidebarRefreshManyUntracked);
REGISTER_GIT_WORKSTATION_SCENARIO(diff_open_1000_file_changes, RunDiffOpen1000FileChanges);
const ScenarioRegistration g_perf_git_workstation_diff_next_hunk_large_file({
    .name = "diff_next_hunk_large_file",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 5,
    .measurement_revision = 3,
    .run = RunDiffNextHunkLargeFile,
});
const ScenarioRegistration g_perf_git_workstation_diff_stage_hunk_large_patch({
    .name = "diff_stage_hunk_large_patch",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .measurement_revision = 3,
    .run = RunDiffStageHunkLargePatch,
});
const ScenarioRegistration g_perf_git_workstation_diff_stage_selected_lines({
    .name = "diff_stage_selected_lines",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .measurement_revision = 3,
    .run = RunDiffStageSelectedLines,
});
const ScenarioRegistration g_perf_git_workstation_merge_open_many_conflicts({
    .name = "merge_open_many_conflicts",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .measurement_revision = 2,
    .run = RunMergeOpenManyConflicts,
});
const ScenarioRegistration g_perf_git_workstation_merge_next_conflict_large_file({
    .name = "merge_next_conflict_large_file",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 8,
    .run = RunMergeNextConflictLargeFile,
});
REGISTER_GIT_WORKSTATION_SCENARIO(merge_accept_hunk_interleaved, RunMergeAcceptHunkInterleaved);
const ScenarioRegistration g_perf_git_workstation_merge_edit_result_then_scroll({
    .name = "merge_edit_result_then_scroll",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 12,
    .run = RunMergeEditResultThenScroll,
});
const ScenarioRegistration g_perf_git_workstation_compare_type_in_wrapped_diff({
    .name = "compare_type_in_wrapped_diff",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .measurement_revision = 3,
    .run = RunCompareTypeInWrappedDiff,
});
REGISTER_GIT_WORKSTATION_SCENARIO(commit_open_with_large_staged_set, RunCommitOpenWithLargeStagedSet);
const ScenarioRegistration g_perf_git_workstation_external_change_refresh_open_diff({
    .name = "external_change_refresh_open_diff",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .measurement_revision = 3,
    .run = RunExternalChangeRefreshOpenDiff,
});
const ScenarioRegistration g_perf_git_workstation_external_change_refresh_open_merge({
    .name = "external_change_refresh_open_merge",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .measurement_revision = 2,
    .run = RunExternalChangeRefreshOpenMerge,
});

#undef REGISTER_GIT_WORKSTATION_SCENARIO

}
}
