#include "TestSupport.h"

#include "project/ProjectSearchService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::ProjectSearchResult;
using microide::project::ProjectSearchCaseMode;
using microide::project::ProjectSearchOptions;
using microide::project::ProjectSearchPatternMode;
using microide::project::ProjectSearchService;

struct SearchRunResult {
  std::vector<ProjectSearchResult> results;
  std::string error;
  bool truncated = false;
  bool finished = false;
};

SearchRunResult RunProjectSearch(const std::filesystem::path& root,
                                 std::string query,
                                 ProjectSearchOptions options = {},
                                 std::vector<std::filesystem::path> indexed_files = {}) {
  ProjectSearchService service;
  const std::uint64_t run_id = service.Start(root, std::move(query), options,
                                             std::move(indexed_files));

  SearchRunResult result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = service.TakePendingUpdate();
    if (update.run_id == run_id) {
      result.results.insert(result.results.end(),
                            std::make_move_iterator(update.results.begin()),
                            std::make_move_iterator(update.results.end()));
      if (!update.error.empty()) {
        result.error = std::move(update.error);
      }
      result.truncated = result.truncated || update.truncated;
      if (update.finished) {
        result.finished = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  service.Stop();
  return result;
}

void TestProjectSearchServiceLiteralModesAndCaseControls() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "Alpha alpha ALPHA\n");
  WriteFile(root / "special.txt", "alp.a [abc]\n");

  const auto smart_literal = RunProjectSearch(root, "alpha");
  Expect(smart_literal.finished, "smart literal project search should finish");
  Expect(smart_literal.error.empty(), "smart literal project search should not error");
  Expect(smart_literal.results.size() == 3,
         "default literal smart-case search should match every case variant");
  Expect(smart_literal.results[0].relative_path == std::filesystem::path("notes.txt"),
         "default literal search should report the first matching file");
  Expect(smart_literal.results[0].line == 0 && smart_literal.results[0].column == 0,
         "default literal search should report the first line-leading match");

  ProjectSearchOptions sensitive_options;
  sensitive_options.case_mode = ProjectSearchCaseMode::Sensitive;
  const auto sensitive_literal = RunProjectSearch(root, "alpha", sensitive_options);
  Expect(sensitive_literal.finished, "case-sensitive literal project search should finish");
  Expect(sensitive_literal.error.empty(),
         "case-sensitive literal project search should not error");
  Expect(sensitive_literal.results.size() == 1,
         "explicit sensitive literal search should only match exact-case text");
  Expect(sensitive_literal.results[0].line == 0 && sensitive_literal.results[0].column == 6,
         "explicit sensitive literal search should keep the lowercase match");

  ProjectSearchOptions insensitive_options;
  insensitive_options.case_mode = ProjectSearchCaseMode::Insensitive;
  const auto insensitive_literal = RunProjectSearch(root, "Alpha", insensitive_options);
  Expect(insensitive_literal.finished, "case-insensitive literal project search should finish");
  Expect(insensitive_literal.error.empty(),
         "case-insensitive literal project search should not error");
  Expect(insensitive_literal.results.size() == 3,
         "explicit insensitive literal search should ignore the query casing");

  const auto literal_metacharacters = RunProjectSearch(root, "alp.a");
  Expect(literal_metacharacters.finished,
         "literal project search with regex metacharacters should finish");
  Expect(literal_metacharacters.error.empty(),
         "literal project search with regex metacharacters should not error");
  Expect(literal_metacharacters.results.size() == 1,
         "default literal project search should treat dots as plain characters");
  Expect(literal_metacharacters.results[0].relative_path == std::filesystem::path("special.txt"),
         "literal metacharacter search should find the literal text");
}

void TestProjectSearchServiceRegexModeAndInvalidRegex() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "alpha\nAlpha\nalpXa\n");

  ProjectSearchOptions regex_options;
  regex_options.pattern_mode = ProjectSearchPatternMode::Regex;
  const auto smart_regex = RunProjectSearch(root, "alp.a", regex_options);
  Expect(smart_regex.finished, "regex project search should finish");
  Expect(smart_regex.error.empty(), "regex project search should not error");
  Expect(smart_regex.results.size() == 3,
         "regex project search should evaluate patterns with smart-case handling");

  ProjectSearchOptions sensitive_regex_options = regex_options;
  sensitive_regex_options.case_mode = ProjectSearchCaseMode::Sensitive;
  const auto sensitive_regex = RunProjectSearch(root, "alp.a", sensitive_regex_options);
  Expect(sensitive_regex.finished, "explicit sensitive regex project search should finish");
  Expect(sensitive_regex.error.empty(),
         "explicit sensitive regex project search should not error");
  Expect(sensitive_regex.results.size() == 2,
         "explicit sensitive regex search should stop matching differently cased text");

  const auto invalid_regex = RunProjectSearch(root, "[alpha", regex_options);
  Expect(invalid_regex.finished, "invalid regex project search should finish");
  Expect(!invalid_regex.error.empty(), "invalid regex project search should surface an error");
  Expect(invalid_regex.results.empty(), "invalid regex project search should not publish matches");
}

