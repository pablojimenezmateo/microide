#include "perf/PerfHarness.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "perf/AllocationCounter.h"
#include "compare/MergeModel.h"
#include "platform/Subprocess.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

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

// Growth this scenario's own open is allowed to add, enforced regardless of what
// else already ran in the process. Measured at ~90 MiB in a fresh process (153 MiB
// steady state from a ~63 MiB pre-open baseline); 160 MiB leaves the same kind of
// headroom the absolute budget does while still catching a real regression.
constexpr std::uint64_t kRepoOpenIdleRssGrowthBudgetBytes = 160ULL * 1024ULL * 1024ULL;

// RSS at scenario entry below which the process counts as fresh, so the absolute
// budget above is this scenario's to answer for. A full-suite run enters here well
// past this (measured 279 MiB after 48 other scenarios).
constexpr std::uint64_t kRepoOpenIdleFreshProcessRssBytes = 128ULL * 1024ULL * 1024ULL;

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

// Fixture trees for the compare/merge scenarios below, built ONCE per process and
// shared by every measured iteration.
//
// They used to be built inside the scenario body, so each iteration rewrote a
// 1 MiB seed file two or three times and — for the git-backed ones — ran `git
// init`, two `git config`s, `git add`, and `git commit`: five subprocess spawns on
// the scenario thread, all inside the measured window. On
// compare_scroll_large_fixture that setup was ~190 ms of the ~250 ms of
// main-thread time the scenario reported, so the scroll burst it exists to measure
// accounted for under a quarter of its own number, and a real regression in it
// would have been invisible under the setup noise.
//
// The scenarios only ever read these trees after building them (they scroll, they
// select, they type into an in-memory result buffer), so one build per process is
// sound. The trees are removed when the map's destructor runs at exit — the perf
// binary returns from main rather than quick_exit-ing, so that actually happens.
const std::filesystem::path& EnsureSharedFixtureTree(
    const std::string& name,
    const std::function<void(const std::filesystem::path&)>& build) {
  static std::map<std::string, ScopedTempTree> trees;
  const auto existing = trees.find(name);
  if (existing != trees.end()) {
    return existing->second.root();
  }
  const auto [inserted, ok] =
      trees.emplace(std::piecewise_construct, std::forward_as_tuple(name),
                    std::forward_as_tuple(std::filesystem::temp_directory_path() / name));
  build(inserted->second.root());
  return inserted->second.root();
}

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
  // RSS is a PROCESS number, and every scenario in a run shares one process. This
  // gate used to compare the absolute figure against the budget no matter what
  // else had already run, so in a full-suite run it was really asserting "the sum
  // of forty-eight unrelated scenarios' peak footprint is under 256 MiB" — it read
  // 279 MiB there against 153 MiB for the same scenario run on its own, and which
  // side of the line it landed on depended on the scenario order and the iteration
  // count. That is not a repo-open memory gate.
  //
  // The order-independent thing this scenario controls is the footprint its own
  // open adds, so that is what is always enforced. The absolute budget still means
  // something in a fresh process, so it is enforced too — but only when the
  // process is actually fresh, judged by the RSS already resident at entry.
  const ProcessSample rss_at_entry = ReadProcessSample();
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
  constexpr double kMib = 1024.0 * 1024.0;
  const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / kMib; };
  const std::uint64_t open_growth_bytes =
      rss_after_idle.rss_bytes > rss_at_entry.rss_bytes
          ? rss_after_idle.rss_bytes - rss_at_entry.rss_bytes
          : 0;
  // A fresh process is under the entry threshold; anything above it means other
  // scenarios already ran here and the absolute figure is not this scenario's.
  const bool process_is_fresh = rss_at_entry.rss_bytes <= kRepoOpenIdleFreshProcessRssBytes;
  if (rss_after_open.rss_bytes > 0 || rss_after_idle.rss_bytes > 0) {
    std::cerr << "repo_open_rss_idle: rss_at_entry=" << rss_at_entry.rss_bytes
              << " (" << mib(rss_at_entry.rss_bytes) << " MiB)"
              << " rss_after_open=" << rss_after_open.rss_bytes
              << " (" << mib(rss_after_open.rss_bytes) << " MiB)"
              << " rss_after_idle=" << rss_after_idle.rss_bytes
              << " (" << mib(rss_after_idle.rss_bytes) << " MiB)"
              << " open_growth=" << open_growth_bytes
              << " (" << mib(open_growth_bytes) << " MiB)"
              << " growth_budget=" << mib(kRepoOpenIdleRssGrowthBudgetBytes) << " MiB"
              << " absolute_budget=" << mib(kRepoOpenIdleRssBudgetBytes) << " MiB"
              << (process_is_fresh ? " (fresh process: absolute budget enforced)"
                                   : " (shared process: absolute budget not this scenario's,"
                                     " growth budget enforced)")
              << "\n";
  }
  if (open_growth_bytes > kRepoOpenIdleRssGrowthBudgetBytes) {
    throw std::runtime_error(
        "repo_open_rss_idle: opening the large project grew RSS by " +
        std::to_string(mib(open_growth_bytes)) + " MiB, over the " +
        std::to_string(mib(kRepoOpenIdleRssGrowthBudgetBytes)) + " MiB growth budget");
  }
  if (process_is_fresh && rss_after_idle.rss_bytes > kRepoOpenIdleRssBudgetBytes) {
    throw std::runtime_error(
        "repo_open_rss_idle: steady-state RSS " + std::to_string(mib(rss_after_idle.rss_bytes)) +
        " MiB exceeded budget " + std::to_string(mib(kRepoOpenIdleRssBudgetBytes)) + " MiB");
  }
}

