#include "perf/AllocationCounter.h"
#include "perf/Baseline.h"
#include "perf/PerfCpuAffinity.h"
#include "workspace/NativeDialogTestMode.h"
#include "perf/PerfHarness.h"
#include "perf/PerfHarnessIsolation.h"
#include "perf/ScenarioProcessIsolation.h"

#include "editor/BracketScanner.h"
#include "editor/DiagnosticsStore.h"
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
#include <ctime>
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

#include <unistd.h>

#include "util/JsonValue.h"
#include "util/Parse.h"

namespace microide::tests::perf {
namespace {

inline constexpr std::size_t kDefaultIterations = 10;

struct CliOptions {
  std::vector<std::string> scenarios;
  bool update_baseline = false;
  bool update_deterministic_only = false;
  bool smoke = false;
  bool print_build_config = false;
  bool require_fixtures = false;
  bool keep_artifacts = false;
  std::size_t iterations = kDefaultIterations;
  std::optional<std::filesystem::path> report_json;
  std::optional<std::filesystem::path> report_text;
  std::optional<std::string> reference_runner;
  std::optional<std::string> layout_mode;
  std::string renderer_driver = "software";
  std::string video_driver = "dummy";
  std::string pin_cores = "auto";
  bool isolate_scenarios = true;
};

bool g_require_fixtures = false;

struct ProcessSample {
  std::uint64_t rss_bytes = 0;
  std::uint64_t cpu_ticks = 0;
};

ProcessSample ReadProcessSample() {
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
    const std::optional<std::int64_t> utime = util::ParseInt64(tokens[11]);
    const std::optional<std::int64_t> stime = util::ParseInt64(tokens[12]);
    if (utime.has_value() && stime.has_value() && *utime >= 0 && *stime >= 0) {
      sample.cpu_ticks = static_cast<std::uint64_t>(*utime) + static_cast<std::uint64_t>(*stime);
    }
  }
  return sample;
}

double CpuPercentFromSamples(const ProcessSample& before,
                             const ProcessSample& after,
                             std::chrono::milliseconds elapsed) {
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
    if (arg.rfind("--update-baseline=", 0) == 0) {
      const std::string value = arg.substr(std::string("--update-baseline=").size());
      if (value != "all" && value != "deterministic") {
        return std::nullopt;
      }
      options.update_baseline = true;
      options.update_deterministic_only = value == "deterministic";
      continue;
    }
    if (arg == "--build-config") {
      options.print_build_config = true;
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
    if (arg == "--no-isolate") {
      options.isolate_scenarios = false;
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "multi_project_switch",
      .smoke = true,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .gate_net_heap_metrics = false,
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
      .warmup_iterations = 1,
      .gate_net_heap_metrics = false,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.PumpFrames(2);
            std::vector<std::filesystem::path> files;
            files.reserve(20);
            for (int i = 1; i <= 20; ++i) {
              files.push_back(std::filesystem::path("tests/perf/fixtures/large_project") /
                              ("pkg0/file_" + std::to_string(i) + ".txt"));
            }
            context.Measure("multi_tab.open_tabs", [&]() {
              for (const std::filesystem::path& file : files) {
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "typing_large_file",
      .smoke = true,
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "scroll_large_file",
      .smoke = true,
      .warmup_iterations = 1,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .measurement_revision = 2,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/large_project");
            context.OpenTab("tests/perf/fixtures/large_project/pkg0/file_1.txt");
            context.Measure("scroll_large_file.scroll_burst", [&] {
              for (int i = 0; i < 40; ++i) {
                context.Scroll(-1);
                context.PumpFrames(1);
              }
              for (int i = 0; i < 20; ++i) {
                context.KeyDown(SDLK_PAGEDOWN);
                context.PumpFrames(1);
              }
            });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_tab_derived_cache_residency",
      .smoke = false,
      .baseline_gated = false,
      .run =
          [](ScenarioContext& context) {
            using TA = microide::workspace::WorkspaceShell::TestAccess;
            (void)TA::SetSettingValue(context.Shell(), "editor.fold.enabled", "true");
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(8);
            (void)TA::EnsureActiveFoldingModelFresh(context.Shell());
            for (int i = 0; i < 320; ++i) {
              context.KeyDown(SDLK_PAGEDOWN);
              context.PumpFrames(1);
            }
            for (int i = 0; i < 24; ++i) {
              context.Type("x");
              context.PumpFrames(1);
            }
            const TA::DerivedCacheResidency residency =
                TA::EditorDerivedCacheResidency(context.Shell());
            if (residency.editor_tabs == 0) {
              throw std::runtime_error(
                  "editor_tab_derived_cache_residency: no editor tab was open, so the "
                  "reported bytes describe nothing");
            }
            if (residency.total() == 0) {
              throw std::runtime_error(
                  "editor_tab_derived_cache_residency: a warmed 50k-line tab reported "
                  "zero retained bytes, which means the accounting is not reaching the "
                  "caches");
            }
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_scroll_fresh_content_large",
      .smoke = false,
      .baseline_gated = true,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(8);
            microide::editor::TextViewport& viewport = context.ActiveViewport();
            viewport.MoveCursorTo(0, 0, false);
            viewport.SetScrollLine(0);
            context.PumpFrames(1);
            std::vector<double> samples_us;
            samples_us.reserve(640);
            context.Measure("scroll_fresh_content.page_down_sweep", [&] {
              for (int i = 0; i < 640; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.KeyDown(SDLK_PAGEDOWN);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
            if (context.ActiveViewport().scroll_line() < 12000) {
              throw std::runtime_error(
                  "editor_scroll_fresh_content_large: the page-down sweep did not scroll "
                  "through fresh rows");
            }
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
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
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
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
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
      .measurement_revision = 2,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path project = "tests/perf/fixtures/linter_project";
            const std::filesystem::path source = project / "src" / "index.js";
            FixtureRestoreGuard restore_guard(source, ReadFileTextOrThrow(source));
            (void)context.Open(project);
            context.OpenTab(source);
            context.Measure("linter.type_invalid_edit", [&]() { context.Type("\nconst broken = ;"); });
            context.Measure("linter.save", [&]() { (void)context.ExecuteCommand("save"); });
            std::vector<editor::Diagnostic> diagnostics;
            diagnostics.reserve(24);
            for (std::size_t i = 0; i < 24; ++i) {
              editor::Diagnostic diagnostic;
              diagnostic.range.start.line = i * 2 + 1;
              diagnostic.range.start.column = 3;
              diagnostic.range.end.line = i * 2 + 1;
              diagnostic.range.end.column = 12;
              diagnostic.severity = (i % 3 == 0) ? editor::DiagnosticSeverity::Error
                                                 : editor::DiagnosticSeverity::Warning;
              diagnostic.message = "perf fixture diagnostic";
              diagnostics.push_back(std::move(diagnostic));
            }
            context.Measure("linter.publish_diagnostics", [&]() {
              context.PublishDiagnostics("linter", source, diagnostics);
              context.PumpFrames(3);
            });
            if (!context.HasDiagnostics(source)) {
              context.SkipScenario("linter_on_save: published diagnostics did not reach the store");
            }
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_scroll_long_output",
      .smoke = true,
      .warmup_iterations = 1,
      .measurement_revision = 2,
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
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_stream_chunked_output",
      .smoke = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.Measure("terminal.open", [&]() { context.OpenTerminal("perf-stream"); });
            std::string output;
            output.reserve(12000 * 24);
            for (int i = 0; i < 12000; ++i) {
              output += "perf-stream-line ";
              output += std::to_string(i);
              output += "\r\n";
            }
            constexpr std::size_t kPtyChunkBytes = 8192;
            context.Measure("terminal.stream_chunked", [&]() {
              for (std::size_t offset = 0; offset < output.size(); offset += kPtyChunkBytes) {
                context.FeedTerminalOutput(
                    std::string_view(output).substr(offset, kPtyChunkBytes));
              }
            });
            context.PumpFrames(2);
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "output_channel_stream_paint",
      .smoke = false,
      .baseline_gated = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.AppendOutputLine("perf.output", "perf output");
            context.ShowOutputChannel("perf.output");
            context.PumpFrames(2);
            const workspace::WorkspaceShell::RenderInvalidation frame_trigger{
                .full = false, .rects = {SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f}}};
            const auto lines = [] {
              std::vector<std::string> built;
              for (int line = 0; line < 40 * 8; ++line) {
                built.push_back("perf-output-line " + std::to_string(line) +
                                " src/main.cpp:12:5: note: streamed by the scenario");
              }
              return built;
            };
            std::vector<std::string> shown = lines();
            std::vector<std::string> hidden = lines();
            const auto stream = [&](std::string_view channel, std::vector<std::string>& batch) {
              for (std::size_t burst = 0; burst < 40; ++burst) {
                for (std::size_t line = 0; line < 8; ++line) {
                  context.AppendOutputLine(channel, std::move(batch[burst * 8 + line]));
                }
                context.PumpPartialFrame(frame_trigger);
              }
            };
            context.Measure("output.stream_shown", [&]() { stream("perf.output", shown); });
            context.Measure("output.stream_hidden", [&]() { stream("perf.hidden", hidden); });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "debug_pane_step_paint",
      .smoke = false,
      .baseline_gated = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            constexpr std::size_t kValues = 40;
            (void)context.Open("tests/perf/fixtures/small_project");
            context.ShowDebugVariables(kValues);
            context.PumpFrames(2);
            const workspace::WorkspaceShell::RenderInvalidation frame_trigger{
                .full = false, .rects = {SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f}}};
            const std::vector<ScenarioContext::DebugValues> steps = ScenarioContext::DebugVariableSteps(kValues, 40);
            const auto step = [&]() {
              for (const ScenarioContext::DebugValues& values : steps) {
                context.ApplyDebugVariables(values);
                context.PumpPartialFrame(frame_trigger);
              }
            };
            context.ShowDebugPaneVariables(true);
            context.PumpFrames(1);
            context.Measure("debug.step_shown", [&]() { step(); });
            context.ShowDebugPaneVariables(false);
            context.PumpFrames(1);
            context.Measure("debug.step_hidden", [&]() { step(); });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_mouse_owner_hover",
      .smoke = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.OpenTerminal("perf-mouse-owner");
            std::string screen = "\x1b[?1049h\x1b[?1003h\x1b[?1006h";
            for (int row = 1; row <= 20; ++row) {
              screen += "\x1b[" + std::to_string(row) + ";1H transcript line " + std::to_string(row);
            }
            screen += "\x1b[22;40H\x1b[38;2;120;120;120m Jump to bottom (ctrl+End) \x1b[0m";
            context.FeedTerminalOutput(screen);
            context.PumpFrames(2);
            auto& shell = context.Shell();
            const SDL_FPoint start = workspace::WorkspaceShell::TestAccess::TerminalCellPoint(shell, 21, 2);
            const SDL_FPoint end = workspace::WorkspaceShell::TestAccess::TerminalCellPoint(shell, 21, 62);
            const SDL_FPoint next = workspace::WorkspaceShell::TestAccess::TerminalCellPoint(shell, 21, 3);
            const float step = (next.x - start.x) * 0.5f;
            context.Measure("terminal.mouse_owner_hover_320_moves", [&]() {
              float x = start.x;
              for (int i = 0; i < 320; ++i) {
                const auto result = context.MouseMoveHover(x, start.y);
                context.PumpPartialFrame(result.redraw);
                if (i == 160) {
                  context.FeedTerminalOutput(
                      "\x1b[22;40H\x1b[38;2;255;255;255;48;2;70;70;70m Jump to bottom (ctrl+End) \x1b[0m");
                }
                x += step;
                if (x > end.x) {
                  x = start.x;
                }
              }
            });
            context.PumpFrames(2);
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_alt_screen_toggle",
      .smoke = false,
      .warmup_iterations = 1,
      .measurement_revision = 2,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.Measure("terminal.open", [&]() { context.OpenTerminal("perf-alt-screen"); });
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
      .name = "terminal_paint_colored_selection",
      .smoke = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.OpenTerminal("perf-paint");
            std::string output;
            output.reserve(160 * 400);
            for (int line = 0; line < 160; ++line) {
              for (int word = 0; word < 16; ++word) {
                output += "\x1b[3";
                output += static_cast<char>('0' + (line + word) % 8);
                if (word % 3 == 0) {
                  output += ";4";
                  output += static_cast<char>('0' + (word + 1) % 8);
                } else {
                  output += ";49";
                }
                output += "mword";
                output += std::to_string(word);
                output += ' ';
              }
              output += "\x1b[0m\r\n";
            }
            context.FeedTerminalOutput(output);
            context.PumpFrames(2);
            context.Measure("terminal.paint_colored", [&]() { context.PumpFrames(10); });
            (void)context.ExecuteCommand("term-select-all");
            context.PumpFrames(1);
            context.Measure("terminal.paint_selected", [&]() { context.PumpFrames(10); });
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "terminal_echo_partial_frames",
      .smoke = false,
      .baseline_gated = false,
      .warmup_iterations = 1,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(2);
            context.OpenTerminal("perf-echo");
            std::string output;
            output.reserve(160 * 400);
            for (int line = 0; line < 160; ++line) {
              for (int word = 0; word < 16; ++word) {
                output += "\x1b[3";
                output += static_cast<char>('0' + (line + word) % 8);
                output += word % 3 == 0 ? ";44" : ";49";
                output += "mword";
                output += std::to_string(word);
                output += ' ';
              }
              output += "\x1b[0m\r\n";
            }
            output += "prompt$ ";
            context.FeedTerminalOutput(output);
            context.PumpFrames(2);
            workspace::WorkspaceShell::RenderInvalidation wake;
            wake.frame = true;
            context.Measure("terminal.echo_400_frames", [&]() {
              for (int key = 0; key < 200; ++key) {
                const char glyph = static_cast<char>('a' + key % 26);
                context.FeedTerminalOutput(std::string_view(&glyph, 1));
                context.PumpPartialFrame(wake);
                context.FeedTerminalOutput("\b \b");
                context.PumpPartialFrame(wake);
              }
            });
            context.CloseAllTerminals();
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "long_soak_8h",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
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
  constexpr double kIdleSoakCpuBudgetMs = 20.0;
  PerfHarness::RegisterScenario(Scenario{
      .name = "idle_soak_30s",
      .smoke = false,
      .warmup_iterations = 1,
      .gate_cpu_metrics = false,
      .run =
          [](ScenarioContext& context) {
            context.PumpFrames(2);
            context.PrimeEditorEssentialsIdleSoakSurface();
            context.Wait(std::chrono::seconds(3));
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "file_finder_cold",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!RequireFixture(context, fixture, "file_finder_cold")) {
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
            context.Measure("file_finder_cold.open_finder", [&] {
              context.OpenFileFinder();
              context.PumpFrames(1);
            });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "file_finder_type_query",
      .smoke = false,
      .warmup_iterations = 1,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!RequireFixture(context, fixture, "file_finder_type_query")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("file_finder_type_query: failed to open fixture");
            }
            if (!context.WaitForFileIndexPath("src_49/file_09999.cpp",
                                              std::chrono::seconds(20))) {
              throw std::runtime_error(
                  "file_finder_type_query: file index did not finish building");
            }
            context.PumpFrames(2);
            context.OpenFileFinder();
            context.PumpFrames(1);
            context.Measure("file_finder_type_query.type_and_rank", [&] {
              for (const char letter : std::string_view("file_09999")) {
                context.Type(std::string_view(&letter, 1));
                context.PumpFrames(1);
              }
            });
            context.Measure("file_finder_type_query.backspace_rescan", [&] {
              for (int i = 0; i < 4; ++i) {
                context.KeyDown(SDLK_BACKSPACE);
                context.PumpFrames(1);
              }
            });
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "git_sidebar_activate",
      .smoke = false,
      .warmup_iterations = 1,
      .measurement_revision = 2,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/git_status_project");
            if (!RequireFixture(context, fixture, "git_sidebar_activate")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("git_sidebar_activate: failed to open fixture");
            }
            context.PumpFrames(2);
            context.ActivateGitSidebar();
            workspace::WorkspaceShell::TestAccess::PerfRunGitSidebarRefreshSync(context.Shell());
            context.PumpFrames(5);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "project_search_start_after_index_change",
      .smoke = false,
      .warmup_iterations = 4,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!RequireFixture(context, fixture, "project_search_start_after_index_change")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error(
                  "project_search_start_after_index_change: failed to open fixture");
            }
            if (!context.WaitForFileIndexPath("src_49/file_09999.cpp",
                                              std::chrono::seconds(15))) {
              throw std::runtime_error(
                  "project_search_start_after_index_change: file index did not build in time");
            }
            context.StartSearch("symbol_00001");
            if (!context.WaitForProjectSearchFinished(std::chrono::seconds(15))) {
              throw std::runtime_error(
                  "project_search_start_after_index_change: warm-up search did not finish");
            }
            platform::IndexUpdateBatch batch;
            batch.is_initial = false;
            platform::IndexUpdateBatch::Change change;
            change.kind = platform::IndexUpdateBatch::Kind::CreatedOrModified;
            change.entry.relative_path = "src_00/file_00000.cpp";
            change.entry.size = 0;
            change.entry.mtime = std::filesystem::file_time_type::clock::now();
            batch.changes.push_back(std::move(change));
            const std::uint64_t rebuilds_before =
                util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRebuilds);
            context.Measure("project_search.index_change_to_search_start", [&] {
              if (!workspace::WorkspaceShell::TestAccess::DispatchFileIndexWatcherBatchForTesting(
                      context.Shell(), std::move(batch))) {
                throw std::runtime_error(
                    "project_search_start_after_index_change: no live file-index watcher, so "
                    "the index cannot be invalidated the way a save does");
              }
              context.PumpFrames(1);
              context.StartSearch("symbol_09999");
            });
            if (util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRebuilds) ==
                rebuilds_before) {
              throw std::runtime_error(
                  "project_search_start_after_index_change: the change rebuilt no derived path "
                  "cache, so this phase is measuring a cache hit and not the work it is named "
                  "for");
            }
            if (!context.WaitForProjectSearchFinished(std::chrono::seconds(15))) {
              throw std::runtime_error(
                  "project_search_start_after_index_change: search did not finish in time");
            }
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "search_first_result",
      .smoke = false,
      .warmup_iterations = 16,
      .tolerance_p95_percent = tolerance::kJitterWallP95,
      .tolerance_max_percent = tolerance::kJitterWallMax,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!RequireFixture(context, fixture, "search_first_result")) {
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("search_first_result: failed to open fixture");
            }
            if (!context.WaitForFileIndexPath("src_49/file_09999.cpp",
                                              std::chrono::seconds(15))) {
              throw std::runtime_error("search_first_result: file index did not build in time");
            }
            bool search_finished = false;
            context.Measure("search_first_result.search_to_first_result", [&] {
              // perf-measure-waits-on-clock: WaitForProjectSearchFinished spins on a flag and allocates nothing
              context.StartSearch("symbol_09999");
              search_finished = context.WaitForProjectSearchFinished(std::chrono::seconds(15));
              context.PumpFrames(2);
            });
            if (!search_finished) {
              throw std::runtime_error("search_first_result: search did not finish in time");
            }
          },
  });
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
      .gate_net_heap_metrics = false,
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

            context.Measure("switch_and_idle.switch_and_settle", [&] {
              (void)context.Open(project_a);
              context.PumpFrames(1);
              (void)context.Open(project_b);
              context.PumpFrames(30);
            });
          },
  });

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
            context.Measure("editor_fold_viewport_refresh.scroll_frame", [&] {
              for (int i = 0; i < 96; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.Scroll(-2);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
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
            context.Measure("editor_sticky_scroll_scroll.fast_scroll_frame", [&] {
              for (int i = 0; i < 100; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.Scroll(-3);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
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
            context.Measure("editor_indent_guides_paint.scroll_paint_frame", [&] {
              for (int i = 0; i < 80; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.Scroll(((i % 7) & 1) != 0 ? 2 : -2);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
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
            context.Measure("editor_render_whitespace_paint.scroll_overlay_frame", [&] {
              for (int i = 0; i < 80; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.Scroll(-2);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
            EnforceP95Microseconds("editor_render_whitespace_paint.scroll_overlay_frame", samples_us,
                                   30'000.0);
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_scroll_only_no_content_bump",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            context.PumpFrames(20);
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
            context.Measure("editor_scroll_only_no_content_bump.scroll_frame", [&] {
              for (int i = 0; i < 100; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                context.Scroll((i % 2 == 0) ? -2 : 2);
                context.PumpFrames(1);
                const auto t1 = std::chrono::steady_clock::now();
                samples_us.push_back(
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
              }
            });
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "editor_bracket_match_caret_motion",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            OpenEditorEssentials50kCppOrThrow(context);
            microide::editor::TextViewport& vp = context.ActiveViewport();
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
            EnforceP95Microseconds("editor_auto_close_typing", samples_us, 200'000.0);
            context.PumpFrames(2);
          },
  });
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
            context.Measure("smart_indent.newline_undo_burst", [&] {
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
            });
            EnforceP95Microseconds("editor_smart_indent_typing", samples_us, 200'000.0);
            context.PumpFrames(2);
          },
  });
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

util::JsonValue ToJson(const Aggregate& aggregate,
                       const std::optional<BaselineComparison>& comparison) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  util::JsonArray iterations_json;
  iterations_json.reserve(aggregate.iterations.size());
  for (const Iteration& iteration : aggregate.iterations) {
    util::JsonObject phase_duration_json;
    util::JsonObject phase_metrics_json;
    std::unordered_map<std::string, Iteration::PhaseMetrics> phase_totals;
    std::vector<std::string> phase_order;
    for (const Iteration::PhaseMetrics& phase : iteration.phase_metrics) {
      auto [it, inserted] = phase_totals.emplace(phase.name, Iteration::PhaseMetrics{});
      if (inserted) {
        it->second.name = phase.name;
        phase_order.push_back(phase.name);
      }
      it->second.wall_ms += phase.wall_ms;
      it->second.allocations += phase.allocations;
      it->second.frees += phase.frees;
      it->second.bytes_allocated += phase.bytes_allocated;
      it->second.bytes_freed += phase.bytes_freed;
    }
    for (const std::string& name : phase_order) {
      const Iteration::PhaseMetrics& phase = phase_totals[name];
      phase_duration_json[name] = phase.wall_ms;
      phase_metrics_json[name] = util::JsonObject{
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
    iteration_json["net_heap_bytes"] = iteration.metrics.net_heap_bytes;
    iteration_json["phase_durations_ms"] = std::move(phase_duration_json);
    iteration_json["phase_metrics"] = std::move(phase_metrics_json);
    iteration_json["perf_counters"] = std::move(counters_json);
    iterations_json.push_back(std::move(iteration_json));
  }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  util::JsonValue baseline_json = util::JsonValue();
  if (comparison.has_value()) {
    util::JsonArray metrics_json;
    metrics_json.reserve(comparison->metrics.size());
    for (const MetricComparison& metric : comparison->metrics) {
      metrics_json.push_back(util::JsonObject{
          {"metric", metric.metric},
          {"expected", metric.expected},
          {"actual", metric.actual},
          {"raw_actual", metric.raw_actual},
          {"tolerance_percent", metric.tolerance_percent},
          {"delta_percent", MetricDeltaPercent(metric)},
          {"envelope_used_percent", EnvelopeUsedPercent(metric)},
          {"passed", metric.passed},
          {"enforced", metric.enforced},
          {"note", metric.note},
      });
    }
    baseline_json = util::JsonObject{
        {"gated", true},
        {"passed", comparison->passed},
        {"clock_normalization_applied", comparison->clock.applied},
        {"clock_normalization_factor", comparison->clock.factor},
        {"clock_normalization_clamped", comparison->clock.clamped},
        {"metrics", std::move(metrics_json)},
    };
  } else {
    baseline_json = util::JsonObject{{"gated", false}};
  }

  util::JsonArray phases_json;
  phases_json.reserve(aggregate.phases.size());
  for (const PhaseMetricSet& phase : aggregate.phases) {
    phases_json.push_back(util::JsonObject{
        {"name", phase.name},
        {"p50_allocations", phase.p50_allocations},
        {"max_allocations", phase.max_allocations},
        {"p50_wall_ms", phase.p50_wall_ms},
        {"iterations", static_cast<std::int64_t>(phase.iterations)},
        {"share_of_scenario_allocations",
         aggregate.metrics.p50_allocations > 0.0
             ? phase.p50_allocations / aggregate.metrics.p50_allocations
             : 0.0},
    });
  }

  return util::JsonObject{
      {"scenario", aggregate.scenario_name},
      {"smoke", aggregate.smoke},
      {"measurement_revision", static_cast<std::int64_t>(aggregate.measurement_revision)},
      {"baseline", std::move(baseline_json)},
      {"phases", std::move(phases_json)},
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
                      {"p50_net_heap_bytes", aggregate.metrics.p50_net_heap_bytes},
                      {"p50_cpu_calibration_ns", aggregate.metrics.p50_cpu_calibration_ns},
                  }},
      {"iterations", std::move(iterations_json)},
  };
}

}
}

int main(int argc, char** argv) {
  using namespace microide::tests::perf;
  microide::util::MarkTracingMainThread();
  setenv("MICROIDE_SEARCH_WORKER_LIMIT", "1", 0);
  microide::terminal::SetUsePlaceholderTerminalsForTesting(true);
  microide::workspace::SetNativeDialogsDisabledForTesting(true);
  {
    std::string isolate_error;
    if (EstablishIsolatedAppRoot(false, &isolate_error).empty()) {
      std::cerr << "[perf] failed to establish the process-level isolated app-root: "
                << isolate_error << "\n";
      return 1;
    }
  }
  RegisterBuiltInScenarios();
  const std::optional<CliOptions> options = ParseCli(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: microide_perf [--scenarios=a,b] "
                 "[--update-baseline[=all|deterministic]] [--smoke] "
                 "[--require-fixtures] [--keep-artifacts] [--iterations=N] "
                 "[--build-config] "
                 "[--report-json=path] [--report-text=path] "
                 "[--reference-runner=name] "
                 "[--layout-mode=auto|regular|compact] "
                 "[--renderer=software|auto|<sdl-driver>] "
                 "[--video=dummy|auto|<sdl-driver>] "
                 "[--pin-cores=auto|off|<cpu-list>] [--no-isolate]\n";
    return 1;
  }

  if (options->print_build_config) {
    std::cout << PerfBuildConfig() << '\n';
    return 0;
  }

  g_require_fixtures = options->require_fixtures;

  const CpuAffinityPlan affinity = ApplyPerfCpuAffinity(options->pin_cores);
  std::cerr << "[perf] cpu affinity: " << affinity.description << '\n';
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

  const bool gpu_lane = run_options.renderer_driver != "software";
  const bool isolate_scenarios = options->isolate_scenarios && ScenarioProcessIsolationAvailable();
  if (options->isolate_scenarios && !isolate_scenarios) {
    std::cerr << "[perf] per-scenario process isolation unavailable on this platform; "
                 "metrics carry the whole suite's process state (TD-2026-08-06-152)\n";
  }
  if (gpu_lane && options->update_baseline) {
    std::cerr << "--update-baseline is only valid on the software reference lane; "
                 "the GPU lane (--renderer=" << run_options.renderer_driver
              << ") is advisory and cannot write baselines\n";
    return 1;
  }
  const bool windowed_lane = run_options.video_driver != "dummy";
  if (windowed_lane && options->update_baseline) {
    std::cerr << "--update-baseline is only valid on the dummy video reference lane; "
                 "a windowed run (--video=" << run_options.video_driver
              << ") measures window-system present cost and is advisory\n";
    return 1;
  }
  const bool reference_lane = !gpu_lane && !windowed_lane && !unpinned_lane &&
                              options->reference_runner.has_value() &&
                              *options->reference_runner == std::string("perf-runner-v1");
  if (options->update_baseline && options->iterations < kDefaultIterations) {
    std::cerr << "--update-baseline requires at least " << kDefaultIterations
              << " iterations (got --iterations=" << options->iterations
              << "); a baseline recorded over a shorter run records the settling passes and "
                 "cannot be held by a default-length run\n";
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
  std::unordered_map<std::string, BaselineComparison> comparisons;
  bool all_passed = true;
  std::size_t selected_count = 0;
  if (options->require_fixtures) {
    static constexpr std::string_view kGeneratedFixtures[] = {
        "tests/perf/fixtures/kernel_sized_project",
        "tests/perf/fixtures/editor_essentials_50k_cpp",
        "tests/perf/fixtures/editor_essentials_50k_py",
    };
    for (const std::string_view fixture : kGeneratedFixtures) {
      if (!DirectoryExistsNoThrow(std::filesystem::path{fixture})) {
        std::cerr << "required fixture tree missing: " << fixture
                  << " (generate with tests/perf/generate_*.py, or drop --require-fixtures)\n";
        return 1;
      }
    }
  }
  if (std::string fixture_error;
      !PerfHarness::VerifyPresentFixtureTrees("tests/perf/fixtures", &fixture_error)) {
    std::cerr << fixture_error << '\n';
    return 1;
  }

  std::size_t run_index = 0;
  for (const Scenario& scenario : PerfHarness::RegisteredScenarios()) {
    PerfHarness::RunOptions probe = run_options;
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
    std::string isolation_error;
    std::optional<Aggregate> aggregate;
    if (isolate_scenarios && selected) {
      bool child_selected = true;
      aggregate = RunScenarioInChildProcess(scenario, run_options, &child_selected,
                                            &isolation_error);
      if (!aggregate.has_value() && !child_selected) {
        isolation_error.clear();
      }
    } else {
      aggregate = PerfHarness::RunScenario(scenario, run_options);
      isolation_error = PerfHarness::LastError();
    }
    if (!aggregate.has_value()) {
      if (selected) {
        std::cerr << "scenario failed to run: " << scenario.name << " (" << isolation_error
                  << ")\n";
        return 1;
      }
      continue;
    }
    if (!aggregate->skip_reason.empty()) {
      std::cerr << "[perf] SKIP " << scenario.name << " — " << aggregate->skip_reason << '\n';
      if (scenario.baseline_gated && !options->update_baseline) {
        std::cerr << "[perf] a baseline-gated scenario that cannot run is a failed run: its "
                     "committed baseline describes work this process did not do\n";
        all_passed = false;
      }
      continue;
    }
    aggregates.push_back(*aggregate);

    if (gpu_lane) {
      std::cerr << "[perf][gpu] " << scenario.name
                << " p50=" << aggregate->metrics.p50_wall_ms << "ms"
                << " p95=" << aggregate->metrics.p95_wall_ms << "ms\n";
      continue;
    }

    const std::filesystem::path baseline_path =
        std::filesystem::path("tests/perf/baselines") / (scenario.name + ".json");
    if (options->update_baseline) {
      if (!scenario.baseline_gated) {
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
                         .cpu_p50_percent = scenario.tolerance_p50_percent,
                         .cpu_p95_percent = scenario.tolerance_p95_percent,
                         .cpu_max_percent = scenario.tolerance_max_percent,
                         .rss_mean_percent = scenario.tolerance_rss_percent,
                         .net_heap_percent = scenario.tolerance_net_heap_percent,
                         .phase_alloc_p50_percent = resolve_alloc(
                             scenario.tolerance_alloc_p50_percent,
                             scenario.tolerance_p50_percent)},
      };
      record.phases = aggregate->phases;
      record.has_cpu_metrics = scenario.gate_cpu_metrics;
      record.has_rss_metrics = true;
      record.has_net_heap_metrics = scenario.gate_net_heap_metrics;
      record.has_calibration = aggregate->metrics.p50_cpu_calibration_ns > 0.0;
      record.iterations = aggregate->iterations.size();
      record.measurement_revision = scenario.measurement_revision;
      record.timing_is_advisory = !reference_lane;
      record.build_config = PerfBuildConfig();
      record.wall_spread_percent =
          aggregate->metrics.p50_wall_ms > 0.0
              ? std::max(0.0, (aggregate->metrics.p95_wall_ms - aggregate->metrics.p50_wall_ms) /
                                  aggregate->metrics.p50_wall_ms * 100.0)
              : 0.0;
      if (options->update_deterministic_only) {
        const std::optional<BaselineRecord> existing = LoadBaseline(baseline_path);
        if (!existing.has_value()) {
          BaselineRecord minted = record;
          minted.timing_is_advisory = true;
          if (!SaveBaseline(baseline_path, minted)) {
            std::cerr << "failed to save baseline: " << baseline_path << '\n';
            return 1;
          }
          std::cerr << "[perf] " << scenario.name
                    << ": minted a baseline with the timing half ADVISORY (allocations and net "
                       "heap gate now; rerun --update-baseline on the reference runner to arm "
                       "wall/cpu/rss)\n";
          continue;
        }
        const double prior_p50 = existing->metrics.p50_allocations;
        record = MergeDeterministicMetrics(*existing, record);
        const double delta = prior_p50 > 0.0
                                 ? (record.metrics.p50_allocations - prior_p50) / prior_p50 * 100.0
                                 : 0.0;
        std::cerr << "[perf] " << scenario.name << " p50_allocations " << prior_p50 << " -> "
                  << record.metrics.p50_allocations << " (" << (delta >= 0.0 ? "+" : "") << delta
                  << "%)\n";
      }
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
    comparisons[scenario.name] = comparison;
    bool enforced_failure = false;
    for (const MetricComparison& metric : comparison.metrics) {
      if (metric.passed || !metric.enforced) {
        continue;
      }
      if (windowed_lane && metric.metric.find("wall") != std::string::npos) {
        continue;
      }
      enforced_failure = true;
    }
    const char* verdict = !enforced_failure ? "PASS" : (options->smoke ? "WARN" : "FAIL");
    const CalibrationSpread calibration = MeasureCalibrationSpread(*aggregate);
    const bool clock_moved = calibration.valid && calibration.ratio >= kCalibrationSpreadNoteRatio;
    const std::string calibration_note =
        clock_moved ? DescribeCalibrationSpread(calibration) : std::string{};
    const std::string clock_note = [&]() -> std::string {
      if (!comparison.clock.applied) {
        return {};
      }
      const double deviation = std::abs(comparison.clock.factor - 1.0);
      if (deviation < 0.05 && !comparison.clock.clamped) {
        return {};
      }
      std::ostringstream note;
      note << "  [cpu+wall normalised for machine clock: this run measured "
           << comparison.clock.factor << "x the baseline's calibration"
           << (comparison.clock.clamped ? ", CLAMPED — probe reading is out of range, treat the cpu"
                                          " and wall gates as unenforced"
                                        : "")
           << "]";
      return note.str();
    }();
    std::string metric_notes;
    {
      std::vector<std::pair<std::string, std::string>> notes;
      for (const MetricComparison& metric : comparison.metrics) {
        if (metric.note.empty()) {
          continue;
        }
        std::string reason = metric.note;
        if (reason.rfind(metric.metric, 0) == 0) {
          reason = reason.substr(metric.metric.size());
          if (!reason.empty() && reason.front() == ' ') {
            reason.erase(0, 1);
          }
        }
        const auto existing = std::find_if(notes.begin(), notes.end(),
                                           [&](const auto& entry) { return entry.first == reason; });
        if (existing == notes.end()) {
          notes.emplace_back(reason, metric.metric);
        } else {
          existing->second += ", " + metric.metric;
        }
      }
      for (const auto& [reason, metrics] : notes) {
        metric_notes += "  [" + metrics + " " + reason + "]";
      }
    }
    std::cerr << "[perf] " << verdict << ' ' << scenario.name << " (p50_wall="
              << aggregate->metrics.p50_wall_ms << "ms, p50_alloc="
              << aggregate->metrics.p50_allocations << ")" << clock_note << calibration_note
              << metric_notes << "\n";
    if (!comparison.passed) {
      for (const MetricComparison& metric : comparison.metrics) {
        if (metric.passed || !metric.enforced) {
          continue;
        }
        const bool advisory_metric =
            windowed_lane && metric.metric.find("wall") != std::string::npos;
        const double delta_percent = MetricDeltaPercent(metric);
        const bool duration_metric = metric.metric.find("cpu") != std::string::npos ||
                                     metric.metric.find("wall") != std::string::npos;
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

  ReportMetadata metadata;
  metadata.runner_class =
      options->reference_runner.value_or(std::string("local-advisory"));
  metadata.cpu_affinity = affinity.description;
  metadata.provenance =
      !gpu_lane && !windowed_lane && !unpinned_lane && options->reference_runner.has_value() &&
              *options->reference_runner == std::string("perf-runner-v1")
          ? std::string("reference")
          : std::string("advisory");
  const std::string resolved_video = PerfHarness::ResolvedVideoDriver();
  metadata.sdl_video_driver =
      resolved_video.empty() ? run_options.video_driver : resolved_video;
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
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (gmtime_r(&now, &utc) != nullptr) {
      char buffer[32] = {};
      if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0) {
        metadata.timestamp_utc = buffer;
      }
    }
  }
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
  if (options->keep_artifacts) {
    metadata.isolated_app_root = "(retained per-scenario, see stderr for paths)";
  } else {
    metadata.isolated_app_root = "(per-scenario isolated, cleaned at shutdown)";
  }

  {
    struct HeadroomRow {
      const std::string* scenario = nullptr;
      const MetricComparison* metric = nullptr;
      double used = 0.0;
    };
    std::vector<HeadroomRow> deterministic;
    std::vector<HeadroomRow> machine_sensitive;
    for (const auto& [name, comparison] : comparisons) {
      for (const MetricComparison& metric : comparison.metrics) {
        if (!metric.passed) {
          continue;
        }
        if (!metric.enforced) {
          continue;
        }
        const double used = EnvelopeUsedPercent(metric);
        if (used < kEnvelopeNoticePercent) {
          continue;
        }
        (metric.metric.find("alloc") != std::string::npos ? deterministic : machine_sensitive)
            .push_back(HeadroomRow{&name, &metric, used});
      }
    }
    const auto by_used_desc = [](const HeadroomRow& a, const HeadroomRow& b) {
      return a.used > b.used;
    };
    std::sort(deterministic.begin(), deterministic.end(), by_used_desc);
    std::sort(machine_sensitive.begin(), machine_sensitive.end(), by_used_desc);
    const auto print_rows = [](const std::vector<HeadroomRow>& rows, const char* suffix) {
      for (const HeadroomRow& row : rows) {
        const double delta_percent = MetricDeltaPercent(*row.metric);
        std::cerr << "[perf]   " << *row.scenario << ' ' << row.metric->metric
                  << ": baseline=" << row.metric->expected << " measured=" << row.metric->actual
                  << " (" << (delta_percent >= 0.0 ? "+" : "") << delta_percent << "% of +"
                  << row.metric->tolerance_percent << "% allowed = " << row.used
                  << "% of envelope)" << suffix << '\n';
      }
    };
    if (!deterministic.empty()) {
      std::cerr << "[perf] HEADROOM: " << deterministic.size()
                << " allocation gate(s) passed while consuming >=" << kEnvelopeNoticePercent
                << "% of their envelope. Allocation counts are deterministic, so this is a code"
                   " change, not machine noise — investigate before rebaselining it away"
                   " (TD-2026-08-06-139).\n";
      print_rows(deterministic, "");
    }
    if (!machine_sensitive.empty()) {
      std::cerr << "[perf] headroom (duration/resident): " << machine_sensitive.size()
                << " gate(s) passed while consuming >=" << kEnvelopeNoticePercent
                << "% of their envelope. These carry machine state; corroborate against the"
                   " allocation counts before reading them as a regression.\n";
      print_rows(machine_sensitive, "  [machine-sensitive]");
    }
    if (deterministic.empty() && machine_sensitive.empty() && !comparisons.empty()) {
      std::cerr << "[perf] headroom: every gated metric stayed below " << kEnvelopeNoticePercent
                << "% of its envelope.\n";
    }
  }

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
          << " provenance=" << metadata.provenance
          << " timestamp_utc=" << metadata.timestamp_utc << "\n";
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
            << aggregate.metrics.max_wall_ms << "ms alloc_p50="
            << aggregate.metrics.p50_allocations << "\n";
        for (const PhaseMetricSet& phase : aggregate.phases) {
          out << "  phase " << phase.name << ": alloc_p50=" << phase.p50_allocations
              << " (" << (aggregate.metrics.p50_allocations > 0.0
                              ? phase.p50_allocations / aggregate.metrics.p50_allocations * 100.0
                              : 0.0)
              << "% of scenario) wall_p50=" << phase.p50_wall_ms << "ms\n";
        }
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
      const auto found = comparisons.find(aggregate.scenario_name);
      scenarios_json.push_back(ToJson(aggregate, found == comparisons.end()
                                                     ? std::optional<BaselineComparison>{}
                                                     : std::optional<BaselineComparison>{
                                                           found->second}));
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
        {"timestamp_utc", metadata.timestamp_utc},
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

  if (!isolate_scenarios) {
    Allocations::DumpTracedAllocationSites();
    microide::util::DumpPerformanceCountersOnce();
  }

  return all_passed ? 0 : 1;
}
