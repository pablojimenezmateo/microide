#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

enum class TestListMode {
  None,
  Flat,
  Gtest,
};

struct TestRunnerCliOptions {
  bool verbose = false;
  bool show_help = false;
  bool print_timings = false;
  TestListMode list_mode = TestListMode::None;
  std::vector<std::string> substring_filters;
  std::optional<std::string> gtest_filter;
  // Deterministic round-robin sharding for parallel ctest execution. The suite
  // runs as one long serial process; splitting the selected tests across N
  // shards (one process each, run concurrently by `ctest -jN`) is the only way
  // to use more than one core, which matters most under the sanitizers. Each
  // shard runs the selected tests whose 0-based ordinal satisfies
  // `ordinal % shard_count == shard_index`. Defaults mean "run everything".
  int shard_index = 0;
  int shard_count = 1;
};

struct TestRunnerCliParseResult {
  TestRunnerCliOptions options;
  std::optional<std::string> error;
};

TestRunnerCliParseResult ParseTestRunnerArgs(const std::vector<std::string_view>& args);
void PrintTestRunnerUsage(std::ostream& out, std::string_view program_name);

}  // namespace microide::tests