void TestProjectSearchServiceHiddenAndBinaryFiles() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "visible.txt", "alpha\n");
  WriteFile(root / ".hidden.txt", "alpha\n");
  WriteFile(root / "binary.dat", std::string("alpha\0beta\n", 11));

  const auto visible_only = RunProjectSearch(root, "alpha");
  Expect(visible_only.finished, "visible-only project search should finish");
  Expect(visible_only.error.empty(), "visible-only project search should not error");
  Expect(visible_only.results.size() == 1,
         "default project search should skip hidden files and binary files");
  Expect(visible_only.results[0].relative_path == std::filesystem::path("visible.txt"),
         "default project search should keep the visible match");

  ProjectSearchOptions hidden_options;
  hidden_options.show_hidden = true;
  const auto including_hidden = RunProjectSearch(root, "alpha", hidden_options);
  Expect(including_hidden.finished, "hidden-inclusive project search should finish");
  Expect(including_hidden.error.empty(), "hidden-inclusive project search should not error");
  Expect(including_hidden.results.size() == 2,
         "hidden-inclusive project search should include hidden files without reading binary data");
  const bool has_hidden_match = std::any_of(
      including_hidden.results.begin(), including_hidden.results.end(), [](const auto& result) {
        return result.relative_path == std::filesystem::path(".hidden.txt");
      });
  Expect(has_hidden_match, "hidden-inclusive project search should include hidden matches");
}

void TestProjectSearchServicePublishesStableResultOrdering() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha alpha\nbeta alpha\n");
  WriteFile(root / "b.txt", "gamma\nalpha\n");
  WriteFile(root / "c.txt", "Alpha\n");

  const auto ordered = RunProjectSearch(root, "alpha");
  Expect(ordered.finished, "ordered project search should finish");
  Expect(ordered.error.empty(), "ordered project search should not error");
  Expect(!ordered.truncated, "ordered project search should not truncate small result sets");
  Expect(ordered.results.size() == 5,
         "ordered project search should report every match across files and lines");

  Expect(ordered.results[0].relative_path == std::filesystem::path("a.txt") &&
             ordered.results[0].line == 0 && ordered.results[0].column == 0,
         "project search should report the first file-leading match first");
  Expect(ordered.results[1].relative_path == std::filesystem::path("a.txt") &&
             ordered.results[1].line == 0 && ordered.results[1].column == 6,
         "project search should keep later matches on the same line in column order");
  Expect(ordered.results[2].relative_path == std::filesystem::path("a.txt") &&
             ordered.results[2].line == 1 && ordered.results[2].column == 5,
         "project search should keep later lines in the same file in order");
  Expect(ordered.results[3].relative_path == std::filesystem::path("b.txt") &&
             ordered.results[3].line == 1 && ordered.results[3].column == 0,
         "project search should move to the next file in lexical path order");
  Expect(ordered.results[4].relative_path == std::filesystem::path("c.txt") &&
             ordered.results[4].line == 0 && ordered.results[4].column == 0,
         "project search should preserve sorted file ordering across the full result set");
}

void TestProjectSearchServiceFlagsTruncatedLargeResultSets() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::string repeated_lines;
  for (int line = 0; line < 25; ++line) {
    repeated_lines += "alpha\n";
  }
  for (int file_index = 0; file_index < 10; ++file_index) {
    const std::string label = file_index < 10 ? "0" + std::to_string(file_index)
                                              : std::to_string(file_index);
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }

  const auto capped = RunProjectSearch(root, "alpha");
  Expect(capped.finished, "large project search should finish");
  Expect(capped.error.empty(), "large project search should not error");
  Expect(capped.truncated, "large project search should report when the result set is capped");
  Expect(capped.results.size() == 200,
         "large project search should publish the full capped result set");
  Expect(capped.results.front().relative_path == std::filesystem::path("file00.txt") &&
             capped.results.front().line == 0 && capped.results.front().column == 0,
         "large project search should keep the first capped match");
  Expect(capped.results.back().relative_path == std::filesystem::path("file07.txt") &&
             capped.results.back().line == 24 && capped.results.back().column == 0,
         "large project search should keep the last published capped match");
}

