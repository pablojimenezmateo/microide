#include "perf/Baseline.h"
#include "perf/PerfCpuAffinity.h"
#include "perf/PerfHarness.h"

#include "editor/BracketScanner.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "terminal/TerminalSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/TraceChannel.h"
#include "util/StringUtil.h"

#if defined(__linux__)
#include <unistd.h>
#endif

#include "util/JsonValue.h"
#include "util/Parse.h"

namespace microide::tests::perf {
namespace {

struct CliOptions {
  std::vector<std::string> scenarios;
  bool update_baseline = false;
  bool smoke = false;
  bool require_fixtures = false;
  bool keep_artifacts = false;
  std::size_t iterations = 10;
  std::optional<std::filesystem::path> report_json;
  std::optional<std::filesystem::path> report_text;
  std::optional<std::string> reference_runner;
  std::optional<std::string> layout_mode;
  // SDL renderer driver to measure through (see RunOptions::renderer_driver).
  // Default "software" keeps the portable, baseline-gated reference lane.
  std::string renderer_driver = "software";
  // SDL video driver to measure through (see RunOptions::video_driver). Default
  // "dummy" keeps the baseline-gated reference lane, and the harness sets it
  // itself so a bare `microide_perf` reproduces the gate.
  std::string video_driver = "dummy";
  // CPU set to measure on (see PerfCpuAffinity.h). "auto" pins to the fastest
  // cluster on a heterogeneous machine and is a no-op elsewhere.
  std::string pin_cores = "auto";
};

// Set by main() after CLI parse so scenario lambdas registered at static-init
// time can consult the flag. Default false matches local-dev behavior (skip
// missing fixtures); CI passes --require-fixtures so missing fixtures fail.
bool g_require_fixtures = false;

// Returns true if the fixture is present and the scenario should proceed.
// Returns false (silent skip) when the fixture is missing and --require-fixtures
// is not set. Throws when --require-fixtures is set — converts a quiet skip into
// a CI failure so a missing fixture cannot mask a regression.
bool EnsureFixtureOrSkip(const std::filesystem::path& fixture, const char* scenario_label) {
  if (DirectoryExistsNoThrow(fixture)) {
    return true;
  }
  if (g_require_fixtures) {
    throw std::runtime_error(std::string(scenario_label) +
                             ": required fixture missing: " + fixture.string());
  }
  std::cerr << scenario_label << ": fixture missing, skipping\n";
  return false;
}

struct ProcessSample {
  std::uint64_t rss_bytes = 0;
  std::uint64_t cpu_ticks = 0;
};

ProcessSample ReadProcessSample() {
#if defined(__linux__)
  ProcessSample sample{};
  {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t rss_pages = 0;
    statm >> total_pages >> rss_pages;
    if (!statm) {
      throw std::runtime_error("failed to read /proc/self/statm");
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    sample.rss_bytes = rss_pages * static_cast<std::uint64_t>(page_size > 0 ? page_size : 4096);
  }
  {
    std::ifstream stat("/proc/self/stat");
    std::string line;
    std::getline(stat, line);
    if (!stat || line.empty()) {
      throw std::runtime_error("failed to read /proc/self/stat");
    }
    const std::size_t tail_pos = line.rfind(')');
    if (tail_pos == std::string::npos || tail_pos + 2 >= line.size()) {
      throw std::runtime_error("unexpected /proc/self/stat format");
    }
    std::istringstream fields(line.substr(tail_pos + 2));
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) {
      tokens.push_back(token);
    }
    if (tokens.size() < 15) {
      throw std::runtime_error("insufficient /proc/self/stat fields");
    }
    // Non-throwing parse: a malformed /proc/self/stat shape yields a controlled
    // missing-sample (zero ticks) rather than an exception from a numeric parser.
    const std::optional<std::int64_t> utime = util::ParseInt64(tokens[11]);
    const std::optional<std::int64_t> stime = util::ParseInt64(tokens[12]);
    if (utime.has_value() && stime.has_value() && *utime >= 0 && *stime >= 0) {
      sample.cpu_ticks = static_cast<std::uint64_t>(*utime) + static_cast<std::uint64_t>(*stime);
    }
  }
  return sample;
#else
  return {};
#endif
}

double CpuPercentFromSamples(const ProcessSample& before,
                             const ProcessSample& after,
                             std::chrono::milliseconds elapsed) {
#if defined(__linux__)
  if (after.cpu_ticks < before.cpu_ticks || elapsed.count() <= 0) {
    return 0.0;
  }
  const long ticks_per_sec = sysconf(_SC_CLK_TCK);
  if (ticks_per_sec <= 0) {
    return 0.0;
  }
  const double cpu_seconds =
      static_cast<double>(after.cpu_ticks - before.cpu_ticks) / static_cast<double>(ticks_per_sec);
  const double wall_seconds = static_cast<double>(elapsed.count()) / 1000.0;
  return wall_seconds > 0.0 ? (cpu_seconds / wall_seconds) * 100.0 : 0.0;
#else
  (void)before;
  (void)after;
  (void)elapsed;
  return 0.0;
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

class FixtureRestoreGuard {
 public:
  FixtureRestoreGuard(std::filesystem::path path, std::string original)
      : path_(std::move(path)), original_(std::move(original)) {}

  ~FixtureRestoreGuard() {
    try {
      WriteFileTextOrThrow(path_, original_);
    } catch (...) {
      // Do not throw from destructor; best-effort restore only.
    }
  }

 private:
  std::filesystem::path path_;
  std::string original_;
};

std::vector<std::string> SplitComma(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  for (char c : text) {
    if (c == ',') {
      if (!current.empty()) {
        out.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

std::optional<CliOptions> ParseCli(int argc, char** argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i] != nullptr ? argv[i] : "";
    if (arg.rfind("--scenarios=", 0) == 0) {
      options.scenarios = SplitComma(arg.substr(std::string("--scenarios=").size()));
      continue;
    }
    if (arg == "--update-baseline") {
      options.update_baseline = true;
      continue;
    }
    if (arg == "--smoke") {
      options.smoke = true;
      continue;
    }
    if (arg == "--require-fixtures") {
      options.require_fixtures = true;
      continue;
    }
    if (arg == "--keep-artifacts") {
      options.keep_artifacts = true;
      continue;
    }
    if (arg.rfind("--iterations=", 0) == 0) {
      const auto parsed = util::ParseSize(arg.substr(std::string("--iterations=").size()));
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.iterations = *parsed;
      continue;
    }
    if (arg.rfind("--report-json=", 0) == 0) {
      options.report_json = std::filesystem::path(arg.substr(std::string("--report-json=").size()));
      continue;
    }
    if (arg.rfind("--report-text=", 0) == 0) {
      options.report_text = std::filesystem::path(arg.substr(std::string("--report-text=").size()));
      continue;
    }
    if (arg.rfind("--reference-runner=", 0) == 0) {
      options.reference_runner = arg.substr(std::string("--reference-runner=").size());
      continue;
    }
    if (arg.rfind("--layout-mode=", 0) == 0) {
      const std::string value = arg.substr(std::string("--layout-mode=").size());
      if (value != "auto" && value != "regular" && value != "compact") {
        return std::nullopt;
      }
      options.layout_mode = value;
      continue;
    }
    if (arg.rfind("--renderer=", 0) == 0) {
      options.renderer_driver = arg.substr(std::string("--renderer=").size());
      if (options.renderer_driver.empty()) {
        return std::nullopt;
      }
      continue;
    }
    if (arg.rfind("--video=", 0) == 0) {
      options.video_driver = arg.substr(std::string("--video=").size());
      if (options.video_driver.empty()) {
        return std::nullopt;
      }
      continue;
    }
    if (arg.rfind("--pin-cores=", 0) == 0) {
      options.pin_cores = arg.substr(std::string("--pin-cores=").size());
      if (options.pin_cores.empty()) {
        return std::nullopt;
      }
      continue;
    }
    return std::nullopt;
  }
  return options;
}

double SamplePercentile(std::vector<double>& values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double idx = p * static_cast<double>(values.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi) {
    return values[lo];
  }
  const double weight = idx - static_cast<double>(lo);
  return values[lo] * (1.0 - weight) + values[hi] * weight;
}

std::vector<std::string> FindStaleBaselineScenarios(const std::vector<Scenario>& scenarios) {
  std::unordered_set<std::string> registered;
  for (const Scenario& scenario : scenarios) {
    if (scenario.baseline_gated) {
      registered.insert(scenario.name);
    }
  }

  std::vector<std::string> stale;
  std::error_code error;
  const std::filesystem::path baseline_dir = "tests/perf/baselines";
  if (!std::filesystem::exists(baseline_dir, error) || error) {
    return stale;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(baseline_dir, error)) {
    if (error || !entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path file = entry.path();
    if (file.extension() != ".json") {
      continue;
    }
    const std::string scenario_name = file.stem().string();
    if (registered.find(scenario_name) == registered.end()) {
      stale.push_back(scenario_name);
    }
  }
  std::sort(stale.begin(), stale.end());
  return stale;
}

// TD-2026-07-17-059: the symmetric half of the manifest check. FindStaleBaseline-
// Scenarios catches baseline files with no registered scenario (orphans); this
// catches the reverse — a baseline-gated scenario with NO committed baseline file,
// which would otherwise run against harness defaults instead of a pinned budget.
std::vector<std::string> FindScenariosMissingBaseline(const std::vector<Scenario>& scenarios) {
  std::vector<std::string> missing;
  std::error_code error;
  const std::filesystem::path baseline_dir = "tests/perf/baselines";
  for (const Scenario& scenario : scenarios) {
    if (!scenario.baseline_gated) {
      continue;
    }
    const std::filesystem::path baseline = baseline_dir / (scenario.name + ".json");
    if (!std::filesystem::exists(baseline, error) || error) {
      missing.push_back(scenario.name);
    }
  }
  std::sort(missing.begin(), missing.end());
  return missing;
}

void EnforceP95Microseconds(const char* label, std::vector<double>& samples_us, double p95_budget_us) {
  if (samples_us.size() < 8) {
    throw std::runtime_error(std::string(label) + ": insufficient samples");
  }
  const double p95 = SamplePercentile(samples_us, 0.95);
  if (p95 > p95_budget_us) {
    throw std::runtime_error(std::string(label) + " p95_us=" + std::to_string(p95) + " budget_us=" +
                             std::to_string(p95_budget_us));
  }
}

std::string ConcatLinesAtIndices(const microide::editor::TextViewport& vp,
                                 const std::array<std::size_t, 8>& lines) {
  std::string out;
  out.reserve(4096);
  for (std::size_t L : lines) {
    if (L < vp.lines().size()) {
      out.append(vp.lines()[L]);
    }
    out.push_back('\n');
  }
  return out;
}

constexpr char kEditorEssentials50kCppPath[] =
    "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";

void OpenEditorEssentials50kCppOrThrow(ScenarioContext& context) {
  const std::filesystem::path path{kEditorEssentials50kCppPath};
  if (!PathExistsNoThrow(path)) {
    throw std::runtime_error(std::string("missing fixture: ") + kEditorEssentials50kCppPath);
  }
  if (!context.Open(path.parent_path())) {
    throw std::runtime_error("failed to open editor essentials fixture project");
  }
  context.OpenTab(path);
  context.PumpFrames(2);
}

void RegisterBuiltInScenarios() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  PerfHarness::RegisterScenario(Scenario{
      .name = "cold_startup_no_project",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            context.PumpFrames(5);
          },
  });
  // Five real project opens per iteration, each arming a file-index build and a
  // tree watch on background threads. Their WALL is not reproducible on this
  // runner -- five consecutive runs of one unchanged binary spread the p50 across
  // 25.4-34.7 ms (+37%) -- so the wall envelope is sized to cover that. The
  // allocation envelope stays tight: the counter is per-thread, so this scenario
  // reports only the shell thread's allocations and reports the same number every
  // run. That tight gate is what would catch a regression like the switch
  // teardown this scenario found (a 79 ms p50 before the inotify retire landed).
  PerfHarness::RegisterScenario(Scenario{
      .name = "multi_project_switch",
      .smoke = true,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            const std::vector<std::filesystem::path> projects = {
                "tests/perf/fixtures/small_project",
                "tests/perf/fixtures/large_project",
                "tests/perf/fixtures/kernel_sized_project",
                "tests/fixtures/diff/simple",
                "tests/fixtures/large/plain",
            };
            context.Measure("multi_project.switch_cycles", [&]() {
              for (int pass = 0; pass < 2; ++pass) {
                for (const auto& project : projects) {
                  (void)context.Open(project);
                  context.PumpFrames(2);
                }
              }
            });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "multi_tab_cycle",
      .smoke = true,
      // warmup: the first pass on a fresh driver pays the project's cold open --
      // the background file-index build, the initial watch batch, the session
      // write -- which is ~15x the steady-state cost of the measured work here.
      // Left un-warmed it landed in exactly one measured iteration and governed
      // p95/max, so the committed baseline held a p95 several times its own p50
      // and the allocation gate flapped between runs on nothing but which
      // percentile the cold pass fell into. `cold_startup_large_project` is the
      // scenario that measures that open on purpose.
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.PumpFrames(2);
            context.Measure("multi_tab.open_tabs", [&]() {
              for (int i = 1; i <= 20; ++i) {
                const auto file = std::filesystem::path("tests/perf/fixtures/large_project") /
                                  ("pkg0/file_" + std::to_string(i) + ".txt");
                context.OpenTab(file);
                context.PumpFrames(1);
              }
            });
            context.Measure("multi_tab.cycle_tabs", [&]() {
              for (int cycle = 0; cycle < 10; ++cycle) {
                for (SDL_Keycode digit : {SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5,
                                          SDLK_6, SDLK_7, SDLK_8, SDLK_9, SDLK_0}) {
                  context.KeyDown(digit, SDL_KMOD_ALT);
                  context.PumpFrames(1);
                }
              }
            });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "typing_small_file",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.OpenTab("tests/perf/fixtures/small_project/dir0/file_1.cpp");
            context.Type(" // perf typing small");
            std::string error;
            (void)context.AssertNoAllocationsDuringDraw(&error);
            context.PumpFrames(2);
          },
  });
  // Wall spread +20% p50/p95/max across four consecutive runs of one unchanged
  // binary; allocations are exact, so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "typing_large_file",
      .smoke = true,
      // warmup: the first pass on a fresh driver pays the project's cold open --
      // the background file-index build, the initial watch batch, the session
      // write -- which is ~15x the steady-state cost of the measured work here.
      // Left un-warmed it landed in exactly one measured iteration and governed
      // p95/max, so the committed baseline held a p95 several times its own p50
      // and the allocation gate flapped between runs on nothing but which
      // percentile the cold pass fell into. `cold_startup_large_project` is the
      // scenario that measures that open on purpose.
      .warmup_iterations = 1,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.OpenTab("tests/perf/fixtures/large_project/pkg0/file_1.txt");
            context.Type(" // perf typing large");
            std::string error;
            (void)context.AssertNoAllocationsDuringDraw(&error);
            context.PumpFrames(2);
          },
  });
  // Small measured phase (~1.5 ms) whose wall this runner cannot hold at the
  // default envelope; allocations are exact, so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "scroll_large_file",
      .smoke = true,
      // warmup: the first pass on a fresh driver pays the project's cold open --
      // the background file-index build, the initial watch batch, the session
      // write -- which is ~15x the steady-state cost of the measured work here.
      // Left un-warmed it landed in exactly one measured iteration and governed
      // p95/max, so the committed baseline held a p95 several times its own p50
      // and the allocation gate flapped between runs on nothing but which
      // percentile the cold pass fell into. `cold_startup_large_project` is the
      // scenario that measures that open on purpose.
      .warmup_iterations = 1,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.OpenTab("tests/perf/fixtures/large_project/pkg0/file_1.txt");
            for (int i = 0; i < 40; ++i) {
              context.Scroll(-1);
            }
            for (int i = 0; i < 20; ++i) {
              context.KeyDown(SDLK_PAGEDOWN);
            }
            context.PumpFrames(2);
          },
  });
  // Sustained scroll through *fresh* content across the whole 50k-line file.
  // Unlike editor_sticky_scroll_scroll / editor_render_whitespace_paint (which
  // re-scroll a small window and are therefore glyph-cache-friendly), this sweeps
  // top-to-bottom so the working set (tens of thousands of distinct colored runs)
  // far exceeds the 4096-entry texture cache and keeps missing on evicted lines
  // across iterations. This is the workload that decides whether the
  // composite-build path (BuildAsciiCompositeSurface + texture upload) is a real
  // bottleneck on the GPU lane -- i.e. whether a batched glyph atlas could ever
  // win. Advisory only (no portable baseline); run it via
  //   microide_perf --scenarios=editor_scroll_fresh_content_large --renderer=auto
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_scroll_fresh_content_large",
      .smoke = false,
      .baseline_gated = false,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(8);
            std::vector<double> samples_us;
            samples_us.reserve(640);
            // One downward sweep: ~640 page-downs over a 50k-line file paints a
            // continuously fresh viewport, so glyph-cache misses accumulate the
            // way they do when a user scrolls through a large file for real.
            for (int i = 0; i < 640; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.KeyDown(SDLK_PAGEDOWN);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            // Advisory envelope only -- a loose ceiling to catch gross blowups;
            // the real signal is p50 wall + the text_texture_cache_* counters.
            EnforceP95Microseconds("editor_scroll_fresh_content_large.page_down_frame",
                                   samples_us, 80'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "window_resize_stress",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.Measure("resize.compact_to_regular", [&]() {
              for (int i = 0; i < 12; ++i) {
                context.ResizeWindow(1280, 720);
                context.PumpFrames(1);
                context.ResizeWindow(1920, 1080);
                context.PumpFrames(1);
              }
            });
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "project_search_literal",
      .smoke = true,
      // Decoupled tolerances (see perf-harness.md). Now that the async stages are
      // driven to fixed states the ALLOCATION counts are exactly reproducible
      // run to run -- that is the real oracle here, and it is gated tight. The
      // measured work is a couple of milliseconds, where this shared runner's
      // scheduler jitter is +/-20%, so the wall envelope is widened rather than
      // letting jitter trip a gate every other run.
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
            // `search` is Find in Buffer (Ctrl+F), not project search -- this
            // scenario ran the wrong command against a query that appears in no
            // file's CONTENT (`node_0001` is a filename in this fixture), then
            // slept 50 ms. Its whole 52 ms wall was that sleep, and its
            // allocations were whatever the background file-index build happened
            // to have reached, which is why the committed baseline holds
            // p50=193 against p95=91930 and max=166987.
            //
            // Run the search the name promises, over a query with real coverage
            // (999 hits across the 1200-file fixture), and drive both async
            // stages to fixed states as search_first_result does: the index build
            // first (node_1200.cc is in the last subsystem directory, so its
            // presence means the build completed), then the search itself.
            if (!context.WaitForFileIndexPath("subsys_19/node_1200.cc",
                                              std::chrono::seconds(15))) {
              throw std::runtime_error("project_search_literal: file index did not build in time");
            }
            context.StartSearch("symbol_0");
            if (!context.WaitForProjectSearchFinished(std::chrono::seconds(15))) {
              throw std::runtime_error("project_search_literal: search did not finish in time");
            }
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "project_search_regex",
      .smoke = true,
      // See project_search_literal.
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
            // See project_search_literal: same wrong command, same content-free
            // query, same fixed-sleep sampling.
            if (!context.WaitForFileIndexPath("subsys_19/node_1200.cc",
                                              std::chrono::seconds(15))) {
              throw std::runtime_error("project_search_regex: file index did not build in time");
            }
            context.ToggleProjectSearchPatternMode();
            context.StartSearch("symbol_0[0-9]+");
            if (!context.WaitForProjectSearchFinished(std::chrono::seconds(15))) {
              throw std::runtime_error("project_search_regex: search did not finish in time");
            }
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "linter_on_save",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path project = "tests/perf/fixtures/linter_project";
            const std::filesystem::path source = project / "src" / "index.js";
            FixtureRestoreGuard restore_guard(source, ReadFileTextOrThrow(source));
            (void)context.Open(project);
            context.OpenTab(source);
            context.Measure("linter.type_invalid_edit", [&]() { context.Type("\nconst broken = ;"); });
            context.Measure("linter.save", [&]() { (void)context.ExecuteCommand("save"); });
            context.Measure("linter.wait_diagnostics", [&]() {
              (void)context.WaitForDiagnostics(source, std::chrono::milliseconds(120));
            });
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "compare_tab_open",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/fixtures/diff/simple");
            (void)context.ExecuteCommand("compare README.md HEAD");
            context.PumpFrames(3);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "merge_tab_open",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/fixtures/merge/simple");
            (void)context.ExecuteCommand(
                "merge base.txt incoming.txt current.txt result.txt");
            context.PumpFrames(3);
          },
  });
  // Both terminal scenarios below feed the emulator directly instead of launching
  // a command and sleeping. The harness runs with placeholder terminals
  // (SetUsePlaceholderTerminalsForTesting, PerfMain), so NO shell is ever spawned:
  // the previous `yes` / `bash -lc` launches produced a terminal holding exactly
  // one blank line, and both scenarios spent their whole measured window scrolling
  // and toggling an EMPTY buffer. The committed baselines recorded that. Feeding
  // bytes through the same path the pty reader thread uses makes them measure the
  // emulator, the scrollback and the render path they are named for, and makes the
  // allocation counts byte-identical run to run.
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_scroll_long_output",
      .smoke = true,
      // Iteration 0 pays the project open and the emulator's first buffer growth;
      // without a warmup it alone governs p95/max and the gate flaps.
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.Measure("terminal.open", [&]() { context.OpenTerminal("perf-output"); });
            std::string output;
            output.reserve(4000 * 24);
            for (int i = 0; i < 4000; ++i) {
              output += "perf-output-line ";
              output += std::to_string(i);
              output += "\r\n";
            }
            context.Measure("terminal.feed_output",
                            [&]() { context.FeedTerminalOutput(output); });
            context.Measure("terminal.scroll_burst", [&]() {
              for (int i = 0; i < 48; ++i) {
                context.Scroll(-1);
              }
            });
            context.PumpFrames(2);
            // The driver is shared across a scenario's iterations, so a terminal an
            // iteration opens is still attached on the next one.
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_alt_screen_toggle",
      .smoke = false,
      // Iteration 0 pays the project open and the emulator's first buffer growth;
      // without a warmup it alone governs p95/max and the gate flaps.
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.Measure("terminal.open", [&]() { context.OpenTerminal("perf-alt-screen"); });
            // Fill a deep primary scrollback, then toggle the alternate screen many
            // times. Each toggle enters + exits, which previously deep-copied the
            // full primary scrollback twice; this is what surfaces that cost (and
            // confirms the move-based swap that replaced it).
            std::string scrollback;
            scrollback.reserve(2000 * 32);
            for (int i = 0; i < 2000; ++i) {
              scrollback += "alt-screen-scrollback-line ";
              scrollback += std::to_string(i);
              scrollback += "\r\n";
            }
            context.Measure("terminal.fill_scrollback",
                            [&]() { context.FeedTerminalOutput(scrollback); });
            std::string toggles;
            toggles.reserve(200 * 16);
            for (int i = 0; i < 200; ++i) {
              toggles += "\x1b[?1049h\x1b[?1049l";
            }
            context.Measure("terminal.alt_toggle_burst",
                            [&]() { context.FeedTerminalOutput(toggles); });
            context.PumpFrames(2);
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "long_soak_8h",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            // Keep default runs practical; CI/nightly can override this via env.
            const std::uint64_t default_seconds = 3ULL;
            std::uint64_t soak_seconds = default_seconds;
            if (const char* env = std::getenv("MICROIDE_PERF_LONG_SOAK_SECONDS")) {
              const auto parsed = util::ParseSize(env);
              if (parsed.has_value() && *parsed > 0) {
                soak_seconds = static_cast<std::uint64_t>(*parsed);
              }
            }
            const std::uint64_t wake_budget_per_hour = 7200;
            const std::uint64_t sample_period_seconds = 1;
            const std::uint64_t midpoint_second = soak_seconds / 2;

            context.PumpFrames(2);
            const ProcessSample start_sample = ReadProcessSample();
            ProcessSample prev_cpu_sample = start_sample;
            ProcessSample mid_sample = start_sample;
            ProcessSample end_sample = start_sample;
            double max_cpu_percent = 0.0;
            std::uint64_t wakeups_current_hour = 0;

            for (std::uint64_t second = 1; second <= soak_seconds; ++second) {
              wakeups_current_hour += context.Wait(std::chrono::seconds(sample_period_seconds));
              const ProcessSample now = ReadProcessSample();
              max_cpu_percent =
                  std::max(max_cpu_percent, CpuPercentFromSamples(prev_cpu_sample, now, std::chrono::seconds(1)));
              prev_cpu_sample = now;

              if (second == midpoint_second) {
                mid_sample = now;
              }
              if (second % 3600 == 0) {
                if (wakeups_current_hour > wake_budget_per_hour) {
                  throw std::runtime_error("wake-up budget exceeded in long_soak_8h");
                }
                wakeups_current_hour = 0;
              }
              end_sample = now;
            }
            if (wakeups_current_hour > wake_budget_per_hour) {
              throw std::runtime_error("wake-up budget exceeded in final partial hour");
            }

            std::cerr << "long_soak_8h rss_start=" << start_sample.rss_bytes
                      << " rss_mid=" << mid_sample.rss_bytes
                      << " rss_end=" << end_sample.rss_bytes
                      << " max_cpu_percent=" << max_cpu_percent << "\n";
            context.PumpFrames(1);
          },
  });
  // CPU the process may spend across the 27 s soak window, summed over every
  // thread. Set from measurement with headroom, not from a guess -- see the
  // scenario body for what it does and does not cover.
  // CPU the whole process may spend across the 27 s soak window, summed over
  // every thread. Measured on perf-runner-v1 across ten iterations: 3.85, 3.95,
  // 4.09, 4.16, 4.67, then 8.55, 9.25, 10.31, 10.57, 10.69 -- one run, one clean
  // step where the governor walked the clock down (harness.cpu_calibration_ns
  // 671 -> 857 us at the same boundary). 20 ms sits at ~2x the slow mode, which
  // is 0.07% CPU across the window; a regression that so much as wakes a thread
  // at 10 Hz for 27 s lands far above it. Do not tighten it toward the fast mode
  // -- the fast mode is a property of the machine, not of the code.
  constexpr double kIdleSoakCpuBudgetMs = 20.0;
  PerfHarness::RegisterScenario(Scenario{
      .name = "idle_soak_30s",
      .smoke = false,
      // One discarded pass. Opening the fixture project costs the watcher ~1000
      // coalesced events and the file index a full build, none of which is what
      // "idle" means; that pass measured 32-51 ms against a 14 ms steady state
      // and owned this scenario's max and p95 outright.
      .warmup_iterations = 1,
      // Replaced by the soak-window assertion in the body -- see gate_cpu_metrics
      // in PerfHarness.h for why the iteration-level number cannot gate here.
      .gate_cpu_metrics = false,
      .run =
          [](ScenarioContext& context) {
            context.PumpFrames(2);
            // Section 13.E.3: prime outline debounce + active snippet placeholder session, then verify no
            // new SDL wake cadence during the soak window (same zero-wake assertion as before).
            context.PrimeEditorEssentialsIdleSoakSurface();
            // Task 9.6: assert watcher/executor threads generate zero wake events after settling.
            // Allow 3 s for background work to settle before the soak window begins.
            context.Wait(std::chrono::seconds(3));
            // Scope the CPU assertion to the soak window itself. The iteration's
            // baseline-gated p50_cpu_ms cannot do this job: 18 of the ~15 ms it
            // measures are harness frames at ~0.83 ms each, and two or three of
            // those are rendered on a core that just woke from 27 s of sleeping,
            // at the 605 MHz hardware floor rather than the 5157 MHz ceiling.
            // Whether the governor has ramped by then is what decides a 15 ms
            // iteration from a 30 ms one -- the same binary passed and failed the
            // same +100% gate on consecutive runs with byte-identical application
            // counters. The application's own idle cost, which is the whole point
            // of this scenario, is the ~2 ms residual underneath all that.
            // Measured here it is directly assertable, exactly like the zero-wake
            // budget, and immune to the clock question (TD-2026-08-05-137).
            const double cpu_before_soak = ProcessCpuMilliseconds();
            const std::uint64_t wakeups_during_soak =
                context.Wait(std::chrono::seconds(27));
            const double soak_cpu_ms = ProcessCpuMilliseconds() - cpu_before_soak;
            if (wakeups_during_soak > 0) {
              std::cerr << "idle_soak_30s: " << wakeups_during_soak
                        << " unexpected wake events during soak window\n";
              throw std::runtime_error("idle_soak_30s: unexpected wakes during soak window");
            }
            std::cerr << "idle_soak_30s soak_cpu_ms=" << soak_cpu_ms
                      << " budget_ms=" << kIdleSoakCpuBudgetMs << '\n';
            if (soak_cpu_ms > kIdleSoakCpuBudgetMs) {
              throw std::runtime_error("idle_soak_30s: CPU budget exceeded during soak window");
            }
            context.PumpFrames(1);
          },
  });
  // Task 9.2: file_finder_cold — open large fixture, measure time-to-first file-finder result.
  //
  // The finder rebuilds its whole candidate cache whenever the file index version
  // moves, and the index is built on a background thread, so WITHOUT the wait
  // below the rebuild lands in one or two of the ten iterations at random: the
  // measured allocations were 286 on the median and ~42,000 on the p95, and which
  // run captured a baseline decided whether the gate passed. Waiting for the index
  // to reach its last file puts the rebuild inside EVERY iteration -- which is
  // also the case the scenario is named for, a *cold* finder over a fully indexed
  // 10k-file project -- and makes the count identical run to run.
  PerfHarness::RegisterScenario(Scenario{
      .name = "file_finder_cold",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!EnsureFixtureOrSkip(fixture, "file_finder_cold")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("file_finder_cold: failed to open fixture");
            }
            if (!context.WaitForFileIndexPath("src_49/file_09999.cpp",
                                              std::chrono::seconds(20))) {
              throw std::runtime_error("file_finder_cold: file index did not finish building");
            }
            context.PumpFrames(2);
            context.OpenFileFinder();
            context.PumpFrames(1);
          },
  });
  // Task 9.4: git_sidebar_activate — open git fixture, activate sidebar, measure first status.
  PerfHarness::RegisterScenario(Scenario{
      .name = "git_sidebar_activate",
      .smoke = false,
      // The activation dispatches an async git status; five frame pumps caught
      // however much of it happened to have landed, so the allocation count --
      // this suite's one deterministic oracle -- wandered 498/534/595/624/626
      // across runs with `git.commands_run`, `subprocess.spawns` and
      // `git.command_output_bytes` byte-identical. Waiting for the refresh to
      // land puts the same work inside the window every time, and it is also what
      // this scenario claims to measure ("first status"). The warmup discards the
      // 4,137-allocation cold pass that owned max and p95.
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/git_status_project");
            if (!EnsureFixtureOrSkip(fixture, "git_sidebar_activate")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("git_sidebar_activate: failed to open fixture");
            }
            context.PumpFrames(2);
            context.ActivateGitSidebar();
            // Run the status the activation dispatched, synchronously, the way
            // every git-workstation scenario does. Activating alone dispatches it
            // onto the background executor and the five frame pumps below caught
            // however much of it happened to have landed -- so this scenario's
            // allocation count, the suite's one deterministic oracle, wandered
            // 498/534/595/624/626 across runs with git.commands_run,
            // subprocess.spawns and git.command_output_bytes byte-identical.
            workspace::WorkspaceShell::TestAccess::PerfRunGitSidebarRefreshSync(context.Shell());
            context.PumpFrames(5);
          },
  });
  // Task 9.5: search_first_result — search for a symbol near end of 10k-file corpus.
  // Measures ~2.5 ms, which four consecutive runs of one unchanged binary
  // spread by +-23%. Allocations are byte-identical across those runs, so they
  // keep the tight gate and stay the real oracle here.
  PerfHarness::RegisterScenario(Scenario{
      .name = "search_first_result",
      .smoke = false,
      // Warm up until the process reaches steady state: the first pass builds the
      // 10k-file index cold, and background subsystems (index watcher initial
      // scan, etc.) keep allocating for several more passes before quiescing.
      // Discarding those settling passes — combined with the single search worker,
      // index-ready wait, and drain-once below — makes the measured MEDIAN a fully
      // deterministic 20,207 allocations every run, so the tight p50 gate below is
      // the authoritative, precise regression signal. Every iteration measures the
      // same steady-state value, so p95/max are the same metric under noise: their
      // razor-thin baseline (also 20,207) has no headroom, so a lone iteration
      // spilled by an incidental background wake would trip them. Widen p95/max to
      // absorb that irreducible global-counter tail noise rather than false-positive
      // — the deterministic allocation p50 already catches any genuine regression.
      //
      // That determinism is the ALLOCATION count, not the wall: the measured phase
      // is ~2.5 ms and four consecutive runs of one unchanged binary spread its
      // wall p50 by +-23%, so the wall envelopes are widened too and the allocation
      // envelopes stay tight. Which is the point of decoupling them.
      .warmup_iterations = 16,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!EnsureFixtureOrSkip(fixture, "search_first_result")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("search_first_result: failed to open fixture");
            }
            // Determinism: the project search's candidate set is the file index
            // and its result stream is async, so a bare PumpFrames snapshot
            // measured a random point mid-index / mid-search (its
            // p50_allocations swung ~80x run to run). Drive both async stages to
            // fixed states so the whole-run metric is deterministic. symbol_09999
            // lives in the last fixture file (src_49/file_09999.cpp), so its
            // presence in the index means the initial build has fully completed.
            if (!context.WaitForFileIndexPath("src_49/file_09999.cpp",
                                              std::chrono::seconds(15))) {
              throw std::runtime_error("search_first_result: file index did not build in time");
            }
            context.StartSearch("symbol_09999");
            if (!context.WaitForProjectSearchFinished(std::chrono::seconds(15))) {
              throw std::runtime_error("search_first_result: search did not finish in time");
            }
            context.PumpFrames(2);
          },
  });
  // Wall spread +33% p50 / +23% p95 across four consecutive runs of one
  // unchanged binary; allocations byte-identical, so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "cold_startup_small_project",
      .smoke = true,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(5);
          },
  });
  // Wall spread +29% p50 across four consecutive runs of one unchanged binary.
  // Allocations are exact (the counter is per-thread), so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "cold_startup_large_project",
      .smoke = true,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.PumpFrames(5);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "switch_and_idle",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path project_a = "tests/perf/fixtures/switch_project_a";
            const std::filesystem::path project_b = "tests/perf/fixtures/switch_project_b";

            (void)context.Open(project_a);
            for (int i = 1; i <= 20; ++i) {
              const std::string index = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
              context.OpenTab(project_a / "src" / ("file_" + index + ".cpp"));
            }
            context.PumpFrames(3);

            (void)context.Open(project_b);
            for (int i = 1; i <= 15; ++i) {
              const std::string index = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
              context.OpenTab(project_b / "src" / ("file_" + index + ".cpp"));
            }
            context.PumpFrames(3);

            (void)context.Open(project_a);
            context.PumpFrames(1);
            (void)context.Open(project_b);
            context.PumpFrames(30);
          },
  });

  // OpenSpec §13.B — block structure (folding, sticky scroll, indent guides / whitespace, outline).
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_fold_recompute",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.enabled", "true");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(24);
            microide::editor::TextViewport& vp = TA::ActiveEditor(context.Shell());
            std::vector<double> samples_us;
            samples_us.reserve(28);
            // Edits deep in the brace nest invalidate a broad prefix; `EnsureFoldingModelFresh` stays
            // on the `ComputeWithBudget(2000)` path (WorkspaceFoldingRefresh.cpp).
            for (int i = 0; i < 24; ++i) {
              (void)context.ExecuteCommand("goto 25000");
              const auto t0 = std::chrono::steady_clock::now();
              vp.InsertCharacter('!');
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
              if (!vp.Undo()) {
                throw std::runtime_error("editor_fold_recompute: undo failed");
              }
            }
            EnforceP95Microseconds("editor_fold_recompute.insert_and_fold_frame", samples_us,
                                   60'000.0);
            std::string error;
            (void)context.AssertNoAllocationsDuringDraw(&error);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_fold_viewport_refresh",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.enabled", "true");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(20);
            (void)TA::EnsureActiveFoldingModelFresh(context.Shell());
            std::vector<double> samples_us;
            samples_us.reserve(96);
            for (int i = 0; i < 96; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Scroll(-2);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            EnforceP95Microseconds("editor_fold_viewport_refresh.scroll_frame", samples_us,
                                   30'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_sticky_scroll_scroll",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.enabled", "true");
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.sticky_scroll.enabled",
                                      "true");
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.sticky_scroll.max_depth", "4");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(20);
            (void)context.ExecuteCommand("goto 12000 8");
            std::vector<double> samples_us;
            samples_us.reserve(100);
            for (int i = 0; i < 100; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Scroll(-3);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            EnforceP95Microseconds("editor_sticky_scroll_scroll.fast_scroll_frame", samples_us,
                                   30'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_indent_guides_paint",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.enabled", "true");
            (void)TA::SetSettingValue(context.Shell(), "editor.view.indent_guides.enabled", "true");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(18);
            std::vector<double> samples_us;
            samples_us.reserve(80);
            for (int i = 0; i < 80; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Scroll(((i % 7) & 1) != 0 ? 2 : -2);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            EnforceP95Microseconds("editor_indent_guides_paint.scroll_paint_frame", samples_us,
                                   30'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_render_whitespace_paint",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.view.render_whitespace", "true");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(18);
            std::vector<double> samples_us;
            samples_us.reserve(80);
            for (int i = 0; i < 80; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Scroll(-2);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            EnforceP95Microseconds("editor_render_whitespace_paint.scroll_overlay_frame", samples_us,
                                   30'000.0);
            context.PumpFrames(2);
          },
  });
  // openspec change split-layout-revision-tiers — the contract scenario.
  //
  // After the tier split, pure scrolling over a syntax-highlighted file MUST
  // NOT bump `content_revision`, `syntax_revision`, or `layout_shape_revision`.
  // Only `presentation_revision` may move (selection caret, hover overlay).
  // The scenario asserts the per-tier counters directly so a future change
  // that re-introduces a cross-tier invalidation on the scroll path fails
  // here regardless of wall-time impact.
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_scroll_only_no_content_bump",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(20);
            // Capture counters AFTER warm-up so the initial file-load
            // invalidations are excluded from the measurement window.
            const std::uint64_t content_before =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorContentRevisionBumps);
            const std::uint64_t syntax_before =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorSyntaxRevisionBumps);
            const std::uint64_t layout_shape_before =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorLayoutShapeRevisionBumps);
            std::vector<double> samples_us;
            samples_us.reserve(100);
            for (int i = 0; i < 100; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Scroll((i % 2 == 0) ? -2 : 2);
              context.PumpFrames(1);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            const std::uint64_t content_after =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorContentRevisionBumps);
            const std::uint64_t syntax_after =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorSyntaxRevisionBumps);
            const std::uint64_t layout_shape_after =
                microide::util::ReadPerformanceCounter(
                    microide::util::PerfCounterId::EditorLayoutShapeRevisionBumps);
            if (content_after != content_before) {
              throw std::runtime_error(
                  "editor_scroll_only_no_content_bump: scrolling bumped "
                  "content_revision (delta=" +
                  std::to_string(content_after - content_before) + ")");
            }
            if (syntax_after != syntax_before) {
              throw std::runtime_error(
                  "editor_scroll_only_no_content_bump: scrolling bumped "
                  "syntax_revision (delta=" +
                  std::to_string(syntax_after - syntax_before) + ")");
            }
            if (layout_shape_after != layout_shape_before) {
              throw std::runtime_error(
                  "editor_scroll_only_no_content_bump: scrolling bumped "
                  "layout_shape_revision (delta=" +
                  std::to_string(layout_shape_after - layout_shape_before) +
                  ")");
            }
            EnforceP95Microseconds(
                "editor_scroll_only_no_content_bump.scroll_frame", samples_us,
                30'000.0);
            context.PumpFrames(2);
          },
  });
  // OpenSpec §13.C — pair and indent (editor_essentials_50k_cpp).
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_bracket_match_caret_motion",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            microide::editor::TextViewport& vp = context.ActiveViewport();
            // Measure FindBracketMatch, the entry point the editor actually calls on
            // caret motion -- NOT the FindBracketMatchInLines test seam. The seam takes
            // a caller-built line vector, so timing it excluded the per-call window
            // materialization that production pays, and the old scenario prebuilt a
            // view for all 50k lines outside the timed loop: exactly the O(file) shape
            // FindBracketMatch was rewritten to stop doing. A regression in the window
            // build was therefore invisible to this baseline.
            // Shallow nest depth keeps bracket scans bounded while the 50k-line buffer is live.
            vp.MoveCursorTo(24, 8, false);
            std::vector<double> samples_us;
            samples_us.reserve(400);
            for (int i = 0; i < 400; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              (void)microide::editor::FindBracketMatch(vp, vp.cursor_line(), vp.cursor_column(),
                                                       2000);
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
              vp.MoveCursorHorizontal(1);
            }
            // design.md targets ≤ 50µs for the balanced scan; `HighlightedLineTokens` on this path
            // is heavier under Xvfb. Baseline + this envelope gate gross regressions on large fixtures.
            EnforceP95Microseconds("editor_bracket_match_caret_motion", samples_us, 125'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_auto_close_typing",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            microide::editor::TextViewport& vp = context.ActiveViewport();
            vp.MoveCursorTo(32, 0, false);
            vp.MoveCursorLineEnd(false);
            std::vector<double> samples_us;
            samples_us.reserve(128);
            for (int i = 0; i < 120; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              context.Type("(");
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
              if (!vp.Undo()) {
                throw std::runtime_error("editor_auto_close_typing: undo failed");
              }
            }
            // Advisory: stay within practical typing latency on the large buffer (see design.md + typing_large_file).
            EnforceP95Microseconds("editor_auto_close_typing", samples_us, 200'000.0);
            context.PumpFrames(2);
          },
  });
  // Wall spread +54% p50 / +90% max across four consecutive runs of one
  // unchanged binary; allocations byte-identical, so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_smart_indent_typing",
      .smoke = false,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            microide::editor::TextViewport& vp = context.ActiveViewport();
            const std::size_t header_line = 5;
            if (vp.lines().size() <= header_line) {
              throw std::runtime_error("editor_smart_indent_typing: fixture too small");
            }
            vp.MoveCursorTo(header_line, vp.lines()[header_line].size(), false);
            std::vector<double> samples_us;
            samples_us.reserve(128);
            for (int i = 0; i < 120; ++i) {
              const auto t0 = std::chrono::steady_clock::now();
              vp.InsertNewline();
              const auto t1 = std::chrono::steady_clock::now();
              samples_us.push_back(
                  std::chrono::duration<double, std::micro>(t1 - t0).count());
              if (!vp.Undo()) {
                throw std::runtime_error("editor_smart_indent_typing: undo failed");
              }
            }
            EnforceP95Microseconds("editor_smart_indent_typing", samples_us, 200'000.0);
            context.PumpFrames(2);
          },
  });
  // Wall spread +29% p50 / +37% p95 across four consecutive runs of one
  // unchanged binary; allocations byte-identical, so they stay tight.
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_surround_multi_caret",
      .smoke = false,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            microide::editor::TextViewport& vp = context.ActiveViewport();
            constexpr std::size_t kSelLen = 80;
            std::array<std::size_t, 8> line_indices{};
            for (std::size_t i = 0; i < 8; ++i) {
              line_indices[i] = 20000u + i * 1200u;
            }
            const std::string before = ConcatLinesAtIndices(vp, line_indices);
            for (int i = 0; i < 8; ++i) {
              const std::size_t line = line_indices[static_cast<std::size_t>(i)];
              if (line >= vp.lines().size()) {
                throw std::runtime_error("editor_surround_multi_caret: line out of range");
              }
              const std::string& L = vp.lines()[line];
              std::size_t start = L.find("volatile");
              if (start == std::string::npos) {
                start = 40;
              }
              if (start + kSelLen > L.size()) {
                start = L.size() > kSelLen ? L.size() - kSelLen : 0;
              }
              const microide::editor::SelectionRange range{
                  .start = {line, start},
                  .end = {line, start + kSelLen},
              };
              if (i == 0) {
                vp.MoveCursorTo(range.end.line, range.end.column, false);
                vp.MoveCursorTo(range.start.line, range.start.column, true);
              } else {
                vp.AddSecondaryCaretWithRange(range);
              }
            }
            if (!vp.has_multiple_carets()) {
              throw std::runtime_error("editor_surround_multi_caret: expected multi-caret setup");
            }
            if (ConcatLinesAtIndices(vp, line_indices) != before) {
              throw std::runtime_error("editor_surround_multi_caret: buffer changed before insert");
            }
            context.Measure("editor.surround_multi_caret.insert", [&]() { vp.InsertCharacter('('); });
            if (ConcatLinesAtIndices(vp, line_indices) == before) {
              throw std::runtime_error("editor_surround_multi_caret: insert had no effect");
            }
            if (!vp.Undo()) {
              throw std::runtime_error("editor_surround_multi_caret: undo failed");
            }
            if (ConcatLinesAtIndices(vp, line_indices) != before) {
              throw std::runtime_error(
                  "editor_surround_multi_caret: single undo should restore 8-range surround");
            }
            context.PumpFrames(2);
          },
  });
}

