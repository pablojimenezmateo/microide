#include "perf/Baseline.h"
#include "perf/PerfHarness.h"

#include <algorithm>
#include <chrono>
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
#include <vector>

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
  std::size_t iterations = 10;
  std::optional<std::filesystem::path> report_json;
  std::optional<std::filesystem::path> report_text;
  std::optional<std::string> reference_runner;
};

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
    const std::uint64_t utime = std::stoull(tokens[11]);
    const std::uint64_t stime = std::stoull(tokens[12]);
    sample.cpu_ticks = utime + stime;
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
    return std::nullopt;
  }
  return options;
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
      .run =
          [](ScenarioContext& context) {
            const std::vector<std::filesystem::path> projects = {
                "tests/perf/fixtures/small_project",
                "tests/perf/fixtures/large_project",
                "tests/perf/fixtures/kernel_sized_project",
                "tests/fixtures/diff/simple",
                "tests/fixtures/large/plain",
            };
            for (int pass = 0; pass < 3; ++pass) {
              for (const auto& project : projects) {
                (void)context.Open(project);
                context.PumpFrames(2);
              }
            }
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "multi_tab_cycle",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            for (int i = 1; i <= 20; ++i) {
              const auto file = std::filesystem::path("tests/perf/fixtures/large_project") /
                                ("pkg0/file_" + std::to_string(i) + ".txt");
              context.OpenTab(file);
              context.PumpFrames(1);
            }
            for (int cycle = 0; cycle < 10; ++cycle) {
              for (SDL_Keycode digit : {SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5,
                                        SDLK_6, SDLK_7, SDLK_8, SDLK_9, SDLK_0}) {
                context.KeyDown(digit, SDL_KMOD_ALT);
                context.PumpFrames(1);
              }
            }
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "typing_small_file",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
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
      .run =
          [](ScenarioContext& context) {
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
      .run =
          [](ScenarioContext& context) {
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "project_search_literal",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
            (void)context.ExecuteCommand("search node_0001");
            context.Wait(std::chrono::milliseconds(50));
            context.ConsumeProjectSearchUpdates();
            context.PumpFrames(2);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "project_search_regex",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/kernel_sized_project");
            context.ToggleProjectSearchPatternMode();
            (void)context.ExecuteCommand("search node_0[0-9]+");
            context.Wait(std::chrono::milliseconds(50));
            context.ConsumeProjectSearchUpdates();
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
            (void)context.Open(project);
            context.OpenTab(source);
            context.Type(" // save to trigger lint");
            (void)context.ExecuteCommand("save");
            (void)context.WaitForDiagnostics(source, std::chrono::milliseconds(1000));
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "chat_pane_long_transcript",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.ExecuteCommand("chat");
            context.Type(
                "Long transcript fixture: alpha beta gamma delta epsilon zeta eta theta iota "
                "kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega. "
                "Long transcript fixture: alpha beta gamma delta epsilon zeta eta theta iota "
                "kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega.");
            context.PumpFrames(3);
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
  PerfHarness::RegisterScenario(Scenario{
      .name = "idle_soak_30s",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            context.PumpFrames(2);
            // Task 9.6: assert watcher/executor threads generate zero wake events after settling.
            // Allow 3 s for background work to settle before the soak window begins.
            context.Wait(std::chrono::seconds(3));
            const std::uint64_t wakeups_during_soak =
                context.Wait(std::chrono::seconds(27));
            if (wakeups_during_soak > 0) {
              std::cerr << "idle_soak_30s: " << wakeups_during_soak
                        << " unexpected wake events during soak window\n";
            }
            context.PumpFrames(1);
          },
  });
  // Task 9.2: file_finder_cold — open large fixture, measure time-to-first file-finder result.
  PerfHarness::RegisterScenario(Scenario{
      .name = "file_finder_cold",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!std::filesystem::is_directory(fixture)) {
              std::cerr << "file_finder_cold: fixture missing, skipping\n";
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("file_finder_cold: failed to open fixture");
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
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/git_status_project");
            if (!std::filesystem::is_directory(fixture)) {
              std::cerr << "git_sidebar_activate: fixture missing, skipping\n";
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("git_sidebar_activate: failed to open fixture");
            }
            context.PumpFrames(2);
            context.ActivateGitSidebar();
            context.PumpFrames(5);
          },
  });
  // Task 9.5: search_first_result — search for a symbol near end of 10k-file corpus.
  PerfHarness::RegisterScenario(Scenario{
      .name = "search_first_result",
      .smoke = false,
      .run =
          [](ScenarioContext& context) {
            const std::filesystem::path fixture =
                std::filesystem::path("tests/perf/fixtures/file_finder_large");
            if (!std::filesystem::is_directory(fixture)) {
              std::cerr << "search_first_result: fixture missing, skipping\n";
              return;
            }
            if (!context.Open(fixture)) {
              throw std::runtime_error("search_first_result: failed to open fixture");
            }
            context.PumpFrames(2);
            context.StartSearch("symbol_09999");
            context.PumpFrames(10);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "cold_startup_small_project",
      .smoke = true,
      .run =
          [](ScenarioContext& context) {
            (void)context.Open("tests/perf/fixtures/small_project");
            context.PumpFrames(5);
          },
  });
  PerfHarness::RegisterScenario(Scenario{
      .name = "cold_startup_large_project",
      .smoke = true,
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
}

util::JsonValue ToJson(const Aggregate& aggregate) {
  util::JsonArray iterations_json;
  iterations_json.reserve(aggregate.iterations.size());
  for (const Iteration& iteration : aggregate.iterations) {
    iterations_json.push_back(util::JsonObject{
        {"index", static_cast<std::int64_t>(iteration.index)},
        {"wall_ms", iteration.metrics.wall_ms},
        {"allocations", static_cast<std::int64_t>(iteration.metrics.allocations)},
        {"frees", static_cast<std::int64_t>(iteration.metrics.frees)},
        {"bytes_allocated", static_cast<std::int64_t>(iteration.metrics.bytes_allocated)},
        {"bytes_freed", static_cast<std::int64_t>(iteration.metrics.bytes_freed)},
    });
  }
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
                  }},
      {"iterations", std::move(iterations_json)},
  };
}

}  // namespace
}  // namespace microide::tests::perf

