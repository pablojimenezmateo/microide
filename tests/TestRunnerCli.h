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
  TestListMode list_mode = TestListMode::None;
  std::vector<std::string> substring_filters;
  std::optional<std::string> gtest_filter;
};

struct TestRunnerCliParseResult {
  TestRunnerCliOptions options;
  std::optional<std::string> error;
};

TestRunnerCliParseResult ParseTestRunnerArgs(const std::vector<std::string_view>& args);
void PrintTestRunnerUsage(std::ostream& out, std::string_view program_name);

}  // namespace microide::tests