std::vector<std::pair<std::string, std::uint64_t>> AggregateCounterTotals(
    const Aggregate& aggregate) {
  std::unordered_map<std::string, std::uint64_t> totals;
  for (const Iteration& iteration : aggregate.iterations) {
    for (const auto& [name, value] : iteration.perf_counters) {
      totals[name] += value;
    }
  }
  std::vector<std::pair<std::string, std::uint64_t>> ordered;
  ordered.reserve(totals.size());
  for (auto& entry : totals) {
    ordered.push_back(entry);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });
  return ordered;
}

util::JsonValue ToJson(const Aggregate& aggregate) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  util::JsonArray iterations_json;
  iterations_json.reserve(aggregate.iterations.size());
  for (const Iteration& iteration : aggregate.iterations) {
    util::JsonObject phase_duration_json;
    util::JsonObject phase_metrics_json;
    for (const Iteration::PhaseMetrics& phase : iteration.phase_metrics) {
      phase_duration_json[phase.name] = phase.wall_ms;
      phase_metrics_json[phase.name] = util::JsonObject{
          {"wall_ms", phase.wall_ms},
          {"allocations", static_cast<std::int64_t>(phase.allocations)},
          {"frees", static_cast<std::int64_t>(phase.frees)},
          {"bytes_allocated", static_cast<std::int64_t>(phase.bytes_allocated)},
          {"bytes_freed", static_cast<std::int64_t>(phase.bytes_freed)},
      };
    }
    util::JsonObject counters_json;
    for (const auto& [name, value] : iteration.perf_counters) {
      counters_json[name] = static_cast<std::int64_t>(value);
    }
    util::JsonObject iteration_json;
    iteration_json["index"] = static_cast<std::int64_t>(iteration.index);
    iteration_json["wall_ms"] = iteration.metrics.wall_ms;
    iteration_json["allocations"] = static_cast<std::int64_t>(iteration.metrics.allocations);
    iteration_json["frees"] = static_cast<std::int64_t>(iteration.metrics.frees);
    iteration_json["bytes_allocated"] = static_cast<std::int64_t>(iteration.metrics.bytes_allocated);
    iteration_json["bytes_freed"] = static_cast<std::int64_t>(iteration.metrics.bytes_freed);
    iteration_json["cpu_ms"] = iteration.metrics.cpu_ms;
    iteration_json["rss_growth_bytes"] =
        static_cast<std::int64_t>(iteration.metrics.rss_growth_bytes);
    iteration_json["phase_durations_ms"] = std::move(phase_duration_json);
    iteration_json["phase_metrics"] = std::move(phase_metrics_json);
    iteration_json["perf_counters"] = std::move(counters_json);
    iterations_json.push_back(std::move(iteration_json));
  }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  return util::JsonObject{
      {"scenario", aggregate.scenario_name},
      {"smoke", aggregate.smoke},
      {"metrics", util::JsonObject{
                      {"p50_wall_ms", aggregate.metrics.p50_wall_ms},
                      {"p95_wall_ms", aggregate.metrics.p95_wall_ms},
                      {"max_wall_ms", aggregate.metrics.max_wall_ms},
                      {"p50_allocations", aggregate.metrics.p50_allocations},
                      {"p95_allocations", aggregate.metrics.p95_allocations},
                      {"max_allocations", aggregate.metrics.max_allocations},
                      {"p50_cpu_ms", aggregate.metrics.p50_cpu_ms},
                      {"p95_cpu_ms", aggregate.metrics.p95_cpu_ms},
                      {"max_cpu_ms", aggregate.metrics.max_cpu_ms},
                      {"p50_rss_growth_bytes", aggregate.metrics.p50_rss_growth_bytes},
                      {"p95_rss_growth_bytes", aggregate.metrics.p95_rss_growth_bytes},
                      {"max_rss_growth_bytes", aggregate.metrics.max_rss_growth_bytes},
                      {"mean_rss_growth_bytes", aggregate.metrics.mean_rss_growth_bytes},
                      {"p50_cpu_calibration_ns", aggregate.metrics.p50_cpu_calibration_ns},
                  }},
      {"iterations", std::move(iterations_json)},
  };
}

}  // namespace
}  // namespace microide::tests::perf