void RunLargeFileOpenFirstPaint(ScenarioContext& context) {
  const std::filesystem::path file = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!PathExistsNoThrow(file)) {
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
  if (!PathExistsNoThrow(file)) {
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

// Find-as-you-type over a large file. The fixture has the token "perfocc" on
// essentially every line, so the first keystroke matches tens of thousands of
// lines and each further keystroke narrows the set. The incremental refine path
// (WorkspaceShell::RefreshBufferSearch) filters the prior match set instead of
// rescanning the whole document, so total work across the typed query should stay
// roughly flat in document size rather than O(document) per keystroke.
void RunEditorBufferFindIncremental(ScenarioContext& context) {
  const std::filesystem::path file =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(file)) {
    std::cerr << "editor_buffer_find_incremental: missing fixture " << file << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(file);
  context.PumpFrames(2);
  if (context.ActiveViewport().lines().size() < 1000) {
    throw std::runtime_error(
        "editor_buffer_find_incremental: large fixture did not load as expected");
  }

  // Type the query one character at a time, refreshing matches each keystroke,
  // exactly as find-as-you-type does. "perfocc" appears on nearly every line.
  static constexpr std::string_view kQuery = "perfocc";
  context.Measure("buffer_find.find_as_you_type", [&] {
    std::string typed;
    for (const char ch : kQuery) {
      typed.push_back(ch);
      workspace::WorkspaceShell::TestAccess::SetBufferSearchQueryAndRefresh(context.Shell(), typed);
    }
  });
  if (workspace::WorkspaceShell::TestAccess::BufferSearchMatchCount(context.Shell()) < 1000) {
    throw std::runtime_error(
        "editor_buffer_find_incremental: expected many matches for the typed query");
  }
}

void RunMergeScrollLargeFixture(ScenarioContext& context) {
  const std::filesystem::path seed = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!PathExistsNoThrow(seed)) {
    std::cerr << "merge_scroll_large_fixture: missing fixture " << seed << "\n";
    return;
  }

  const std::filesystem::path root =
      EnsureSharedFixtureTree("microide-perf-merge-scroll", [&](const std::filesystem::path& dir) {
        const std::string source = ReadFileTextOrThrow(seed);
        WriteFileTextOrThrow(dir / "base.txt", source);
        WriteFileTextOrThrow(dir / "incoming.txt", source + "\nINCOMING_PERF_TAIL\n");
        WriteFileTextOrThrow(dir / "current.txt", source + "\nCURRENT_PERF_TAIL\n");
      });

  if (!context.Open(root)) {
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
  if (!PathExistsNoThrow(seed)) {
    std::cerr << "compare_scroll_large_fixture: missing fixture " << seed << "\n";
    return;
  }

  const std::filesystem::path root = EnsureSharedFixtureTree(
      "microide-perf-compare-scroll", [&](const std::filesystem::path& dir) {
        const std::string source = ReadFileTextOrThrow(seed);
        InitializeGitRepo(dir);
        WriteFileTextOrThrow(dir / "large.txt", source);
        CommitAll(dir, "add large compare fixture");
        WriteFileTextOrThrow(dir / "large.txt", source + "\nWORKTREE_PERF_TAIL\n");
      });

  if (!context.Open(root)) {
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

// `compare::BuildMergeModel` is one synchronous call on the shell path that opens
// a merge tab, and on a many-hunk merge it is tens of milliseconds — the largest
// single scope in any merge scenario. NOTHING gated it: every merge scenario
// shares one driver across its iterations and reuses the already-open tab, so the
// build lands on iteration 0 and is absorbed by the warmup. This scenario calls it
// directly, once per iteration, over the same interleaved fixture the scroll
// scenario uses (hundreds of hunks). It is a pure function of three strings, so
// the allocation count is exact.
void RunMergeModelBuildInterleaved(ScenarioContext& context) {
  constexpr int kBlocks = 420;
  const std::filesystem::path root = EnsureSharedFixtureTree(
      "microide-perf-merge-interleaved", [&](const std::filesystem::path& dir) {
        WriteFileTextOrThrow(dir / "base.cpp", BuildInterleavedBaseText(kBlocks));
        WriteFileTextOrThrow(dir / "incoming.cpp", BuildInterleavedVariantText(kBlocks, 0));
        WriteFileTextOrThrow(dir / "current.cpp", BuildInterleavedVariantText(kBlocks, 1));
      });
  // Read outside the measured window: this measures the model build, not file I/O.
  const std::string base = ReadFileTextOrThrow(root / "base.cpp");
  const std::string incoming = ReadFileTextOrThrow(root / "incoming.cpp");
  const std::string current = ReadFileTextOrThrow(root / "current.cpp");

  std::size_t hunks = 0;
  context.Measure("merge_model.build_interleaved", [&] {
    const compare::MergeModel model = compare::BuildMergeModel(base, incoming, current);
    hunks = model.hunks.size();
  });
  if (hunks < 200) {
    throw std::runtime_error("merge_model_build_interleaved: expected hundreds of hunks");
  }
}

void RunMergeScrollInterleavedHunks(ScenarioContext& context) {
  constexpr int kBlocks = 420;
  const std::filesystem::path root = EnsureSharedFixtureTree(
      "microide-perf-merge-interleaved", [&](const std::filesystem::path& dir) {
        WriteFileTextOrThrow(dir / "base.cpp", BuildInterleavedBaseText(kBlocks));
        WriteFileTextOrThrow(dir / "incoming.cpp", BuildInterleavedVariantText(kBlocks, 0));
        WriteFileTextOrThrow(dir / "current.cpp", BuildInterleavedVariantText(kBlocks, 1));
      });

  if (!context.Open(root)) {
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
  const std::filesystem::path root = EnsureSharedFixtureTree(
      "microide-perf-compare-selection", [&](const std::filesystem::path& dir) {
        InitializeGitRepo(dir);
        WriteFileTextOrThrow(dir / "large.cpp", BuildInterleavedBaseText(kBlocks));
        CommitAll(dir, "add interleaved compare fixture");
        WriteFileTextOrThrow(dir / "large.cpp", BuildInterleavedVariantText(kBlocks, 0));
      });

  if (!context.Open(root)) {
    throw std::runtime_error("compare_scroll_selection_fixture: failed to open temp git repo");
  }
  const std::filesystem::path source = root / "large.cpp";
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

// Advisory micro-benchmark for the ProjectTabStripVisible() settings-lookup cache.
// The predicate runs inside the uncached CurrentWorkspaceLayout()/ComputeLayout on
// the per-mouse-move window-drag hit-test path (WindowDragRegionContains). With a
// single project open, the size()>1 short-circuit fails and every recompute used to
// re-resolve "chrome.project_tabs.hide_when_single" through the GetSettingValue
// if-chain + settings-store lookup. This scenario hammers both the raw predicate and
// the full uncached layout recompute in the strip-visible and strip-hidden configs so
// a before/after run isolates the per-call lookup cost. Advisory only (not gated, not
// run by default) -- invoke with --scenario=project_tab_strip_layout_hittest.
void RunProjectTabStripLayoutHittest(ScenarioContext& context) {
  const std::filesystem::path project = "tests/perf/fixtures/large_project";
  if (!context.Open(project)) {
    throw std::runtime_error(
        "project_tab_strip_layout_hittest: failed to open project fixture");
  }
  context.PumpFrames(3);

  constexpr int kPredicateCalls = 200000;
  constexpr int kLayoutRecomputes = 20000;

  auto measure_config = [&](const char* config_label, const char* hide_value) {
    context.SetSetting("chrome.project_tabs.hide_when_single", hide_value);
    context.PumpFrames(1);
    // Warm the per-revision cache resolve so the measured loops observe steady state.
    volatile bool warm =
        workspace::WorkspaceShell::TestAccess::ProjectTabStripVisible(context.Shell());
    (void)warm;

    // Raw predicate burst -- isolates exactly the cached lookup.
    {
      const AllocationSnapshot before = Allocations::Snapshot();
      const auto start = std::chrono::steady_clock::now();
      volatile int sink = 0;
      for (int i = 0; i < kPredicateCalls; ++i) {
        sink += workspace::WorkspaceShell::TestAccess::ProjectTabStripVisible(context.Shell())
                    ? 1
                    : 0;
      }
      const auto end = std::chrono::steady_clock::now();
      const AllocationDelta delta = Allocations::DeltaSince(before);
      const double ms = std::chrono::duration<double, std::milli>(end - start).count();
      std::cerr << "[perf][project_tab_strip] predicate " << config_label
                << " calls=" << kPredicateCalls << " wall_ms=" << ms
                << " ns/call=" << (ms * 1e6 / kPredicateCalls)
                << " allocations=" << delta.allocations
                << " bytes=" << delta.bytes_allocated << "\n";
      (void)sink;
    }

    // Full uncached layout recompute burst -- the real WindowDragRegionContains path.
    {
      const AllocationSnapshot before = Allocations::Snapshot();
      const auto start = std::chrono::steady_clock::now();
      volatile float sink = 0.0F;
      for (int i = 0; i < kLayoutRecomputes; ++i) {
        const workspace::WorkspaceLayout layout =
            workspace::WorkspaceShell::TestAccess::CurrentLayout(context.Shell());
        sink += layout.project_tab_strip.h;
      }
      const auto end = std::chrono::steady_clock::now();
      const AllocationDelta delta = Allocations::DeltaSince(before);
      const double ms = std::chrono::duration<double, std::milli>(end - start).count();
      std::cerr << "[perf][project_tab_strip] layout    " << config_label
                << " recomputes=" << kLayoutRecomputes << " wall_ms=" << ms
                << " us/recompute=" << (ms * 1e3 / kLayoutRecomputes)
                << " allocations=" << delta.allocations
                << " bytes=" << delta.bytes_allocated << "\n";
      (void)sink;
    }
  };

  context.Measure("project_tab_strip.hittest", [&] {
    measure_config("strip_visible", "false");  // single project, not hidden -> visible
    measure_config("strip_hidden ", "true");   // single project, hidden when single
  });
}

const ScenarioRegistration g_perf_project_tab_strip_layout_hittest({Scenario{
    .name = "project_tab_strip_layout_hittest",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunProjectTabStripLayoutHittest,
}});
const ScenarioRegistration g_perf_repo_open_rss_idle({Scenario{
    .name = "repo_open_rss_idle",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    // warmup: same reason as the two `*_first_paint` scenarios below. Iteration 0
    // pays the project's cold open (background file-index build, initial watch
    // batch, session write) at 3,685 allocations against a 550 steady state, and
    // it is the only iteration that does. Without a warmup that single value
    // governs p95, so the gate answered differently depending on the iteration
    // count -- FAIL at 8 (+55% p95 allocations), PASS at 20, same binary.
    .warmup_iterations = 1,
    .run = RunRepoOpenRssIdle,
}});
const ScenarioRegistration g_perf_large_file_open_first_paint({Scenario{
    .name = "large_file_open_first_paint",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    // warmup: the first pass on a fresh driver also pays the project's cold
    // open (background file-index build, initial watch batch, session write),
    // which no later iteration repeats -- 5.6k allocations against a 1.8k
    // steady state. Discarding it keeps p95/max describing the tail of the
    // measured work instead of which iteration index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunLargeFileOpenFirstPaint,
}});
const ScenarioRegistration g_perf_large_file_open_lf_first_paint({Scenario{
    .name = "large_file_open_lf_first_paint",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    // warmup: the first pass on a fresh driver also pays the project's cold
    // open (background file-index build, initial watch batch, session write),
    // which no later iteration repeats -- 5.6k allocations against a 1.8k
    // steady state. Discarding it keeps p95/max describing the tail of the
    // measured work instead of which iteration index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunLargeFileOpenLfFirstPaint,
}});
const ScenarioRegistration g_perf_editor_buffer_find_incremental({Scenario{
    .name = "editor_buffer_find_incremental",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunEditorBufferFindIncremental,
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
const ScenarioRegistration g_perf_merge_model_build_interleaved({Scenario{
    .name = "merge_model_build_interleaved",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .run = RunMergeModelBuildInterleaved,
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
