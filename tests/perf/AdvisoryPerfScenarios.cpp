#include "perf/PerfHarness.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "platform/Subprocess.h"
#include "workspace/WorkspaceShellTestAccess.h"

namespace microide::tests::perf {
namespace {

// Steady-state RSS budget for the large-project open scenario.
// Calibrated against an observed reference of ~180 MiB on the perf-runner-v1
// class (SDL3 + SDL3_ttf + Lua + PCRE2 + file index for the 1k-file fixture).
// The budget is set at 256 MiB to allow normal day-to-day variance (font atlas
// growth, allocator slack, GLIBC arena placement) while still catching a real
// regression of ~40% or more over reference. Tighten only after running the
// gated scenario on perf-runner-v1 with the new floor.
constexpr std::uint64_t kRepoOpenIdleRssBudgetBytes = 256ULL * 1024ULL * 1024ULL;

struct ProcessSample {
  std::uint64_t rss_bytes = 0;
};

ProcessSample ReadProcessSample() {
#if defined(__linux__)
  ProcessSample sample{};
  std::ifstream statm("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t rss_pages = 0;
  statm >> total_pages >> rss_pages;
  if (!statm) {
    throw std::runtime_error("failed to read /proc/self/statm");
  }
  const long page_size = sysconf(_SC_PAGESIZE);
  sample.rss_bytes = rss_pages * static_cast<std::uint64_t>(page_size > 0 ? page_size : 4096);
  return sample;
#else
  return {};
#endif
}

std::string ReadFileTextOrThrow(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void WriteFileTextOrThrow(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  output << content;
  if (!output.good()) {
    throw std::runtime_error("failed to flush file: " + path.string());
  }
}

class ScopedTempTree {
 public:
  explicit ScopedTempTree(std::filesystem::path root) : root_(std::move(root)) {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_, error);
    if (error) {
      throw std::runtime_error("failed to create temp tree: " + root_.string());
    }
  }

  ~ScopedTempTree() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
 std::filesystem::path root_;
};

void RequireGitCommandSuccess(const std::filesystem::path& repo_path,
                              const std::vector<std::string>& args,
                              std::string_view context) {
  platform::SubprocessOptions options;
  options.cwd = repo_path;
  options.capture_stdout = true;
  options.capture_stderr = true;

  std::vector<std::string> command;
  command.reserve(args.size() + 1);
  command.emplace_back("git");
  command.insert(command.end(), args.begin(), args.end());
  const platform::SubprocessResult result = platform::RunSubprocess(command, options);
  if (result.exit_code == 0) {
    return;
  }
  throw std::runtime_error(std::string(context) + ": git command failed");
}

void InitializeGitRepo(const std::filesystem::path& repo_path) {
  RequireGitCommandSuccess(repo_path, {"-c", "init.defaultBranch=main", "init", "."}, "git init");
  RequireGitCommandSuccess(repo_path, {"config", "user.name", "Microide Perf"},
                           "git config user.name");
  RequireGitCommandSuccess(repo_path, {"config", "user.email", "microide-perf@example.com"},
                           "git config user.email");
}

void CommitAll(const std::filesystem::path& repo_path, std::string_view message) {
  RequireGitCommandSuccess(repo_path, {"add", "."}, "git add");
  RequireGitCommandSuccess(repo_path, {"commit", "-m", std::string(message)}, "git commit");
}

std::string BuildInterleavedBaseText(int block_count) {
  std::string text;
  text.reserve(static_cast<std::size_t>(block_count) * 96);
  for (int i = 0; i < block_count; ++i) {
    text += "void unit_" + std::to_string(i) + "() {\n";
    text += "  int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    text += "  sink(value_" + std::to_string(i) + ");\n";
    text += "}\n\n";
  }
  return text;
}

std::string BuildInterleavedVariantText(int block_count, int side) {
  std::string text;
  text.reserve(static_cast<std::size_t>(block_count) * 104);
  for (int i = 0; i < block_count; ++i) {
    text += "void unit_" + std::to_string(i) + "() {\n";
    if (i % 3 == side) {
      text += "  int value_" + std::to_string(i) + " = " + std::to_string(i + 1000 + side) + ";\n";
    } else {
      text += "  int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    text += "  sink(value_" + std::to_string(i) + ");\n";
    text += "}\n\n";
  }
  return text;
}

void RunRepoOpenRssIdle(ScenarioContext& context) {
  const std::filesystem::path project = "tests/perf/fixtures/large_project";
  ProcessSample rss_after_open{};
  ProcessSample rss_after_idle{};
  context.Measure("repo_open.open_and_first_frames", [&] {
    if (!context.Open(project)) {
      throw std::runtime_error("repo_open_rss_idle: failed to open large project fixture");
    }
    context.PumpFrames(5);
    rss_after_open = ReadProcessSample();
  });
  context.Measure("repo_open.idle_500ms", [&] {
    (void)context.Wait(std::chrono::milliseconds(500));
    rss_after_idle = ReadProcessSample();
  });
  if (rss_after_open.rss_bytes > 0 || rss_after_idle.rss_bytes > 0) {
    constexpr double kMib = 1024.0 * 1024.0;
    std::cerr << "repo_open_rss_idle: rss_after_open=" << rss_after_open.rss_bytes
              << " (" << (static_cast<double>(rss_after_open.rss_bytes) / kMib) << " MiB)"
              << " rss_after_idle=" << rss_after_idle.rss_bytes
              << " (" << (static_cast<double>(rss_after_idle.rss_bytes) / kMib) << " MiB)"
              << " budget_bytes=" << kRepoOpenIdleRssBudgetBytes
              << " (" << (static_cast<double>(kRepoOpenIdleRssBudgetBytes) / kMib) << " MiB)\n";
  }
  if (rss_after_idle.rss_bytes > kRepoOpenIdleRssBudgetBytes) {
    constexpr double kMib = 1024.0 * 1024.0;
    const double actual_mib = static_cast<double>(rss_after_idle.rss_bytes) / kMib;
    const double budget_mib = static_cast<double>(kRepoOpenIdleRssBudgetBytes) / kMib;
    throw std::runtime_error(
        "repo_open_rss_idle: steady-state RSS " + std::to_string(actual_mib) +
        " MiB exceeded budget " + std::to_string(budget_mib) + " MiB");
  }
}

void RunLargeFileOpenFirstPaint(ScenarioContext& context) {
  const std::filesystem::path file = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!std::filesystem::exists(file)) {
    std::cerr << "large_file_open_first_paint: missing fixture " << file << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.Measure("large_file.open_to_first_paint", [&] {
    context.OpenTab(file);
    context.PumpFrames(2);
  });
  if (context.ActiveViewport().lines().size() < 1000) {
    throw std::runtime_error("large_file_open_first_paint: large fixture did not load as expected");
  }
}

// Companion to large_file_open_first_paint, but opens an LF-only 50k-line file.
// large_file_open_first_paint's fixture has mixed line endings and therefore
// takes the DecodeLines normalization path; this scenario exercises the Phase 4
// direct-load fast path (no '\r' -> bytes handed straight to the piece tree's
// original buffer, skipping the split-into-vector<string> + rejoin round-trip).
// It is the open-time gate for the large-file load fast path.
void RunLargeFileOpenLfFirstPaint(ScenarioContext& context) {
  const std::filesystem::path file =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(file)) {
    std::cerr << "large_file_open_lf_first_paint: missing fixture " << file << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.Measure("large_file.open_lf_to_first_paint", [&] {
    context.OpenTab(file);
    context.PumpFrames(2);
  });
  if (context.ActiveViewport().lines().size() < 1000) {
    throw std::runtime_error(
        "large_file_open_lf_first_paint: large fixture did not load as expected");
  }
}

void RunMergeScrollLargeFixture(ScenarioContext& context) {
  const std::filesystem::path seed = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!std::filesystem::exists(seed)) {
    std::cerr << "merge_scroll_large_fixture: missing fixture " << seed << "\n";
    return;
  }

  const std::string source = ReadFileTextOrThrow(seed);
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-merge-scroll";
  ScopedTempTree temp_tree(temp_root);
  WriteFileTextOrThrow(temp_tree.root() / "base.txt", source);
  WriteFileTextOrThrow(temp_tree.root() / "incoming.txt", source + "\nINCOMING_PERF_TAIL\n");
  WriteFileTextOrThrow(temp_tree.root() / "current.txt", source + "\nCURRENT_PERF_TAIL\n");

  if (!context.Open(temp_tree.root())) {
    throw std::runtime_error("merge_scroll_large_fixture: failed to open temp project root");
  }
  context.Measure("merge_large.open_to_first_paint", [&] {
    if (!context.ExecuteCommand("merge base.txt incoming.txt current.txt result.txt")) {
      throw std::runtime_error("merge_scroll_large_fixture: merge command failed");
    }
    context.PumpFrames(3);
  });
  context.Measure("merge_large.scroll_burst", [&] {
    for (int i = 0; i < 80; ++i) {
      context.Scroll((i & 1) == 0 ? -2 : 2);
      context.PumpFrames(1);
    }
  });
}

void RunCompareScrollLargeFixture(ScenarioContext& context) {
  const std::filesystem::path seed = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!std::filesystem::exists(seed)) {
    std::cerr << "compare_scroll_large_fixture: missing fixture " << seed << "\n";
    return;
  }

  const std::string source = ReadFileTextOrThrow(seed);
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-compare-scroll";
  ScopedTempTree repo_dir(temp_root);
  InitializeGitRepo(repo_dir.root());
  WriteFileTextOrThrow(repo_dir.root() / "large.txt", source);
  CommitAll(repo_dir.root(), "add large compare fixture");
  WriteFileTextOrThrow(repo_dir.root() / "large.txt", source + "\nWORKTREE_PERF_TAIL\n");

  if (!context.Open(repo_dir.root())) {
    throw std::runtime_error("compare_scroll_large_fixture: failed to open temp git repo");
  }
  context.Measure("compare_large.open_to_first_paint", [&] {
    if (!context.ExecuteCommand("compare large.txt HEAD")) {
      throw std::runtime_error("compare_scroll_large_fixture: compare command failed");
    }
    context.PumpFrames(3);
  });
  context.Measure("compare_large.scroll_burst", [&] {
    for (int i = 0; i < 80; ++i) {
      context.Scroll((i & 1) == 0 ? -2 : 2);
      context.PumpFrames(1);
    }
  });
}

void RunMergeScrollInterleavedHunks(ScenarioContext& context) {
  constexpr int kBlocks = 420;
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-merge-interleaved";
  ScopedTempTree temp_tree(temp_root);
  WriteFileTextOrThrow(temp_tree.root() / "base.cpp", BuildInterleavedBaseText(kBlocks));
  WriteFileTextOrThrow(temp_tree.root() / "incoming.cpp", BuildInterleavedVariantText(kBlocks, 0));
  WriteFileTextOrThrow(temp_tree.root() / "current.cpp", BuildInterleavedVariantText(kBlocks, 1));

  if (!context.Open(temp_tree.root())) {
    throw std::runtime_error("merge_scroll_interleaved_hunks: failed to open temp project root");
  }
  context.Measure("merge_interleaved.open_to_first_paint", [&] {
    if (!context.ExecuteCommand("merge base.cpp incoming.cpp current.cpp result.cpp")) {
      throw std::runtime_error("merge_scroll_interleaved_hunks: merge command failed");
    }
    context.PumpFrames(3);
  });
  const auto& merge = workspace::WorkspaceShell::TestAccess::ActiveMerge(context.Shell());
  if (merge.model.hunks.size() < 200) {
    throw std::runtime_error("merge_scroll_interleaved_hunks: expected hundreds of hunks");
  }
  context.Measure("merge_interleaved.scroll_burst", [&] {
    for (int i = 0; i < 96; ++i) {
      context.Scroll((i & 1) == 0 ? -3 : 3);
      context.PumpFrames(1);
    }
  });
}

void RunCompareScrollSelectionFixture(ScenarioContext& context) {
  constexpr int kBlocks = 420;
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-perf-compare-selection";
  ScopedTempTree repo_dir(temp_root);
  InitializeGitRepo(repo_dir.root());
  WriteFileTextOrThrow(repo_dir.root() / "large.cpp", BuildInterleavedBaseText(kBlocks));
  CommitAll(repo_dir.root(), "add interleaved compare fixture");
  WriteFileTextOrThrow(repo_dir.root() / "large.cpp", BuildInterleavedVariantText(kBlocks, 0));

  if (!context.Open(repo_dir.root())) {
    throw std::runtime_error("compare_scroll_selection_fixture: failed to open temp git repo");
  }
  const std::filesystem::path source = repo_dir.root() / "large.cpp";
  context.Measure("compare_selection.open_to_first_paint", [&] {
    if (!workspace::WorkspaceShell::TestAccess::OpenWorkingTreeComparison(
            context.Shell(), source, "HEAD", "HEAD")) {
      throw std::runtime_error("compare_scroll_selection_fixture: compare command failed");
    }
    context.PumpFrames(3);
  });
  auto& compare = workspace::WorkspaceShell::TestAccess::ActiveCompare(context.Shell());
  if (compare.model.hunks.size() < 120) {
    throw std::runtime_error("compare_scroll_selection_fixture: expected many compare hunks");
  }
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(8, 2, false);
  compare.right_viewport.MoveCursorTo(260, 24, true);
  context.Measure("compare_selection.scroll_burst", [&] {
    for (int i = 0; i < 96; ++i) {
      context.Scroll((i & 1) == 0 ? -3 : 3);
      context.PumpFrames(1);
    }
  });
}

const ScenarioRegistration g_perf_repo_open_rss_idle({Scenario{
    .name = "repo_open_rss_idle",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunRepoOpenRssIdle,
}});
const ScenarioRegistration g_perf_large_file_open_first_paint({Scenario{
    .name = "large_file_open_first_paint",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunLargeFileOpenFirstPaint,
}});
const ScenarioRegistration g_perf_large_file_open_lf_first_paint({Scenario{
    .name = "large_file_open_lf_first_paint",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunLargeFileOpenLfFirstPaint,
}});
const ScenarioRegistration g_perf_merge_scroll_large_fixture({Scenario{
    .name = "merge_scroll_large_fixture",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunMergeScrollLargeFixture,
}});
const ScenarioRegistration g_perf_compare_scroll_large_fixture({Scenario{
    .name = "compare_scroll_large_fixture",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunCompareScrollLargeFixture,
}});
const ScenarioRegistration g_perf_merge_scroll_interleaved_hunks({Scenario{
    .name = "merge_scroll_interleaved_hunks",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunMergeScrollInterleavedHunks,
}});
const ScenarioRegistration g_perf_compare_scroll_selection({Scenario{
    .name = "compare_scroll_selection",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunCompareScrollSelectionFixture,
}});

}  // namespace
}  // namespace microide::tests::perf
