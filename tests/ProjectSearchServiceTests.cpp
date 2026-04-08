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
  bool finished = false;
};

SearchRunResult RunProjectSearch(const std::filesystem::path& root,
                                 std::string query,
                                 ProjectSearchOptions options = {}) {
  ProjectSearchService service;
  const std::uint64_t run_id = service.Start(root, std::move(query), options);

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

}  // namespace

void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectSearchService/LiteralModesAndCaseControls",
          TestProjectSearchServiceLiteralModesAndCaseControls);
  AddTest(tests, "ProjectSearchService/RegexModeAndInvalidRegex",
          TestProjectSearchServiceRegexModeAndInvalidRegex);
  AddTest(tests, "ProjectSearchService/HiddenAndBinaryFiles",
          TestProjectSearchServiceHiddenAndBinaryFiles);
}

}  // namespace microide::tests
