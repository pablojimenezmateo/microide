#include "TestRunner.h"

#include "TestRunnerCli.h"
#include "platform/HostPlatform.h"
#include "terminal/TerminalSession.h"
#include "util/Parse.h"
#include "util/TraceChannel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool WildcardMatch(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_pattern_index = std::string_view::npos;
  std::size_t star_text_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' || pattern[pattern_index] == text[text_index])) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_pattern_index = pattern_index++;
      star_text_index = text_index;
      continue;
    }
    if (star_pattern_index != std::string_view::npos) {
      pattern_index = star_pattern_index + 1;
      text_index = ++star_text_index;
      continue;
    }
    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

bool MatchesAnyPattern(std::string_view value, std::string_view patterns) {
  if (patterns.empty()) {
    return true;
  }
  std::size_t start = 0;
  while (start <= patterns.size()) {
    const std::size_t separator = patterns.find(':', start);
    const std::size_t end = separator == std::string_view::npos ? patterns.size() : separator;
    const std::string_view pattern = patterns.substr(start, end - start);
    if (!pattern.empty() && WildcardMatch(pattern, value)) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return false;
}

bool MatchesGtestFilter(std::string_view test_name, std::string_view filter_expression) {
  if (filter_expression.empty()) {
    return true;
  }
  const std::size_t dash = filter_expression.find('-');
  const std::string_view includes =
      dash == std::string_view::npos ? filter_expression : filter_expression.substr(0, dash);
  const std::string_view excludes =
      dash == std::string_view::npos ? std::string_view{} : filter_expression.substr(dash + 1);
  const bool included = includes.empty() ? true : MatchesAnyPattern(test_name, includes);
  if (!included) {
    return false;
  }
  if (!excludes.empty() && MatchesAnyPattern(test_name, excludes)) {
    return false;
  }
  return true;
}

struct GtestListNameParts {
  std::string_view suite;
  std::string_view test;
};

GtestListNameParts SplitGtestListName(std::string_view test_name) {
  const std::size_t separator = test_name.find('/');
  if (separator == std::string_view::npos) {
    return {"Ungrouped", test_name};
  }
  return {test_name.substr(0, separator), test_name.substr(separator + 1)};
}

bool IsSelected(const std::string& test_name,
                const std::vector<std::string>& substring_filters,
                const std::string* gtest_filter) {
  if (gtest_filter != nullptr) {
    return MatchesGtestFilter(test_name, *gtest_filter);
  }
  if (substring_filters.empty()) {
    return true;
  }
  for (const std::string_view filter : substring_filters) {
    if (test_name.find(filter) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void ListSelectedTestsFlat(const std::vector<microide::tests::TestCase>& tests,
                           const std::vector<std::string>& substring_filters,
                           const std::string* gtest_filter) {
  for (const auto& test : tests) {
    if (!IsSelected(test.name, substring_filters, gtest_filter)) {
      continue;
    }
    std::cout << test.name << '\n';
  }
}

void ListSelectedTestsGtest(const std::vector<microide::tests::TestCase>& tests,
                            const std::vector<std::string>& substring_filters,
                            const std::string* gtest_filter) {
  std::string_view current_suite;
  bool have_suite = false;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, substring_filters, gtest_filter)) {
      continue;
    }
    const GtestListNameParts parts = SplitGtestListName(test.name);
    if (!have_suite || parts.suite != current_suite) {
      current_suite = parts.suite;
      have_suite = true;
      std::cout << current_suite << ".\n";
    }
    std::cout << "  " << parts.test << '\n';
  }
}

}  // namespace

