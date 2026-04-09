#include "project/ProjectSearchService.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using microide::project::ProjectSearchCaseMode;
using microide::project::ProjectSearchOptions;
using microide::project::ProjectSearchPatternMode;
using microide::project::ProjectSearchService;

struct BenchRun {
  double elapsed_ms = 0.0;
  std::size_t result_count = 0;
  bool truncated = false;
  bool finished = false;
  std::string error;
};

void PrintUsage() {
  std::cerr
      << "usage: microide_search_bench <project-root> <query> [--regex|--literal]"
      << " [--case=smart|sensitive|insensitive] [--hidden] [--runs=N]\n";
}

std::optional<ProjectSearchCaseMode> ParseCaseMode(std::string_view value) {
  if (value == "smart") {
    return ProjectSearchCaseMode::Smart;
  }
  if (value == "sensitive") {
    return ProjectSearchCaseMode::Sensitive;
  }
  if (value == "insensitive") {
    return ProjectSearchCaseMode::Insensitive;
  }
  return std::nullopt;
}

const char* PatternModeLabel(ProjectSearchPatternMode mode) {
  return mode == ProjectSearchPatternMode::Regex ? "regex" : "literal";
}

const char* CaseModeLabel(ProjectSearchCaseMode mode) {
  switch (mode) {
    case ProjectSearchCaseMode::Sensitive:
      return "sensitive";
    case ProjectSearchCaseMode::Insensitive:
      return "insensitive";
    case ProjectSearchCaseMode::Smart:
    default:
      return "smart";
  }
}

BenchRun RunBench(const std::filesystem::path& root,
                  const std::string& query,
                  const ProjectSearchOptions& options) {
  ProjectSearchService service;
  BenchRun run;

  const auto start_time = std::chrono::steady_clock::now();
  const std::uint64_t run_id = service.Start(root, query, options);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = service.TakePendingUpdate();
    if (update.run_id == run_id) {
      run.result_count += update.results.size();
      run.truncated = run.truncated || update.truncated;
      if (!update.error.empty()) {
        run.error = std::move(update.error);
      }
      if (update.finished) {
        run.finished = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto end_time = std::chrono::steady_clock::now();
  service.Stop();

  run.elapsed_ms =
      std::chrono::duration<double, std::milli>(end_time - start_time).count();
  if (!run.finished && run.error.empty()) {
    run.error = "search benchmark timed out";
  }
  return run;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    PrintUsage();
    return 1;
  }

  const std::filesystem::path project_root = std::filesystem::path(argv[1]).lexically_normal();
  const std::string query = argv[2];
  ProjectSearchOptions options;
  int run_count = 5;

  for (int index = 3; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--regex") {
      options.pattern_mode = ProjectSearchPatternMode::Regex;
      continue;
    }
    if (argument == "--literal") {
      options.pattern_mode = ProjectSearchPatternMode::Literal;
      continue;
    }
    if (argument == "--hidden") {
      options.show_hidden = true;
      continue;
    }
    if (argument.starts_with("--case=")) {
      const auto case_mode = ParseCaseMode(argument.substr(std::string_view("--case=").size()));
      if (!case_mode.has_value()) {
        std::cerr << "invalid case mode: " << argument << '\n';
        PrintUsage();
        return 1;
      }
      options.case_mode = *case_mode;
      continue;
    }
    if (argument.starts_with("--runs=")) {
      try {
        run_count = std::stoi(std::string(argument.substr(std::string_view("--runs=").size())));
      } catch (...) {
        std::cerr << "invalid run count: " << argument << '\n';
        return 1;
      }
      if (run_count <= 0) {
        std::cerr << "run count must be positive\n";
        return 1;
      }
      continue;
    }

    std::cerr << "unknown option: " << argument << '\n';
    PrintUsage();
    return 1;
  }

  std::vector<BenchRun> runs;
  runs.reserve(static_cast<std::size_t>(run_count));
  for (int index = 0; index < run_count; ++index) {
    BenchRun run = RunBench(project_root, query, options);
    if (!run.error.empty()) {
      std::cerr << "run " << (index + 1) << " failed: " << run.error << '\n';
      return 1;
    }
    runs.push_back(std::move(run));
  }

  double min_ms = runs.front().elapsed_ms;
  double max_ms = runs.front().elapsed_ms;
  double total_ms = 0.0;
  for (const BenchRun& run : runs) {
    min_ms = std::min(min_ms, run.elapsed_ms);
    max_ms = std::max(max_ms, run.elapsed_ms);
    total_ms += run.elapsed_ms;
  }
  const double average_ms = total_ms / static_cast<double>(runs.size());

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "project-root: " << project_root << '\n';
  std::cout << "query: " << query << '\n';
  std::cout << "pattern-mode: " << PatternModeLabel(options.pattern_mode) << '\n';
  std::cout << "case-mode: " << CaseModeLabel(options.case_mode) << '\n';
  std::cout << "show-hidden: " << (options.show_hidden ? "yes" : "no") << '\n';
  std::cout << "runs: " << run_count << '\n';
  std::cout << "results: " << runs.front().result_count << '\n';
  std::cout << "truncated: " << (runs.front().truncated ? "yes" : "no") << '\n';
  std::cout << "run-ms:";
  for (const BenchRun& run : runs) {
    std::cout << ' ' << run.elapsed_ms;
  }
  std::cout << '\n';
  std::cout << "min-ms: " << min_ms << '\n';
  std::cout << "max-ms: " << max_ms << '\n';
  std::cout << "avg-ms: " << average_ms << '\n';
  return 0;
}
