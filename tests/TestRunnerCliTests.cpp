#include "TestSupport.h"

#include "TestRunnerCli.h"

#include <vector>

namespace microide::tests {
namespace {

void TestParseHelpFlags() {
  const auto parsed = ParseTestRunnerArgs({"--help"});
  Expect(!parsed.error.has_value(), "help flag should parse without error");
  Expect(parsed.options.show_help, "help flag should request usage output");
}

void TestParseListFlags() {
  const auto parsed = ParseTestRunnerArgs({"--gtest_list_tests"});
  Expect(!parsed.error.has_value(), "list-tests flag should parse without error");
  Expect(parsed.options.list_mode == TestListMode::Gtest,
         "gtest list flag should select gtest-compatible list mode");
}

void TestParseFilterForms() {
  {
    const auto parsed = ParseTestRunnerArgs({"--filter=WorkspaceShell/*Status*"});
    Expect(!parsed.error.has_value(), "inline filter should parse without error");
    Expect(parsed.options.gtest_filter.has_value() &&
               *parsed.options.gtest_filter == "WorkspaceShell/*Status*",
           "inline filter should preserve the requested expression");
  }
  {
    const auto parsed = ParseTestRunnerArgs({"--gtest_filter", "TextRenderer*"});
    Expect(!parsed.error.has_value(), "split gtest filter should parse without error");
    Expect(parsed.options.gtest_filter.has_value() &&
               *parsed.options.gtest_filter == "TextRenderer*",
           "split gtest filter should preserve the requested expression");
  }
}

void TestParsePositionalSubstringFilters() {
  const auto parsed = ParseTestRunnerArgs({"TextRenderer", "WorkspaceShell"});
  Expect(!parsed.error.has_value(), "substring filters should parse without error");
  Expect(parsed.options.substring_filters.size() == 2,
         "positional filters should remain substring filters");
  Expect(parsed.options.substring_filters[0] == "TextRenderer" &&
             parsed.options.substring_filters[1] == "WorkspaceShell",
         "substring filters should preserve argument order");
}

void TestRejectUnknownFlag() {
  const auto parsed = ParseTestRunnerArgs({"--wat"});
  Expect(parsed.error.has_value(), "unknown flags should be rejected");
}

void TestRejectMissingFilterValue() {
  const auto parsed = ParseTestRunnerArgs({"--filter"});
  Expect(parsed.error.has_value(), "filters without values should be rejected");
}

void TestParseDoubleDashTerminator() {
  const auto parsed = ParseTestRunnerArgs({"--", "--help"});
  Expect(!parsed.error.has_value(), "double-dash should switch to positional parsing");
  Expect(!parsed.options.show_help, "double-dash should stop option parsing");
  Expect(parsed.options.substring_filters.size() == 1 &&
             parsed.options.substring_filters[0] == "--help",
         "double-dash should preserve the remaining arguments as substring filters");
}

void TestShardDefaults() {
  const auto parsed = ParseTestRunnerArgs({});
  Expect(!parsed.error.has_value(), "no args should parse without error");
  Expect(parsed.options.shard_index == 0 && parsed.options.shard_count == 1,
         "default sharding should run the whole suite (index 0 of 1)");
}

void TestParseShardForms() {
  {
    const auto parsed = ParseTestRunnerArgs({"--shard-index=3", "--shard-count=12"});
    Expect(!parsed.error.has_value(), "inline shard flags should parse without error");
    Expect(parsed.options.shard_index == 3 && parsed.options.shard_count == 12,
           "inline shard flags should preserve index and count");
  }
  {
    const auto parsed = ParseTestRunnerArgs({"--shard-index", "0", "--shard-count", "4"});
    Expect(!parsed.error.has_value(), "split shard flags should parse without error");
    Expect(parsed.options.shard_index == 0 && parsed.options.shard_count == 4,
           "split shard flags should preserve index and count");
  }
}

void TestRejectInvalidShards() {
  Expect(ParseTestRunnerArgs({"--shard-count=0"}).error.has_value(),
         "shard-count of 0 should be rejected");
  Expect(ParseTestRunnerArgs({"--shard-index=4", "--shard-count=4"}).error.has_value(),
         "shard-index equal to shard-count should be rejected");
  Expect(ParseTestRunnerArgs({"--shard-index=-1", "--shard-count=4"}).error.has_value(),
         "negative shard-index should be rejected");
  Expect(ParseTestRunnerArgs({"--shard-count=notanumber"}).error.has_value(),
         "non-numeric shard-count should be rejected");
  Expect(ParseTestRunnerArgs({"--shard-index"}).error.has_value(),
         "shard-index without a value should be rejected");
}

}  // namespace

void RegisterTestRunnerCliTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TestRunnerCli/ParseHelpFlags", TestParseHelpFlags);
  AddTest(tests, "TestRunnerCli/ParseListFlags", TestParseListFlags);
  AddTest(tests, "TestRunnerCli/ParseFilterForms", TestParseFilterForms);
  AddTest(tests, "TestRunnerCli/ParsePositionalSubstringFilters",
          TestParsePositionalSubstringFilters);
  AddTest(tests, "TestRunnerCli/RejectUnknownFlag", TestRejectUnknownFlag);
  AddTest(tests, "TestRunnerCli/RejectMissingFilterValue", TestRejectMissingFilterValue);
  AddTest(tests, "TestRunnerCli/ParseDoubleDashTerminator", TestParseDoubleDashTerminator);
  AddTest(tests, "TestRunnerCli/ShardDefaults", TestShardDefaults);
  AddTest(tests, "TestRunnerCli/ParseShardForms", TestParseShardForms);
  AddTest(tests, "TestRunnerCli/RejectInvalidShards", TestRejectInvalidShards);
}

}  // namespace microide::tests