int main(int argc, char** argv) {
  using namespace microide::tests::perf;
  RegisterBuiltInScenarios();
  const std::optional<CliOptions> options = ParseCli(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: microide_perf [--scenarios=a,b] [--update-baseline] [--smoke] "
                 "[--iterations=N] [--report-json=path] [--report-text=path] "
                 "[--reference-runner=name]\n";
    return 1;
  }

  PerfHarness::RunOptions run_options;
  run_options.scenario_names = options->scenarios;
  run_options.smoke_only = options->smoke;
  run_options.iterations = options->iterations;
  std::vector<Aggregate> aggregates;
  bool all_passed = true;
  std::size_t selected_count = 0;
  {
    std::string fixture_error;
    if (!PerfHarness::VerifyFixtureTree("tests/perf/fixtures/kernel_sized_project",
                                        "tests/perf/fixtures/kernel_sized_project.sha256",
                                        &fixture_error)) {
      std::cerr << fixture_error << '\n';
      return 1;
    }
  }

  std::size_t run_index = 0;
  for (const Scenario& scenario : PerfHarness::RegisteredScenarios()) {
    PerfHarness::RunOptions probe = run_options;
    const bool name_selected =
        probe.scenario_names.empty() ||
        std::find(probe.scenario_names.begin(), probe.scenario_names.end(), scenario.name) !=
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

    const std::filesystem::path baseline_path =
        std::filesystem::path("tests/perf/baselines") / (scenario.name + ".json");
    if (options->update_baseline) {
      BaselineRecord record{
          .scenario_name = scenario.name,
          .metrics = aggregate->metrics,
      };
      if (!SaveBaseline(baseline_path, record)) {
        std::cerr << "failed to save baseline: " << baseline_path << '\n';
        return 1;
      }
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
    if (!comparison.passed && !options->smoke) {
      all_passed = false;
    }
  }

  if (selected_count == 0) {
    std::cerr << "no scenarios selected\n";
    return 1;
  }

  if (options->report_text.has_value()) {
    std::ofstream out(*options->report_text);
    if (out) {
      for (const Aggregate& aggregate : aggregates) {
        out << aggregate.scenario_name << " p50=" << aggregate.metrics.p50_wall_ms
            << "ms p95=" << aggregate.metrics.p95_wall_ms << "ms max="
            << aggregate.metrics.max_wall_ms << "ms\n";
      }
    }
  }

  if (options->report_json.has_value()) {
    microide::util::JsonArray all;
    all.reserve(aggregates.size());
    for (const Aggregate& aggregate : aggregates) {
      all.push_back(ToJson(aggregate));
    }
    std::ofstream out(*options->report_json);
    if (out) {
      out << microide::util::SerializeJson(microide::util::JsonValue(std::move(all)));
    }
  }

  return all_passed ? 0 : 1;
}
