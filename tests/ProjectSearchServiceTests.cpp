#include "TestSupport.h"

#include "project/ProjectSearchService.h"
#include "project/ProjectSearchServiceInternal.h"
#include "project/ProjectFileScanner.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
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
  std::size_t final_searched_files = 0;
  std::size_t final_total_files = 0;
  std::size_t total_matches = 0;
};

SearchRunResult RunProjectSearch(const std::filesystem::path& root,
                                 std::string query,
                                 ProjectSearchOptions options = {},
                                 std::vector<std::filesystem::path> indexed_files = {}) {
  if (indexed_files.empty()) {
    indexed_files = project::CollectProjectFiles(
        root, options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                  : project::ProjectFileScanMode::ExcludeHidden);
  }
  ProjectSearchService service;
  const std::uint64_t run_id = service.Start(
      root, std::move(query), options,
      std::make_shared<const std::vector<std::filesystem::path>>(std::move(indexed_files)));

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
      if (update.total_files > 0) {
        result.final_searched_files = update.searched_files;
        result.final_total_files = update.total_files;
      }
      if (update.total_matches > 0) {
        result.total_matches = update.total_matches;
      }
      if (update.finished) {
        result.finished = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  service.Stop();

  // The engine streams matches in whatever order its parallel workers find them;
  // mirror the shell consumer by restoring deterministic (file_index, line,
  // column) order before assertions inspect the result list.
  std::sort(result.results.begin(), result.results.end(),
            [](const ProjectSearchResult& lhs, const ProjectSearchResult& rhs) {
              if (lhs.file_index != rhs.file_index) {
                return lhs.file_index < rhs.file_index;
              }
              if (lhs.line != rhs.line) {
                return lhs.line < rhs.line;
              }
              return lhs.column < rhs.column;
            });
  return result;
}

void TestProjectSearchServiceLiteralModesAndCaseControls() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "Alpha alpha ALPHA\n");
  WriteFile(root / "special.txt", "alp.a [abc]\n");

  const auto indexed_files =
      project::CollectProjectFiles(root, project::ProjectFileScanMode::ExcludeHidden);
  const std::string indexed_file_count_message =
      "project file scan should discover both visible files; got " +
      std::to_string(indexed_files.size());
  Expect(indexed_files.size() == 2, indexed_file_count_message);

  const auto smart_literal = RunProjectSearch(root, "alpha", {}, indexed_files);
  Expect(smart_literal.finished, "smart literal project search should finish");
  Expect(smart_literal.error.empty(), "smart literal project search should not error");
  const std::string smart_literal_count_message =
      "default literal smart-case search should match every case variant; got " +
      std::to_string(smart_literal.results.size());
  Expect(smart_literal.results.size() == 3,
         smart_literal_count_message);
  Expect(smart_literal.results[0].relative_path == std::filesystem::path("notes.txt"),
         "default literal search should report the first matching file");
  Expect(smart_literal.results[0].relative_path_string == "notes.txt",
         "project search should populate cached relative path display strings");
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

void TestProjectSearchServiceUnicodeCaseFolding() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  // "café" (lowercase é U+00E9), "CAFÉ" (uppercase É U+00C9), plus Cyrillic pair.
  WriteFile(root / "unicode.txt", "café CAFÉ ПРИВЕТ привет\n");

  // Smart-case, all-lowercase query: folds and matches both case variants.
  const auto cafe = RunProjectSearch(root, "café");
  Expect(cafe.finished && cafe.error.empty(), "unicode smart-case search finishes");
  Expect(cafe.results.size() == 2, "café matches both café and CAFÉ via Unicode case fold");
  // Length-preserving fold keeps reported byte columns aligned with the source.
  Expect(cafe.results[0].column == 0 && cafe.results[1].column == 6,
         "reported columns are original byte offsets, not folded-buffer offsets");

  const auto privet = RunProjectSearch(root, "привет");
  Expect(privet.results.size() == 2, "Cyrillic привет matches ПРИВЕТ and привет");

  // An uppercase non-ASCII letter in the query makes smart-case sensitive.
  const auto sensitive = RunProjectSearch(root, "CAFÉ");
  Expect(sensitive.results.size() == 1, "uppercase É forces a case-sensitive smart search");
}