namespace microide::tests {

int RunTestSuite(int argc, char** argv, const TestSuiteConfig& config) {
  const std::string_view name = config.binary_name;
  const auto tear_down = [&config]() {
    if (config.tear_down) {
      config.tear_down();
    }
  };

  // Tests drive the shell from this thread; marking it keeps the trace
  // summary's main-thread split meaningful under the suite.
  microide::util::MarkTracingMainThread();

  // Match the production process (src/app/main.cpp): a write to a subprocess pipe
  // whose reader has died must surface as EPIPE, not a signal. Without it the suite
  // inherits SIG_DFL and any git/LSP/DAP write that loses the race under load kills
  // the whole shard — and whether it did depended on whether some earlier test in
  // that shard happened to call IgnoreBrokenPipeSignal() first, so adding a test
  // anywhere could reshuffle the round-robin and move the crash to a new shard.
  microide::platform::IgnoreBrokenPipeSignal();

  // Use in-process placeholder terminals for the whole suite instead of spawning
  // real PTY-backed shells. Formerly a compile-time MICROIDE_TESTING fork; now a
  // runtime switch so core compiles identically for the production binary.
  microide::terminal::SetUsePlaceholderTerminalsForTesting(true);

  if (config.install_process_hooks) {
    config.install_process_hooks();
  }

  // Isolate every user-directory write (recents MRU, user config, per-project state,
  // caches) into a throwaway temp tree for the whole process, so the suite never reads or
  // pollutes the developer's real ~/.local/state, ~/.config, etc. Several WorkspaceShell
  // fixtures open temp projects that would otherwise record their /tmp roots into the real
  // recents store. These outlive every test; individual fixtures that set their own scoped
  // XDG/HOME still override-then-restore back to these. Restored + removed at process exit.
  const microide::tests::TemporaryDirectory isolated_user_root;
  const std::filesystem::path user_root = isolated_user_root.path();
  std::error_code isolate_ec;
  std::filesystem::create_directories(user_root / "state", isolate_ec);
  std::filesystem::create_directories(user_root / "config", isolate_ec);
  std::filesystem::create_directories(user_root / "data", isolate_ec);
  std::filesystem::create_directories(user_root / "cache", isolate_ec);
  const microide::tests::ScopedEnvVar isolated_xdg_state("XDG_STATE_HOME",
                                                         (user_root / "state").string());
  const microide::tests::ScopedEnvVar isolated_xdg_config("XDG_CONFIG_HOME",
                                                          (user_root / "config").string());
  const microide::tests::ScopedEnvVar isolated_xdg_data("XDG_DATA_HOME",
                                                        (user_root / "data").string());
  const microide::tests::ScopedEnvVar isolated_xdg_cache("XDG_CACHE_HOME",
                                                         (user_root / "cache").string());
  const microide::tests::ScopedEnvVar isolated_localappdata("LOCALAPPDATA",
                                                            (user_root / "state").string());
  const microide::tests::ScopedEnvVar isolated_appdata("APPDATA",
                                                       (user_root / "config").string());

  std::vector<std::string_view> args;
  args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
    if (!arg.empty()) {
      args.push_back(arg);
    }
  }
  const auto parsed = microide::tests::ParseTestRunnerArgs(args);
  if (parsed.error.has_value()) {
    std::cerr << name << ": " << *parsed.error << "\n\n";
    microide::tests::PrintTestRunnerUsage(
        std::cerr, argc > 0 && argv[0] != nullptr ? std::string_view(argv[0]) : name);
    tear_down();
    return 2;
  }
  if (parsed.options.show_help) {
    microide::tests::PrintTestRunnerUsage(
        std::cout, argc > 0 && argv[0] != nullptr ? std::string_view(argv[0]) : name);
    tear_down();
    return 0;
  }

  if (config.set_up) {
    if (const std::optional<std::string> error = config.set_up()) {
      std::cerr << name << ": " << *error << '\n';
      tear_down();
      return 2;
    }
  }

  std::vector<microide::tests::TestCase> tests;
  config.register_tests(tests);

