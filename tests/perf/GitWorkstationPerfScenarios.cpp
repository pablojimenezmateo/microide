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
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests::perf {
namespace {

using TA = microide::workspace::WorkspaceShell::TestAccess;

// 1 000 modified tracked paths stress status parsing/grouping without a multi-minute open tax.
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
  if (compare.model.hunks.size() < 1) {
    throw std::runtime_error("diff_next_hunk_large_file: expected compare hunks");
  }
  context.Measure("diff.next_hunk_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.JumpCompareHunk(1);
      context.PumpFrames(1);
    }
  });
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
  // Close any merge tab a previous iteration left open BEFORE the measured
  // window. The driver is reused across iterations, so without this the `merge`
  // command re-shows the already-open tab and every iteration after the first
  // measures a re-show -- the p50 then describes an operation this phase is not
  // named for, and a baseline of 185 allocations for "open a large merge" is what
  // that produced (TD-2026-08-12-190).
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
    // `current += base` — appending the whole accumulated base once per block —
    // made this file the quadratic sum of every prefix: 3.3 MB of repeated text
    // against a 24 KB base, so the scenario measured a 140x-lopsided diff rather
    // than the three-way merge it is named for. Give `current` its own per-block
    // variant, the way `incoming` has one, so every block is a genuine
    // base/incoming/current conflict.
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
  PrimeGitWorkstationFixture(context, kLargeDiffFixture, "external_change_refresh_open_diff");
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
  PrimeGitWorkstationFixture(context, kManyConflictsFixture, "external_change_refresh_open_merge");
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
// Not the macro: same warmup story as the two merge scenarios. Allocations settle
// by iteration 2 (87,251 then 86,15x flat), RSS growth by iteration 4
// (12.7 MB / 3.7 / 2.0 / 2.6 / then zero apart from occasional sub-MB arena
// top-ups). Without a warmup the ramp dominates `p50_rss_growth_bytes`.
const ScenarioRegistration g_perf_git_workstation_diff_next_hunk_large_file({
    .name = "diff_next_hunk_large_file",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 5,
    .run = RunDiffNextHunkLargeFile,
});
// Not the macro: these three stage into the same large patch and share a resident
// shape no envelope at the default 25% can hold. Their growth alternates cleanly —
// zero on odd iterations, ~200-400 KB on even ones — and the size of the growing
// half moves run to run: diff_stage_hunk_large_patch measured a trimmed mean of
// 174 / 157 / 227 KB across three full runs of one unchanged binary (1.45x), the
// other two 1.23x and 1.15x.
//
// 60% was not enough, and TD-2026-08-06-150 chased down why rather than widening
// again. `mean_rss_growth_bytes` for these is not a property of the scenario at
// all — it is a property of WHAT RAN BEFORE IT IN THE PROCESS. One unchanged
// binary, diff_stage_selected_lines, trimmed mean per iteration:
//
//   solo run                              33 / 37 / 61 KB
//   after a 26-scenario prefix            79 KB
//   after a 24-scenario prefix            265 KB
//   after the full 52-scenario prefix     239 KB
//   full suite (TD-2026-08-06-147)        174 / 273 KB
//
// An 8x range, and the app's actual retention is IDENTICAL across every one of
// them: `p50_net_heap_bytes` reads exactly 28,470 in all of them, as it does for
// 52 of 52 scenarios re-measured under three different prefixes (worst spread 9
// bytes). The 8x is glibc failing to trim a heap that 61,761 allocations per
// iteration have fragmented differently depending on who fragmented it first.
//
// So the split is: `p50_net_heap_bytes` is the gate that a retention regression
// trips, byte-exactly and prefix-independently; this one stays as an
// order-of-magnitude backstop for growth that never went through `operator new`
// (mmap, SDL, Lua, PCRE2, libc) and is invisible to the byte-exact gate. 150%
// covers the 1.56x same-prefix draw with margin. It deliberately does NOT try to
// cover the 8x cross-prefix swing — no envelope should, and a suite edit that
// re-levels these is expected to go red and be rebaselined, not absorbed.
//
// Widened HERE and not in the JSON on purpose: --update-baseline rewrites every
// tolerance in a baseline from this struct, so a hand-edit to the file survives
// exactly until the next rebaseline (TD-2026-08-05-136).
const ScenarioRegistration g_perf_git_workstation_diff_stage_hunk_large_patch({
    .name = "diff_stage_hunk_large_patch",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .run = RunDiffStageHunkLargePatch,
});
const ScenarioRegistration g_perf_git_workstation_diff_stage_selected_lines({
    .name = "diff_stage_selected_lines",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .run = RunDiffStageSelectedLines,
});
// Not the macro: `measurement_revision` moved when the scenario started closing
// its merge tab between iterations, so every iteration measures an OPEN rather
// than a re-show (TD-2026-08-12-190).
const ScenarioRegistration g_perf_git_workstation_merge_open_many_conflicts({
    .name = "merge_open_many_conflicts",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .measurement_revision = 2,
    .run = RunMergeOpenManyConflicts,
});
// Not the macro: this one needs a LONG warmup, and it is the reason to look at a
// scenario's per-iteration series rather than only its summary. Re-opening this
// merge settles over about seven iterations -- allocations 39,069 / 11,27x (x5) /
// 9,590 / 7,779-and-flat, RSS growth 9.3 MB / 7.3 / 7.3 / 7.8 / 5.0 / 2.2 / 0.8 /
// 0.02 / 0-and-flat -- as the allocator reaches the arena size this merge's peak
// footprint needs. It is a warmup curve, not a leak: growth stops dead once it
// gets there.
//
// Without a warmup the gate's verdict depends on the iteration count it happened
// to be run with: `p50_rss_growth_bytes` read 1.5 MB at 8 iterations (FAIL
// against a 64 KB floor) and 0 at 20 (PASS), from the same binary. A gate that
// answers differently depending on how long you ran it is not gating anything.
const ScenarioRegistration g_perf_git_workstation_merge_next_conflict_large_file({
    .name = "merge_next_conflict_large_file",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 8,
    .run = RunMergeNextConflictLargeFile,
});
REGISTER_GIT_WORKSTATION_SCENARIO(merge_accept_hunk_interleaved, RunMergeAcceptHunkInterleaved);
// Not the macro: this one needs a LONG warmup. Iteration 0 opens the merge and
// builds the model (36,490 allocations against a 18,3xx steady state), so without
// one it alone governs p95/max and the gate tracks which iteration the cold pass
// landed on rather than the tail of the measured work.
//
// One is not enough, though, and the series says so: allocations sit at ~18,3xx
// for eleven iterations and then STEP to ~15,49x and stay -- a discrete
// transition, the shape of caches reaching capacity and starting to reuse rather
// than a gradual settling. RSS growth tracks it exactly: ~3.9 MB per iteration
// through iteration 8, then 942 KB / 627 KB / 4 KB, then flat zero from iteration
// 11 on. Growth stops dead there, so it is a warmup curve and not a leak, but at
// one warmup the `p50_rss_growth_bytes` gate was reading the ramp: 1.7 MB against
// a 766 KB baseline, and identically so on the pre-rewrite binary.
const ScenarioRegistration g_perf_git_workstation_merge_edit_result_then_scroll({
    .name = "merge_edit_result_then_scroll",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .warmup_iterations = 12,
    .run = RunMergeEditResultThenScroll,
});
REGISTER_GIT_WORKSTATION_SCENARIO(commit_open_with_large_staged_set, RunCommitOpenWithLargeStagedSet);
// The third of the same family; see diff_stage_hunk_large_patch above.
const ScenarioRegistration g_perf_git_workstation_external_change_refresh_open_diff({
    .name = "external_change_refresh_open_diff",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_rss_percent = 150.0,
    .run = RunExternalChangeRefreshOpenDiff,
});
REGISTER_GIT_WORKSTATION_SCENARIO(external_change_refresh_open_merge,
                                 RunExternalChangeRefreshOpenMerge);

#undef REGISTER_GIT_WORKSTATION_SCENARIO

}  // namespace
}  // namespace microide::tests::perf