void TestProjectSearchServiceNormalizesPreviewWhitespace() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "alpha\t\tbeta   gamma\n");

  const auto result = RunProjectSearch(root, "alpha");
  Expect(result.finished, "preview-normalizing project search should finish");
  Expect(result.error.empty(), "preview-normalizing project search should not error");
  Expect(result.results.size() == 1,
         "preview-normalizing project search should return one literal match");
  Expect(result.results[0].preview == "alpha beta gamma",
         "project search preview should collapse repeated whitespace for render-time reuse");
  Expect(result.results[0].preview.substr(result.results[0].match_preview_start,
                                          result.results[0].match_preview_length) == "alpha",
         "project search should map the match range onto the collapsed preview for highlighting");

  // A match after collapsed whitespace should still map correctly. Use a token
  // that appears in no other file under this root so the count is unambiguous.
  WriteFile(root / "indented.txt", "\t\tzeta value\n");
  const auto indented = RunProjectSearch(root, "zeta");
  Expect(indented.finished && indented.results.size() == 1,
         "indented preview search should return one match");
  Expect(indented.results[0].preview.substr(indented.results[0].match_preview_start,
                                            indented.results[0].match_preview_length) == "zeta",
         "match highlight range should account for collapsed leading whitespace");
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

// A case-insensitive REGEX search must fold Unicode case the same way the literal
// path already does. PCRE2_CASELESS on its own (no PCRE2_UTF) folds ASCII and
// nothing else, so the identical query used to match case-insensitively as a
// literal and case-sensitively as a regex.
void TestProjectSearchServiceRegexCaseInsensitiveFoldsNonAscii() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  // Greek delta and Cyrillic de, lowercase in the file, uppercase in the query.
  WriteFile(root / "greek.txt", "value δelta here\n");
  WriteFile(root / "cyrillic.txt", "value дom here\n");

  ProjectSearchOptions options;
  options.pattern_mode = ProjectSearchPatternMode::Regex;
  options.case_mode = ProjectSearchCaseMode::Insensitive;

  const auto greek = RunProjectSearch(root, "Δelta", options);
  Expect(greek.finished && greek.error.empty(), "non-ASCII regex search should finish cleanly");
  Expect(greek.results.size() == 1,
         "a case-insensitive regex search for Δ must find δ, matching the literal path");

  const auto cyrillic = RunProjectSearch(root, "Дom", options);
  Expect(cyrillic.finished && cyrillic.error.empty(), "Cyrillic regex search should finish");
  Expect(cyrillic.results.size() == 1,
         "a case-insensitive regex search for Д must find д");

  // The literal path is the reference behavior these must agree with.
  ProjectSearchOptions literal_options;
  literal_options.case_mode = ProjectSearchCaseMode::Insensitive;
  const auto literal = RunProjectSearch(root, "Δelta", literal_options);
  Expect(literal.results.size() == greek.results.size(),
         "regex and literal case-insensitive search must agree on non-ASCII case folding");

  // An ASCII query keeps the fast byte-oriented path and must be unaffected.
  const auto ascii = RunProjectSearch(root, "v.lue", options);
  Expect(ascii.finished && ascii.results.size() == 2,
         "an ASCII regex query must still match both files");
}

void TestProjectSearchServiceRegexEmptyMatchDoesNotHideRealMatch() {
  // Regression: the empty-match branch used to advance one byte and abandon the
  // offset entirely. For an alternation whose earlier branch matches empty at the
  // same position a later branch matches non-empty (`x?|foo` at "foo": `x?` wins
  // leftmost with an empty match at 0, but `foo` also starts at 0), the real match
  // was silently dropped. The anchored retry must recover it.
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "alt.txt", "foo\n");

  ProjectSearchOptions regex_options;
  regex_options.pattern_mode = ProjectSearchPatternMode::Regex;
  const auto result = RunProjectSearch(root, "x?|foo", regex_options);
  Expect(result.finished, "empty-match regex search should finish");
  Expect(result.error.empty(), "empty-match regex search should not error");
  Expect(result.results.size() == 1,
         "a non-empty alternative sharing an offset with an empty match must be found");
  Expect(result.results[0].column == 0 && result.results[0].match_preview_length == 3,
         "the recovered match should be the non-empty `foo` at column 0");
}