int main(int argc, char** argv) {
  using namespace microide::tests::perf;
  // Scenarios drive the shell from this thread, so it is the main thread for
  // trace-summary purposes.
  microide::util::MarkTracingMainThread();
  // Pin project-search to a single worker for the whole run. The allocation
  // counter is process-global (counts every thread), so N parallel search workers
  // make a search scenario's measured allocations non-deterministic; one worker
  // makes them reproducible. This is a regression harness, not an absolute-latency
  // benchmark, so serialized-but-stable is the right trade (see perf-harness.md).
  // Respects an existing value so a caller can override.
  setenv("MICROIDE_SEARCH_WORKER_LIMIT", "1", /*overwrite=*/0);
  // Placeholder terminals (no real shell spawn) during perf measurement; was a
  // compile-time MICROIDE_TESTING fork, now a runtime switch (see TestMain.cpp).
  microide::terminal::SetUsePlaceholderTerminalsForTesting(true);
  RegisterBuiltInScenarios();
  const std::optional<CliOptions> options = ParseCli(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: microide_perf [--scenarios=a,b] [--update-baseline] [--smoke] "
                 "[--require-fixtures] [--keep-artifacts] [--iterations=N] "
                 "[--report-json=path] [--report-text=path] "
                 "[--reference-runner=name] "
                 "[--layout-mode=auto|regular|compact] "
                 "[--renderer=software|auto|<sdl-driver>] "
                 "[--video=dummy|auto|<sdl-driver>] "
                 "[--pin-cores=auto|off|<cpu-list>]\n";
    return 1;
  }

  g_require_fixtures = options->require_fixtures;

  // Before anything is measured: settle which CPUs this process runs on. On a
  // hybrid machine the scheduler's choice is worth up to 2.4x on a small
  // scenario, which is larger than most of the gates -- see PerfCpuAffinity.h.
  const CpuAffinityPlan affinity = ApplyPerfCpuAffinity(options->pin_cores);
  std::cerr << "[perf] cpu affinity: " << affinity.description << '\n';
  // A run that declined the harness's own CPU set is measuring a different
  // machine than the baselines describe, same as a windowed video lane.
  const bool unpinned_lane = options->pin_cores != "auto";
  if (unpinned_lane && options->update_baseline) {
    std::cerr << "--update-baseline is only valid with --pin-cores=auto; measuring on a "
                 "hand-picked CPU set (--pin-cores=" << options->pin_cores
              << ") is advisory\n";
    return 1;
  }

  PerfHarness::RunOptions run_options;
  run_options.scenario_names = options->scenarios;
  run_options.smoke_only = options->smoke;
  run_options.iterations = options->iterations;
  run_options.layout_mode_override = options->layout_mode;
  run_options.keep_artifacts = options->keep_artifacts;
  run_options.renderer_driver = options->renderer_driver;
  run_options.video_driver = options->video_driver;

  // A non-software renderer is the advisory GPU lane: numbers are not
  // cross-machine portable, so they are reported but never gated or written to
  // baselines (mirrors the DAP advisory scenarios). The software lane stays the
  // authoritative, baseline-gated reference.
  const bool gpu_lane = run_options.renderer_driver != "software";
  if (gpu_lane && options->update_baseline) {
    std::cerr << "--update-baseline is only valid on the software reference lane; "
                 "the GPU lane (--renderer=" << run_options.renderer_driver
              << ") is advisory and cannot write baselines\n";
    return 1;
  }
  // Same rule for the video lane, and it is the one that actually bit: a real
  // video driver adds window-system present cost to every frame-pumping
  // scenario and none to the pure-unit ones, so a windowed run reads as a
  // 2-12x regression across half the suite with byte-identical allocation
  // counts. Two sessions chased that as a code regression before the lane was
  // identified. Numbers from a windowed run are advisory and cannot be written
  // back as baselines.
  const bool windowed_lane = run_options.video_driver != "dummy";
  if (windowed_lane && options->update_baseline) {
    std::cerr << "--update-baseline is only valid on the dummy video reference lane; "
                 "a windowed run (--video=" << run_options.video_driver
              << ") measures window-system present cost and is advisory\n";
    return 1;
  }
  const auto stale_baselines = FindStaleBaselineScenarios(PerfHarness::RegisteredScenarios());
  if (!stale_baselines.empty()) {
    std::cerr << "stale perf baselines found for unregistered scenarios:\n";
    for (const std::string& name : stale_baselines) {
      std::cerr << "  - " << name << "\n";
    }
    std::cerr << "remove or restore these scenarios before running perf baselines\n";
    return 1;
  }
  // TD-2026-07-17-059: the reverse manifest direction — every baseline-gated
  // scenario must have a committed baseline. A missing one would silently run
  // against harness-default tolerances instead of a pinned budget. --update-baseline
  // is exempt (that run is how a new scenario's baseline gets written).
  if (!options->update_baseline) {
    const auto missing_baselines = FindScenariosMissingBaseline(PerfHarness::RegisteredScenarios());
    if (!missing_baselines.empty()) {
      std::cerr << "registered perf scenarios missing a committed baseline:\n";
      for (const std::string& name : missing_baselines) {
        std::cerr << "  - " << name << "\n";
      }
      std::cerr << "run with --update-baseline to write baselines for new scenarios\n";
      return 1;
    }
  }
  std::vector<Aggregate> aggregates;
  bool all_passed = true;
  std::size_t selected_count = 0;
  // Integrity gate for the manifest-backed fixture trees.
  //
  // Policy matches EnsureFixtureOrSkip, deliberately, because two different
  // answers to "the fixture is not here" is how this broke: a tree that is
  // ENTIRELY ABSENT is a skip (or a hard failure under --require-fixtures), and a
  // tree that is PRESENT must match its manifest or the run dies. These trees are
  // gitignored and generated on demand by the ctest `microide_perf_fixtures`
  // setup, so a fresh checkout legitimately has none of them — and this loop used
  // to hash them unconditionally and report the empty-tree digest as a corruption
  // mismatch. That is what took CI's perf-canary lane red: it runs microide_perf
  // directly for one scenario that reads no fixture at all, so nothing had
  // generated them and nothing needed them.
  //
  // Verifying only what is present also means a filtered run pays only for the
  // trees it has, rather than rehashing ~17 MB across three trees to run one
  // fixture-free scenario.
  {
    static constexpr std::string_view kManifestBackedFixtures[] = {
        "tests/perf/fixtures/kernel_sized_project",
        "tests/perf/fixtures/editor_essentials_50k_cpp",
        "tests/perf/fixtures/editor_essentials_50k_py",
    };
    for (const std::string_view fixture : kManifestBackedFixtures) {
      const std::filesystem::path root{fixture};
      if (!DirectoryExistsNoThrow(root)) {
        if (options->require_fixtures) {
          std::cerr << "required fixture tree missing: " << root.string()
                    << " (generate with tests/perf/generate_*.py, or drop --require-fixtures)\n";
          return 1;
        }
        std::cerr << root.string() << ": fixture tree absent, integrity check skipped\n";
        continue;
      }
      std::string fixture_error;
      if (!PerfHarness::VerifyFixtureTree(root, &fixture_error)) {
        std::cerr << fixture_error << '\n';
        return 1;
      }
    }
  }

  std::size_t run_index = 0;
  for (const Scenario& scenario : PerfHarness::RegisteredScenarios()) {
    PerfHarness::RunOptions probe = run_options;
    // Mirror ShouldRunScenario exactly: with no explicit --scenarios list, an
    // opt-in scenario (run_by_default=false, e.g. editor_moby_dick_workout whose
    // fixture is a network fetch) is NOT selected. Without this the bare full
    // run marked it selected while RunScenario skipped it, so the missing
    // aggregate was misread as "scenario failed to run" and aborted the gate.
    const bool name_selected =
        probe.scenario_names.empty()
            ? scenario.run_by_default
            : std::find(probe.scenario_names.begin(), probe.scenario_names.end(), scenario.name) !=
                  probe.scenario_names.end();
    const bool selected = name_selected && (!probe.smoke_only || scenario.smoke);
    if (selected) {
      ++selected_count;
    }
    if (selected) {
      ++run_index;
      std::cerr << "[perf] running scenario " << run_index << "/" << selected_count << ": "
                << scenario.name << '\n';
    }
    const auto aggregate = PerfHarness::RunScenario(scenario, run_options);
    if (!aggregate.has_value()) {
      if (selected) {
        std::cerr << "scenario failed to run: " << scenario.name
                  << " (" << PerfHarness::LastError() << ")\n";
        return 1;
      }
      continue;
    }
    aggregates.push_back(*aggregate);

    if (gpu_lane) {
      // Advisory GPU lane: report numbers, never gate or compare to the
      // software baselines (different backend, non-portable timings).
      std::cerr << "[perf][gpu] " << scenario.name
                << " p50=" << aggregate->metrics.p50_wall_ms << "ms"
                << " p95=" << aggregate->metrics.p95_wall_ms << "ms\n";
      continue;
    }

    const std::filesystem::path baseline_path =
        std::filesystem::path("tests/perf/baselines") / (scenario.name + ".json");
    if (options->update_baseline) {
      if (!scenario.baseline_gated) {
        // Refusing to write an advisory scenario's baseline is right; aborting
        // the whole run over it was not. A bare `--update-baseline` sweeps in
        // every registered scenario, so one advisory entry made "rebaseline the
        // suite" impossible -- the run died partway through and left every
        // baseline it had not reached untouched, which is how the committed set
        // drifted 3-8x stale. Skip it and carry on; fail only when the caller
        // named it explicitly, where the request really is unsatisfiable.
        const bool named_explicitly =
            !options->scenarios.empty() &&
            std::find(options->scenarios.begin(), options->scenarios.end(), scenario.name) !=
                options->scenarios.end();
        std::cerr << (named_explicitly ? "refusing to update baseline for advisory-only scenario: "
                                       : "[perf] skipping baseline update for advisory-only "
                                         "scenario: ")
                  << scenario.name << '\n';
        if (named_explicitly) {
          return 1;
        }
        continue;
      }
      // A negative allocation tolerance means "inherit the matching wall
      // tolerance" -- resolve it here so the written baseline always carries an
      // explicit value.
      const auto resolve_alloc = [](double alloc_tol, double wall_tol) {
        return alloc_tol >= 0.0 ? alloc_tol : wall_tol;
      };
      BaselineRecord record{
          .scenario_name = scenario.name,
          .metrics = aggregate->metrics,
          .tolerances = {.p50_percent = scenario.tolerance_p50_percent,
                         .p95_percent = scenario.tolerance_p95_percent,
                         .max_percent = scenario.tolerance_max_percent,
                         .alloc_p50_percent = resolve_alloc(scenario.tolerance_alloc_p50_percent,
                                                            scenario.tolerance_p50_percent),
                         .alloc_p95_percent = resolve_alloc(scenario.tolerance_alloc_p95_percent,
                                                            scenario.tolerance_p95_percent),
                         .alloc_max_percent = resolve_alloc(scenario.tolerance_alloc_max_percent,
                                                            scenario.tolerance_max_percent),
                         // CPU carries the same scheduler jitter as wall, so it
                         // inherits the scenario's *resolved* wall envelope rather
                         // than the struct default. Writing the 10/20/50 default
                         // next to a widened 100/150/200 wall envelope would gate
                         // CPU ten times tighter than the metric it tracks, and
                         // every scenario with a widened wall tolerance would flag
                         // on CPU immediately.
                         .cpu_p50_percent = scenario.tolerance_p50_percent,
                         .cpu_p95_percent = scenario.tolerance_p95_percent,
                         .cpu_max_percent = scenario.tolerance_max_percent,
                         // From the scenario, not the struct default: see
                         // Scenario::tolerance_rss_percent for the rebaseline that
                         // silently reset a deliberate widening.
                         .rss_mean_percent = scenario.tolerance_rss_percent},
      };
      record.has_cpu_metrics = scenario.gate_cpu_metrics;
      record.has_rss_metrics = true;
      // Record the clock this baseline was captured at, so a later run's CPU
      // numbers can be compared in the same machine state instead of against a
      // number that silently meant "measured on a core that happened to be at
      // 5.1 GHz" (TD-2026-08-05-137).
      record.has_calibration = aggregate->metrics.p50_cpu_calibration_ns > 0.0;
      if (!SaveBaseline(baseline_path, record)) {
        std::cerr << "failed to save baseline: " << baseline_path << '\n';
        return 1;
      }
      continue;
    }

    if (!scenario.baseline_gated) {
      std::cerr << "[perf] advisory-only scenario completed without baseline enforcement: "
                << scenario.name << '\n';
      continue;
    }

    const auto baseline = LoadBaseline(baseline_path);
    if (!baseline.has_value()) {
      std::cerr << "missing baseline: " << baseline_path << '\n';
      if (!options->smoke) {
        all_passed = false;
      }
      continue;
    }
    const BaselineComparison comparison = CompareToBaseline(*baseline, *aggregate);
    // A windowed run measures window-system present cost the baselines never
    // recorded, so its wall numbers are not comparable. Allocation counts are:
    // they came out byte-identical across the dummy and x11 lanes on every
    // scenario in the suite. Enforce the half that is still meaningful and
    // annotate the other half rather than reporting a suite-wide wall failure
    // that says nothing about the code.
    bool enforced_failure = false;
    for (const MetricComparison& metric : comparison.metrics) {
      if (metric.passed) {
        continue;
      }
      if (windowed_lane && metric.metric.find("wall") != std::string::npos) {
        continue;
      }
      enforced_failure = true;
    }
    // Print a per-scenario verdict so a tripped gate is self-diagnosing: which
    // scenario, which metric, baseline vs measured, the delta, and the tolerance
    // it blew. Without this a failing run only surfaced a bare exit code and the
    // offending metric had to be reverse-engineered from --report-json. Smoke
    // runs do not enforce, so they annotate the line as advisory.
    const char* verdict = !enforced_failure ? "PASS" : (options->smoke ? "WARN" : "FAIL");
    // Reported on every verdict line that earns it, not only on failures: which
    // scenarios run on a clock that moves under them is exactly the audit
    // TD-2026-08-05-137 says nobody has done, and this way it falls out of an
    // ordinary run instead of needing its own sweep.
    const CalibrationSpread calibration = MeasureCalibrationSpread(*aggregate);
    const bool clock_moved = calibration.valid && calibration.ratio >= kCalibrationSpreadNoteRatio;
    const std::string calibration_note =
        clock_moved ? DescribeCalibrationSpread(calibration) : std::string{};
    // Say when the CPU gate was compared in the baseline's machine state rather
    // than in this run's. A normalisation that is invisible is a gate nobody can
    // audit: it is the difference between "the CPU numbers agree" and "the CPU
    // numbers agree after being scaled by 1.4x".
    const std::string clock_note = [&]() -> std::string {
      if (!comparison.clock.applied) {
        return {};
      }
      const double deviation = std::abs(comparison.clock.factor - 1.0);
      if (deviation < 0.05 && !comparison.clock.clamped) {
        return {};
      }
      std::ostringstream note;
      note << "  [cpu normalised for machine clock: this run measured "
           << comparison.clock.factor << "x the baseline's calibration"
           << (comparison.clock.clamped ? ", CLAMPED — probe reading is out of range, treat the cpu"
                                          " gate as unenforced"
                                        : "")
           << "]";
      return note.str();
    }();
    std::cerr << "[perf] " << verdict << ' ' << scenario.name << " (p50_wall="
              << aggregate->metrics.p50_wall_ms << "ms, p50_alloc="
              << aggregate->metrics.p50_allocations << ")" << clock_note << calibration_note
              << "\n";
    if (!comparison.passed) {
      for (const MetricComparison& metric : comparison.metrics) {
        if (metric.passed) {
          continue;
        }
        const bool advisory_metric =
            windowed_lane && metric.metric.find("wall") != std::string::npos;
        const double delta_percent =
            metric.expected != 0.0 ? (metric.actual / metric.expected - 1.0) * 100.0 : 0.0;
        // Duration metrics only. An allocation or RSS gate is unaffected by the
        // clock, so annotating those would be actively misleading — allocation
        // counts coming out identical across a clock step is the *evidence* that
        // a cpu/wall failure is the machine and not the code.
        const bool duration_metric = metric.metric.find("cpu") != std::string::npos ||
                                     metric.metric.find("wall") != std::string::npos;
        // A normalised metric prints both numbers. Reporting only the normalised
        // one hides that the gate did arithmetic; reporting only the raw one makes
        // the percentage next to it unreproducible.
        const bool normalized = metric.actual != metric.raw_actual;
        std::cerr << "[perf]   " << metric.metric << ": baseline=" << metric.expected
                  << " measured=" << metric.actual;
        if (normalized) {
          std::cerr << " (raw " << metric.raw_actual << ", clock-normalised)";
        }
        std::cerr << " (" << (delta_percent >= 0.0 ? "+" : "") << delta_percent
                  << "%, tolerance +" << metric.tolerance_percent << "%)"
                  << (advisory_metric ? "  [advisory: windowed video lane, not comparable]" : "")
                  << (clock_moved && duration_metric ? calibration_note : std::string{}) << '\n';
      }
    }
    if (enforced_failure && !options->smoke) {
      all_passed = false;
    }
  }

  if (selected_count == 0) {
    std::cerr << "no scenarios selected\n";
    return 1;
  }

  // Resolve report metadata once, after harness has had a chance to set SDL_*
  // environment hints. SDL was already torn down per-scenario, so re-read from
  // env rather than calling SDL_GetCurrentVideoDriver.
  ReportMetadata metadata;
  metadata.runner_class =
      options->reference_runner.value_or(std::string("local-advisory"));
  // The GPU and windowed lanes are always advisory regardless of
  // --reference-runner: only the software-renderer + dummy-video reference lane
  // can be authoritative.
  metadata.cpu_affinity = affinity.description;
  metadata.provenance =
      !gpu_lane && !windowed_lane && !unpinned_lane && options->reference_runner.has_value() &&
              *options->reference_runner == std::string("perf-runner-v1")
          ? std::string("reference")
          : std::string("advisory");
  // Report the video lane actually measured, read back from SDL. Reading
  // SDL_VIDEODRIVER out of the environment described what was *requested* and
  // said "default" for the common case, which is precisely the information a
  // reviewer needs to spot a windowed run.
  const std::string resolved_video = PerfHarness::ResolvedVideoDriver();
  metadata.sdl_video_driver =
      resolved_video.empty() ? run_options.video_driver : resolved_video;
  // Report the backend actually measured (the GPU lane resolves e.g. "opengl").
  const std::string resolved_renderer = PerfHarness::ResolvedRendererDriver();
  metadata.sdl_renderer_driver =
      resolved_renderer.empty() ? run_options.renderer_driver : resolved_renderer;
  metadata.scenarios.reserve(aggregates.size());
  for (const Aggregate& aggregate : aggregates) {
    metadata.scenarios.push_back(aggregate.scenario_name);
  }
  metadata.iterations = options->iterations;
  metadata.layout_mode = options->layout_mode.value_or(std::string{});
  {
    const char* seed_env = std::getenv("MICROIDE_PERF_SEED");
    if (seed_env != nullptr && seed_env[0] != '\0') {
      const auto parsed = microide::util::ParseSize(seed_env);
      if (parsed.has_value()) {
        metadata.seed = static_cast<std::uint64_t>(*parsed);
      }
    }
    if (metadata.seed == 0) {
      metadata.seed = 1337ULL;
    }
  }
  // Note: per-scenario sandboxes are cleaned at shutdown by default, so this
  // path is only meaningful with --keep-artifacts (then it points at the
  // last scenario's sandbox).
  if (options->keep_artifacts) {
    metadata.isolated_app_root = "(retained per-scenario, see stderr for paths)";
  } else {
    metadata.isolated_app_root = "(per-scenario isolated, cleaned at shutdown)";
  }

  // Advisory banner so reviewers can distinguish local runs from gate runs.
  if (metadata.provenance == "advisory") {
    std::cerr << "[perf] advisory run (runner_class=" << metadata.runner_class
              << ", video=" << metadata.sdl_video_driver
              << ", cpus=" << metadata.cpu_affinity
              << "); not authoritative for baseline updates\n";
  }

  if (options->report_text.has_value()) {
    std::ofstream out(*options->report_text);
    if (out) {
      out << "# microide_perf report\n";
      out << "runner_class=" << metadata.runner_class
          << " provenance=" << metadata.provenance << "\n";
      out << "sdl_video_driver=" << metadata.sdl_video_driver
          << " renderer_driver=" << metadata.sdl_renderer_driver << "\n";
      out << "iterations=" << metadata.iterations << " seed=" << metadata.seed
          << " layout_mode=" << (metadata.layout_mode.empty() ? "(default)"
                                                              : metadata.layout_mode)
          << "\n";
      out << "cpu_affinity=" << metadata.cpu_affinity << "\n";
    out << "isolated_app_root=" << metadata.isolated_app_root << "\n";
      out << "scenarios=";
      for (std::size_t i = 0; i < metadata.scenarios.size(); ++i) {
        if (i != 0) {
          out << ',';
        }
        out << metadata.scenarios[i];
      }
      out << "\n\n";
      for (const Aggregate& aggregate : aggregates) {
        out << aggregate.scenario_name << " p50=" << aggregate.metrics.p50_wall_ms
            << "ms p95=" << aggregate.metrics.p95_wall_ms << "ms max="
            << aggregate.metrics.max_wall_ms << "ms\n";
        const auto totals = AggregateCounterTotals(aggregate);
        if (!totals.empty()) {
          out << "  counters:";
          const std::size_t limit = std::min<std::size_t>(totals.size(), 12);
          for (std::size_t i = 0; i < limit; ++i) {
            out << (i == 0 ? " " : ", ") << totals[i].first << "=" << totals[i].second;
          }
          if (totals.size() > limit) {
            out << ", ...";
          }
          out << "\n";
        }
      }
    }
  }

  if (options->report_json.has_value()) {
    microide::util::JsonArray scenarios_json;
    scenarios_json.reserve(aggregates.size());
    for (const Aggregate& aggregate : aggregates) {
      scenarios_json.push_back(ToJson(aggregate));
    }
    microide::util::JsonArray scenario_names_json;
    scenario_names_json.reserve(metadata.scenarios.size());
    for (const std::string& name : metadata.scenarios) {
      scenario_names_json.push_back(microide::util::JsonValue(name));
    }
    microide::util::JsonObject metadata_json{
        {"runner_class", metadata.runner_class},
        {"provenance", metadata.provenance},
        {"sdl_video_driver", metadata.sdl_video_driver},
        {"sdl_renderer_driver", metadata.sdl_renderer_driver},
        {"cpu_affinity", metadata.cpu_affinity},
        {"iterations", static_cast<std::int64_t>(metadata.iterations)},
        {"layout_mode", metadata.layout_mode},
        {"seed", static_cast<std::int64_t>(metadata.seed)},
        {"scenarios", std::move(scenario_names_json)},
        {"isolated_app_root", metadata.isolated_app_root},
    };
    microide::util::JsonObject report_json{
        {"metadata", std::move(metadata_json)},
        {"scenarios", std::move(scenarios_json)},
    };
    std::ofstream out(*options->report_json);
    if (out) {
      out << microide::util::SerializeJson(
                 microide::util::JsonValue(std::move(report_json)));
    }
  }

  return all_passed ? 0 : 1;
}