void TestProjectSearchServiceRestartPublishesOnlyLatestRun() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::string repeated_lines;
  for (int line = 0; line < 25; ++line) {
    repeated_lines += "alpha\n";
  }
  for (int file_index = 0; file_index < 10; ++file_index) {
    const std::string label = "0" + std::to_string(file_index);
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }
  WriteFile(root / "omega.txt", "omega\n");

  ProjectSearchService service;
  const std::uint64_t first_run_id = service.Start(root, "alpha");
  const std::uint64_t second_run_id = service.Start(root, "omega");

  SearchRunResult latest;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = service.TakePendingUpdate();
    if (update.run_id == first_run_id) {
      Expect(false, "restarted project search should discard the previous run updates");
    }
    if (update.run_id == second_run_id) {
      latest.results.insert(latest.results.end(), std::make_move_iterator(update.results.begin()),
                            std::make_move_iterator(update.results.end()));
      latest.truncated = latest.truncated || update.truncated;
      if (!update.error.empty()) {
        latest.error = std::move(update.error);
      }
      if (update.finished) {
        latest.finished = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  service.Stop();
  Expect(latest.finished, "restarted project search should finish the latest run");
  Expect(latest.error.empty(), "restarted project search should not error");
  Expect(!latest.truncated, "restarted project search should not inherit truncation from the old run");
  Expect(latest.results.size() == 1,
         "restarted project search should only publish results from the latest query");
  Expect(latest.results[0].relative_path == std::filesystem::path("omega.txt") &&
             latest.results[0].line == 0 && latest.results[0].column == 0,
         "restarted project search should keep the latest query match");
}

void TestProjectSearchServiceUsesIndexedFileSnapshotWhenProvided() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "tracked.txt", "needle\n");
  WriteFile(root / "untracked.txt", "needle\n");

  const auto result = RunProjectSearch(root, "needle", {},
                                       {std::filesystem::path("tracked.txt")});
  Expect(result.finished, "indexed project search should finish");
  Expect(result.error.empty(), "indexed project search should not error");
  Expect(result.results.size() == 1,
         "indexed project search should restrict matching to the supplied snapshot");
  Expect(result.results.front().relative_path == std::filesystem::path("tracked.txt"),
         "indexed project search should respect the supplied relative paths");
}

void TestProjectSearchServiceStopDiscardsLateUpdates() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::string repeated_lines;
  for (int line = 0; line < 50; ++line) {
    repeated_lines += "alpha\n";
  }
  for (int file_index = 0; file_index < 24; ++file_index) {
    const std::string label = file_index < 10 ? "0" + std::to_string(file_index)
                                              : std::to_string(file_index);
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }

  ProjectSearchService service;
  service.Start(root, "alpha");
  service.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  const auto update = service.TakePendingUpdate();
  Expect(update.run_id == 0, "stopped project search should discard any pending updates");
  Expect(update.results.empty(), "stopped project search should not publish late results");
  Expect(!update.finished,
         "stopped project search should not publish a completion update afterwards");
}

}  // namespace

void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectSearchService/LiteralModesAndCaseControls",
          TestProjectSearchServiceLiteralModesAndCaseControls);
  AddTest(tests, "ProjectSearchService/RegexModeAndInvalidRegex",
          TestProjectSearchServiceRegexModeAndInvalidRegex);
  AddTest(tests, "ProjectSearchService/HiddenAndBinaryFiles",
          TestProjectSearchServiceHiddenAndBinaryFiles);
  AddTest(tests, "ProjectSearchService/PublishesStableResultOrdering",
          TestProjectSearchServicePublishesStableResultOrdering);
  AddTest(tests, "ProjectSearchService/FlagsTruncatedLargeResultSets",
          TestProjectSearchServiceFlagsTruncatedLargeResultSets);
  AddTest(tests, "ProjectSearchService/RestartPublishesOnlyLatestRun",
          TestProjectSearchServiceRestartPublishesOnlyLatestRun);
  AddTest(tests, "ProjectSearchService/StopDiscardsLateUpdates",
          TestProjectSearchServiceStopDiscardsLateUpdates);
  AddTest(tests, "ProjectSearchService/UsesIndexedFileSnapshotWhenProvided",
          TestProjectSearchServiceUsesIndexedFileSnapshotWhenProvided);
}

}  // namespace microide::tests