void TestProjectSearchServiceRegexFindNextIsCancellable() {
  // Regression: `x?` matches empty at every offset, so on a line with no 'x' the
  // advance loop walks one byte per iteration doing a PCRE2 Match each time. The
  // per-line cancel check never re-runs inside a single line, so on a huge single
  // line Stop() could not interrupt it. FindNextRegexMatch now polls the cancel
  // flag inside the advance loop. Drive it directly (deterministic; no Start/Stop
  // timing race) with the flag preset and assert it bails before the line end.
  const util::CompiledRegex pattern("x?", 0, "test regex");
  Expect(pattern.valid(), "x? compiles");
  util::RegexMatchData match_data = pattern.CreateMatchData();
  Expect(match_data.valid(), "match data allocates");

  // Longer than kRegexCancelPollInterval so a preset-cancel scan is guaranteed to
  // hit the poll before reaching the end.
  const std::string line(20000, 'a');
  std::size_t search_from = 0;
  std::size_t match_start = 0;
  std::size_t match_end = 0;

  std::atomic<bool> cancel{true};
  const bool found = project::search_internal::FindNextRegexMatch(
      pattern, line, &search_from, &match_data, &match_start, &match_end, cancel);
  Expect(!found, "a cancelled scan reports no match");
  Expect(search_from < line.size(),
         "cancellation bails inside the advance loop, well before the line end");

  // With cancellation not requested the same scan runs to completion: `x?` finds
  // no non-empty match on a no-x line and advances one past the end.
  std::atomic<bool> no_cancel{false};
  search_from = 0;
  const bool found_full = project::search_internal::FindNextRegexMatch(
      pattern, line, &search_from, &match_data, &match_start, &match_end, no_cancel);
  Expect(!found_full, "an uncancelled x? scan on a no-x line finds no non-empty match");
  Expect(search_from == line.size() + 1,
         "an uncancelled scan advances one past the line end");
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

void TestProjectSearchServiceExcludesIgnoredFilesByDefault() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / ".gitignore", "node_modules/\nignored.txt\n");
  WriteFile(root / "tracked.txt", "needle\n");
  WriteFile(root / "ignored.txt", "needle\n");
  WriteFile(root / "node_modules" / "dep" / "index.js", "needle\n");

  const auto result = RunProjectSearch(root, "needle");
  Expect(result.finished, "ignored-excluding project search should finish");
  Expect(result.error.empty(), "ignored-excluding project search should not error");
  Expect(result.results.size() == 1,
         "default project search should exclude ignored files and ignored descendants");
  Expect(result.results[0].relative_path == std::filesystem::path("tracked.txt"),
         "default project search should keep tracked files while excluding ignored paths");
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
  // Each file individually holds MORE than kMaxProjectSearchResults (200) matches, so a
  // single worker scanning one file sequentially is guaranteed to attempt a match past
  // the cap and set `truncated` — unlike a near-boundary fixture (250 total), where
  // parallel early-stop can cancel stragglers before any worker attempts the (cap+1)-th
  // match, leaving the default (early-stop) run's truncation flag racy.
  std::string repeated_lines;
  for (int line = 0; line < 250; ++line) {
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

  // Under parallel search the *which* 200 matches are kept is timing-dependent
  // (workers claim files in racing order), so we no longer assert an exact
  // first/last match. What must hold: the published set is well-formed and sorted
  // in (file_index, line, column) order by the consumer-mirroring helper.
  bool sorted = true;
  for (std::size_t i = 1; i < capped.results.size(); ++i) {
    const auto& prev = capped.results[i - 1];
    const auto& cur = capped.results[i];
    const bool ordered = prev.file_index < cur.file_index ||
                         (prev.file_index == cur.file_index &&
                          (prev.line < cur.line ||
                           (prev.line == cur.line && prev.column <= cur.column)));
    if (!ordered) {
      sorted = false;
      break;
    }
  }
  Expect(sorted, "capped project search results should be in (file, line, column) order");
  Expect(capped.results.front().column == 0 && capped.results.back().column == 0,
         "each capped 'alpha' match should start at column 0");

  // A default (early-stop) run cannot know the true total beyond the cap.
  Expect(capped.total_matches == 0,
         "default capped search should not report an exact total match count");

  // Count-all keeps scanning past the cap to report the exact total while still
  // only storing the first kMaxProjectSearchResults for display.
  ProjectSearchOptions count_all_options;
  count_all_options.count_all_matches = true;
  const auto counted = RunProjectSearch(root, "alpha", count_all_options);
  Expect(counted.finished, "count-all project search should finish");
  Expect(counted.error.empty(), "count-all project search should not error");
  Expect(counted.truncated, "count-all project search should still flag truncation");
  Expect(counted.results.size() == 200,
         "count-all project search should still cap the stored/displayed results");
  Expect(counted.total_matches == 2500,
         "count-all project search should report every match across all files");
}

