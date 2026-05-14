#include "TestRunnerCli.h"

#include <ostream>

namespace microide::tests {

namespace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

enum class ConsumeOptionalValueResult {
  NoMatch,
  Matched,
  MissingValue,
};

ConsumeOptionalValueResult ConsumeOptionalValue(std::string_view arg,
                                               std::string_view prefix,
                                               const std::vector<std::string_view>& args,
                                               std::size_t* index,
                                               std::string* out_value) {
  if (arg == prefix) {
    if (*index + 1 >= args.size()) {
      return ConsumeOptionalValueResult::MissingValue;
    }
    ++(*index);
    *out_value = std::string(args[*index]);
    return ConsumeOptionalValueResult::Matched;
  }

  if (arg.size() <= prefix.size() || arg[prefix.size()] != '=') {
    return ConsumeOptionalValueResult::NoMatch;
  }
  if (!StartsWith(arg, prefix)) {
    return ConsumeOptionalValueResult::NoMatch;
  }

  *out_value = std::string(arg.substr(prefix.size() + 1));
  return ConsumeOptionalValueResult::Matched;
}

}  // namespace

TestRunnerCliParseResult ParseTestRunnerArgs(const std::vector<std::string_view>& args) {
  TestRunnerCliParseResult result;
  result.options.substring_filters.reserve(args.size());

  bool positional_only = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (positional_only) {
      if (!arg.empty()) {
        result.options.substring_filters.emplace_back(arg);
      }
      continue;
    }

    if (arg == "--") {
      positional_only = true;
      continue;
    }
    if (arg == "--help" || arg == "-h" || arg == "--gtest_help") {
      result.options.show_help = true;
      continue;
    }
    if (arg == "--verbose" || arg == "-v") {
      result.options.verbose = true;
      continue;
    }
    if (arg == "--list-tests" || arg == "--list" || arg == "--gtest_list_tests") {
      result.options.list_mode =
          arg == "--gtest_list_tests" ? TestListMode::Gtest : TestListMode::Flat;
      continue;
    }

    std::string filter_value;
    const auto filter_result = ConsumeOptionalValue(arg, "--filter", args, &i, &filter_value);
    if (filter_result == ConsumeOptionalValueResult::MissingValue) {
      result.error = "missing value for --filter";
      return result;
    }
    if (filter_result == ConsumeOptionalValueResult::Matched) {
      result.options.gtest_filter = std::move(filter_value);
      continue;
    }

    const auto gtest_filter_result =
        ConsumeOptionalValue(arg, "--gtest_filter", args, &i, &filter_value);
    if (gtest_filter_result == ConsumeOptionalValueResult::MissingValue) {
      result.error = "missing value for --gtest_filter";
      return result;
    }
    if (gtest_filter_result == ConsumeOptionalValueResult::Matched) {
      result.options.gtest_filter = std::move(filter_value);
      continue;
    }

    if (!arg.empty() && arg.front() == '-') {
      result.error = "unknown argument: " + std::string(arg);
      return result;
    }

    if (!arg.empty()) {
      result.options.substring_filters.emplace_back(arg);
    }
  }

  return result;
}

void PrintTestRunnerUsage(std::ostream& out, std::string_view program_name) {
  out << "usage: " << program_name << " [options] [substring-filter ...]\n"
      << "\n"
      << "Options:\n"
      << "  -h, --help, --gtest_help     Show this help text.\n"
      << "  -v, --verbose                Print each selected test before running it.\n"
      << "  --list-tests, --list         List registered tests and exit.\n"
      << "  --gtest_list_tests           List registered tests and exit.\n"
      << "  --filter=EXPR                Wildcard filter expression using '*' '?' and ':' separators.\n"
      << "  --filter EXPR                Same as --filter=EXPR.\n"
      << "  --gtest_filter=EXPR          Alias for --filter=EXPR.\n"
      << "  --gtest_filter EXPR          Alias for --filter EXPR.\n"
      << "  --                           Treat remaining arguments as substring filters.\n"
      << "\n"
      << "Examples:\n"
      << "  " << program_name << "\n"
      << "  " << program_name << " --list-tests\n"
      << "  " << program_name << " TextRenderer\n"
      << "  " << program_name << " --gtest_filter='WorkspaceShell/*Status*'\n";
}

}  // namespace microide::tests