  std::size_t selected_count = 0;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, parsed.options.substring_filters, parsed.options.gtest_filter ? &*parsed.options.gtest_filter : nullptr)) {
      continue;
    }
    ++selected_count;
  }

  if (parsed.options.list_mode != microide::tests::TestListMode::None) {
    if (parsed.options.list_mode == microide::tests::TestListMode::Gtest) {
      ListSelectedTestsGtest(tests, parsed.options.substring_filters,
                             parsed.options.gtest_filter ? &*parsed.options.gtest_filter
                                                         : nullptr);
    } else {
      ListSelectedTestsFlat(tests, parsed.options.substring_filters,
                            parsed.options.gtest_filter ? &*parsed.options.gtest_filter
                                                        : nullptr);
    }
    tear_down();
    return selected_count == 0 ? 1 : 0;
  }

  if (selected_count == 0) {
    tear_down();
    std::cerr << name << ": no tests matched the provided filters\n";
    return 1;
  }

  // Config knobs (env-driven so ctest/run-checks can tune without a rebuild):
  //   MICROIDE_TEST_TIMEOUT_MS  watchdog per-test limit; a test that exceeds it
  //                             is named and the process aborts (0 disables).
  //   MICROIDE_TEST_SLOW_MS     threshold above which a test prints a SLOW line.
  //   MICROIDE_TEST_TIMINGS     any non-empty value prints the slowest tests.
  const auto env_i64 = [](const char* variable, std::int64_t fallback) -> std::int64_t {
    const char* raw = std::getenv(variable);
    if (raw == nullptr || *raw == '\0') {
      return fallback;
    }
    const std::optional<std::int64_t> parsed_value = microide::util::ParseInt64(raw);
    return parsed_value.value_or(fallback);
  };
  const std::int64_t watchdog_ms = env_i64("MICROIDE_TEST_TIMEOUT_MS", 300000);
  const std::int64_t slow_ms = env_i64("MICROIDE_TEST_SLOW_MS", 2000);
  const bool print_timings =
      parsed.options.print_timings || std::getenv("MICROIDE_TEST_TIMINGS") != nullptr;

  using Clock = std::chrono::steady_clock;
  const auto to_ms = [](Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };

  // Watchdog: names (then aborts on) any test that runs away, so a hang or a
  // pathological test surfaces as a clear diagnostic instead of a mysterious
  // whole-shard ctest timeout. It watches an atomic "current test started at"
  // stamp; the run loop clears it between tests.
  std::atomic<std::int64_t> current_test_started{0};  // 0 == no test running
  std::atomic<const std::string*> current_test_name{nullptr};
  std::atomic<bool> watchdog_stop{false};
  std::thread watchdog;
  if (watchdog_ms > 0) {
    watchdog = std::thread([&]() {
      while (!watchdog_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const std::int64_t started = current_test_started.load(std::memory_order_acquire);
        if (started == 0) {
          continue;
        }
        const std::int64_t elapsed = to_ms(Clock::now().time_since_epoch()) - started;
        if (elapsed >= watchdog_ms) {
          const std::string* running = current_test_name.load(std::memory_order_acquire);
          std::cerr << name << ": TIMEOUT after " << elapsed << "ms in "
                    << (running != nullptr ? *running : std::string("<unknown>")) << '\n';
          std::cerr.flush();
          std::abort();
        }
      }
    });
  }

  // Round-robin sharding: assign every selected test a stable 0-based ordinal
  // and run only those whose ordinal lands in this shard. With the default
  // shard_count == 1 this runs the whole suite. A shard whose slice is empty
  // (shard_count > selected_count) is NOT an error — only a truly empty filter
  // selection is (handled above). See TestRunnerCliOptions.
  const int shard_index = parsed.options.shard_index;
  const auto shard_count = static_cast<std::size_t>(parsed.options.shard_count);
  std::size_t selected_ordinal = 0;
  std::size_t ran_count = 0;
  std::vector<std::pair<std::int64_t, const std::string*>> timings;
  int exit_code = 0;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, parsed.options.substring_filters,
                    parsed.options.gtest_filter ? &*parsed.options.gtest_filter : nullptr)) {
      continue;
    }
    const std::size_t ordinal = selected_ordinal++;
    if (static_cast<int>(ordinal % shard_count) != shard_index) {
      continue;
    }
    ++ran_count;
    if (parsed.options.verbose) {
      std::cerr << "[" << ran_count << "] " << test.name << '\n';
    }
    current_test_name.store(&test.name, std::memory_order_release);
    current_test_started.store(to_ms(Clock::now().time_since_epoch()),
                               std::memory_order_release);
    const auto start = Clock::now();
    try {
      test.run();
    } catch (const std::exception& error) {
      std::cerr << name << " failed in " << test.name << ": " << error.what() << '\n';
      exit_code = 1;
      break;
    } catch (...) {
      std::cerr << name << " failed in " << test.name << ": unknown exception\n";
      exit_code = 1;
      break;
    }
    const std::int64_t elapsed_ms = to_ms(Clock::now() - start);
    current_test_started.store(0, std::memory_order_release);
    timings.emplace_back(elapsed_ms, &test.name);
    if (slow_ms > 0 && elapsed_ms >= slow_ms) {
      std::cerr << name << ": SLOW " << elapsed_ms << "ms " << test.name << '\n';
    }
  }

  watchdog_stop.store(true, std::memory_order_relaxed);
  if (watchdog.joinable()) {
    watchdog.join();
  }

  if (print_timings && !timings.empty()) {
    std::stable_sort(timings.begin(), timings.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    const std::size_t show = std::min<std::size_t>(timings.size(), 25);
    std::cerr << name << ": slowest " << show << " of " << timings.size() << " tests:\n";
    for (std::size_t i = 0; i < show; ++i) {
      std::cerr << "  " << timings[i].first << "ms\t" << *timings[i].second << '\n';
    }
  }

  if (exit_code != 0) {
    tear_down();
    return exit_code;
  }

  // Final summary so callers (and agents) get an unambiguous pass signal
  // without having to inspect the exit code or scrape verbose output.
  if (shard_count > 1) {
    std::cerr << name << ": OK (shard " << shard_index << "/" << shard_count << ": "
              << ran_count << " of " << selected_count
              << (selected_count == 1 ? " test)\n" : " tests)\n");
  } else {
    std::cerr << name << ": OK (" << selected_count
              << (selected_count == 1 ? " test passed)\n" : " tests passed)\n");
  }
  tear_down();
  return 0;
}

}  // namespace microide::tests