// Regression: count-all must report the EXACT total even when the overwhelming
// majority of matches fall past the display cap. Past the cap the engine counts
// matches in a per-worker local (folded into the shared counter once at worker exit)
// instead of a per-match atomic fetch_add, so the fold arithmetic — including the
// boundary-race hand-off between the under-cap atomic path and the over-cap local
// path across multiple workers — must neither drop nor double-count a single match.
void TestProjectSearchServiceCountAllExactTotalFarPastCap() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  // 40 files x 60 matching lines = 2400 matches, 12x the 200 cap: the first 200 go
  // through the atomic slot-claim, the remaining 2200 through the local fold.
  std::string repeated_lines;
  for (int line = 0; line < 60; ++line) {
    repeated_lines += "needle\n";
  }
  for (int file_index = 0; file_index < 40; ++file_index) {
    const std::string label =
        (file_index < 10 ? "0" : "") + std::to_string(file_index);
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }

  ProjectSearchOptions count_all_options;
  count_all_options.count_all_matches = true;
  const auto counted = RunProjectSearch(root, "needle", count_all_options);
  Expect(counted.finished, "count-all search should finish");
  Expect(counted.error.empty(), "count-all search should not error");
  Expect(counted.truncated, "a far-past-cap count-all search must flag truncation");
  Expect(counted.results.size() == 200,
         "count-all still stores only the first cap for display");
  Expect(counted.total_matches == 40 * 60,
         "count-all must report every match exactly once across the cap boundary and all workers");
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
  const auto indexed_files = std::make_shared<const std::vector<std::filesystem::path>>(
      project::CollectProjectFiles(root, project::ProjectFileScanMode::ExcludeHidden));
  const std::uint64_t first_run_id = service.Start(root, "alpha", {}, indexed_files);
  const std::uint64_t second_run_id = service.Start(root, "omega", {}, indexed_files);

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
  const auto indexed_files = std::make_shared<const std::vector<std::filesystem::path>>(
      project::CollectProjectFiles(root, project::ProjectFileScanMode::ExcludeHidden));
  service.Start(root, "alpha", {}, indexed_files);
  service.Stop();

  // A correctly-stopped search must never surface a pending update. Rather than
  // checking once after a fixed sleep (which races a late worker publish), drain
  // continuously for a bounded window and assert nothing valid ever appears.
  bool saw_update = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto update = service.TakePendingUpdate();
    if (update.run_id != 0 || !update.results.empty() || update.finished) {
      saw_update = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(!saw_update,
         "stopped project search should never publish a pending update after Stop");
}

void TestProjectSearchServiceNoMatchFinishesPromptly() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nbeta\n");
  WriteFile(root / "b.txt", "gamma\ndelta\n");

  ProjectSearchService service;
  const auto indexed_files = std::make_shared<const std::vector<std::filesystem::path>>(
      project::CollectProjectFiles(root, project::ProjectFileScanMode::ExcludeHidden));
  const std::uint64_t run_id = service.Start(root, "zzz-not-present", {}, indexed_files);

  bool saw_finished = false;
  std::size_t total_results = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = service.TakePendingUpdate();
    if (update.run_id != run_id) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    total_results += update.results.size();
    if (update.finished) {
      saw_finished = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  service.Stop();
  Expect(saw_finished, "no-match project search should still publish completion promptly");
  Expect(total_results == 0,
         "no-match project search should not publish any intermediate or final results");
}

void TestProjectSearchServicePublishesProgressDenominator() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\n");
  WriteFile(root / "b.txt", "alpha\n");
  WriteFile(root / "c.txt", "beta\n");
  WriteFile(root / "d.txt", "gamma\n");

  const auto indexed = project::CollectProjectFiles(
      root, project::ProjectFileScanMode::ExcludeHidden);
  const auto run = RunProjectSearch(root, "alpha", {}, indexed);

  Expect(run.finished, "progress search should finish");
  Expect(run.final_total_files == indexed.size(),
         "total_files should equal the candidate-set size");
  Expect(run.final_searched_files == indexed.size(),
         "searched_files should reach total_files when the search runs to completion");
}

void TestProjectSearchServiceProgressPublishesBeforeFirstMatch() {
  // Sets a large no-match prefix so the worker visits many files before any
  // match. The denominator should be published immediately at Start, before any
  // results arrive — guarding the "0 of Y files" anchor on large empty repos.
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  for (int i = 0; i < 8; ++i) {
    WriteFile(root / ("noise_" + std::to_string(i) + ".txt"), "nothing\n");
  }
  WriteFile(root / "hit.txt", "needle\n");

  const auto indexed = std::make_shared<const std::vector<std::filesystem::path>>(
      project::CollectProjectFiles(root, project::ProjectFileScanMode::ExcludeHidden));
  ProjectSearchService service;
  const std::uint64_t run_id = service.Start(root, "needle", {}, indexed);

  std::size_t first_total_files_seen = 0;
  bool finished = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = service.TakePendingUpdate();
    if (update.run_id == run_id) {
      if (first_total_files_seen == 0 && update.total_files > 0) {
        first_total_files_seen = update.total_files;
      }
      if (update.finished) {
        finished = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  service.Stop();

  Expect(finished, "search with progress publishing should still finish");
  Expect(first_total_files_seen == indexed->size(),
         "first progress publish should expose the full denominator");
}

void TestProjectFileScannerTerminatesOnSymlinkLoop() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "sub" / "real.txt", "hello\n");

  // A directory symlink whose target is an ancestor turns the tree into a
  // cycle: without a visited-target guard, CollectProjectFiles would recurse
  // sub/loop/sub/loop/... forever. The scan must terminate and index the real
  // file, following the symlink at most a bounded number of times.
  std::error_code link_error;
  std::filesystem::create_directory_symlink(root, root / "sub" / "loop", link_error);
  Expect(!link_error, "fixture should create an ancestor-referential directory symlink");

  const auto files =
      project::CollectProjectFiles(root, project::ProjectFileScanMode::IncludeHidden);

  std::size_t real_file_hits = 0;
  for (const auto& file : files) {
    if (file == std::filesystem::path("sub") / "real.txt") {
      ++real_file_hits;
    }
  }
  Expect(real_file_hits == 1, "the real file should be indexed exactly once by its real path");
  // Termination alone proves the cycle was cut; the bound guards against a
  // regression that follows the loop several extra times before stopping.
  Expect(files.size() < 16, "an ancestor symlink loop must not inflate the scan unbounded");
}

}  // namespace

// TD-2026-07-17-034: a pathological (catastrophic-backtracking) regex must not
// monopolize a search worker. RegexUtil sets a PCRE2 match-step limit
// (kRegexMatchLimit), so a classic exponential pattern hits the limit in bounded
// steps and the search FINISHES quickly rather than hanging. Without the limit the
// pattern below on ~45 non-terminating chars would take ~2^45 steps.
void TestProjectSearchServiceCatastrophicRegexIsBounded() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  // A run of 'a' with no trailing 'b' forces (a+)+b to backtrack exponentially.
  WriteFile(root / "evil.txt", std::string(45, 'a') + "\n");

  ProjectSearchOptions regex_options;
  regex_options.pattern_mode = ProjectSearchPatternMode::Regex;

  const auto start = std::chrono::steady_clock::now();
  const auto result = RunProjectSearch(root, "(a+)+b", regex_options);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  Expect(result.finished,
         "a catastrophic-backtracking regex search must finish (match-step limit fires)");
  // RunProjectSearch's own deadline is 2s; a bounded match limit finishes far faster.
  // Assert comfortably under the deadline so a regression that removed the limit
  // (and hung to the deadline) would fail here.
  Expect(elapsed < std::chrono::milliseconds(1500),
         "the bounded match limit must let the search complete well before the deadline");
}

// VSCode-style scope filters. The point of an include/exclude box is not just a
// narrower result list — an out-of-scope file must never be opened, which is the
// only reason scoping is faster than filtering results after the fact.
void TestProjectSearchServiceScopeGlobsRestrictCandidates() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  WriteFile(root / "src" / "main.cpp", "needle\n");
  WriteFile(root / "src" / "main.h", "needle\n");
  WriteFile(root / "src" / "generated" / "tables.cpp", "needle\n");
  WriteFile(root / "tests" / "MainTests.cpp", "needle\n");
  WriteFile(root / "README.md", "needle\n");

  const auto unscoped = RunProjectSearch(root, "needle");
  Expect(unscoped.finished, "unscoped search should finish");
  Expect(unscoped.results.size() == 5, "unscoped search should match every file");

  ProjectSearchOptions include_only;
  include_only.include_globs = "*.cpp";
  const auto included = RunProjectSearch(root, "needle", include_only);
  Expect(included.finished, "include-scoped search should finish");
  Expect(included.error.empty(), "include-scoped search should not error");
  Expect(included.results.size() == 3, "'*.cpp' should float to every .cpp at any depth");

  ProjectSearchOptions with_exclude = include_only;
  with_exclude.exclude_globs = "**/generated/**, tests";
  const auto scoped = RunProjectSearch(root, "needle", with_exclude);
  Expect(scoped.finished, "scoped search should finish");
  Expect(scoped.results.size() == 1, "exclude globs should subtract from the include set");
  Expect(scoped.results[0].relative_path == std::filesystem::path("src/main.cpp"),
         "only src/main.cpp should survive both scope filters");

  ProjectSearchOptions exclude_only;
  exclude_only.exclude_globs = "*.md";
  const auto without_docs = RunProjectSearch(root, "needle", exclude_only);
  Expect(without_docs.finished, "exclude-only search should finish");
  Expect(without_docs.results.size() == 4,
         "an empty include box should mean 'everything', not 'nothing'");
}

// The scope filter must reject on the path alone. A file whose content would match
// but whose path is out of scope contributes nothing, and the skip is counted so a
// regression that reads the file anyway is visible rather than merely slower.
void TestProjectSearchServiceScopeGlobsSkipFilesWithoutReading() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  for (int index = 0; index < 8; ++index) {
    WriteFile(root / "skipped" / (std::to_string(index) + ".txt"), "needle\n");
  }
  WriteFile(root / "kept.cpp", "needle\n");

  ProjectSearchOptions options;
  options.include_globs = "*.cpp";

  util::ResetPerformanceCounters();
  const auto scoped = RunProjectSearch(root, "needle", options);
  Expect(scoped.finished, "scoped search should finish");
  Expect(scoped.results.size() == 1, "only the in-scope file should match");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectScopeFilteredFiles) == 8,
         "every out-of-scope file should be rejected on its path, before any read");

  util::ResetPerformanceCounters();
  const auto unscoped = RunProjectSearch(root, "needle");
  Expect(unscoped.results.size() == 9, "the unscoped run should still see every file");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectScopeFilteredFiles) == 0,
         "an inactive scope must not run the filter at all");
}

void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectSearchService/CatastrophicRegexIsBounded",
          TestProjectSearchServiceCatastrophicRegexIsBounded);
  AddTest(tests, "ProjectSearchService/LiteralModesAndCaseControls",
          TestProjectSearchServiceLiteralModesAndCaseControls);
  AddTest(tests, "ProjectSearchService/UnicodeCaseFolding",
          TestProjectSearchServiceUnicodeCaseFolding);
  AddTest(tests, "ProjectSearchService/NormalizesPreviewWhitespace",
          TestProjectSearchServiceNormalizesPreviewWhitespace);
  AddTest(tests, "ProjectSearchService/RegexModeAndInvalidRegex",
          TestProjectSearchServiceRegexModeAndInvalidRegex);
  AddTest(tests, "ProjectSearchService/RegexCaseInsensitiveFoldsNonAscii",
          TestProjectSearchServiceRegexCaseInsensitiveFoldsNonAscii);
  AddTest(tests, "ProjectSearchService/RegexEmptyMatchDoesNotHideRealMatch",
          TestProjectSearchServiceRegexEmptyMatchDoesNotHideRealMatch);
  AddTest(tests, "ProjectSearchService/RegexFindNextIsCancellable",
          TestProjectSearchServiceRegexFindNextIsCancellable);
  AddTest(tests, "ProjectSearchService/HiddenAndBinaryFiles",
          TestProjectSearchServiceHiddenAndBinaryFiles);
  AddTest(tests, "ProjectSearchService/ScopeGlobsRestrictCandidates",
          TestProjectSearchServiceScopeGlobsRestrictCandidates);
  AddTest(tests, "ProjectSearchService/ScopeGlobsSkipFilesWithoutReading",
          TestProjectSearchServiceScopeGlobsSkipFilesWithoutReading);
  AddTest(tests, "ProjectSearchService/ExcludesIgnoredFilesByDefault",
          TestProjectSearchServiceExcludesIgnoredFilesByDefault);
  AddTest(tests, "ProjectSearchService/PublishesStableResultOrdering",
          TestProjectSearchServicePublishesStableResultOrdering);
  AddTest(tests, "ProjectSearchService/FlagsTruncatedLargeResultSets",
          TestProjectSearchServiceFlagsTruncatedLargeResultSets);
  AddTest(tests, "ProjectSearchService/CountAllExactTotalFarPastCap",
          TestProjectSearchServiceCountAllExactTotalFarPastCap);
  AddTest(tests, "ProjectSearchService/RestartPublishesOnlyLatestRun",
          TestProjectSearchServiceRestartPublishesOnlyLatestRun);
  AddTest(tests, "ProjectSearchService/StopDiscardsLateUpdates",
          TestProjectSearchServiceStopDiscardsLateUpdates);
  AddTest(tests, "ProjectSearchService/UsesIndexedFileSnapshotWhenProvided",
          TestProjectSearchServiceUsesIndexedFileSnapshotWhenProvided);
  AddTest(tests, "ProjectSearchService/NoMatchFinishesPromptly",
          TestProjectSearchServiceNoMatchFinishesPromptly);
  AddTest(tests, "ProjectSearchService/PublishesProgressDenominator",
          TestProjectSearchServicePublishesProgressDenominator);
  AddTest(tests, "ProjectSearchService/ProgressPublishesBeforeFirstMatch",
          TestProjectSearchServiceProgressPublishesBeforeFirstMatch);
  AddTest(tests, "ProjectFileScanner/TerminatesOnSymlinkLoop",
          TestProjectFileScannerTerminatesOnSymlinkLoop);
}

}  // namespace microide::tests
