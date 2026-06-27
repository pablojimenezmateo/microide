#include "TestSupport.h"

#include "editor/TextBuffer.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuildGitSidebarLineSpecs;
using microide::workspace::BuildGitSidebarEntryTextModel;
using microide::workspace::BuildGitSidebarViewModel;
using microide::workspace::BuildProjectSearchResultLineMap;
using microide::workspace::FindLiteralSearchMatches;
using microide::workspace::FindProjectSearchResultLine;
using microide::workspace::FindSelectedGitSidebarLineIndex;
using microide::workspace::GitSidebarEntry;
using microide::workspace::GitSidebarLineKind;
using microide::workspace::GitSidebarState;
using microide::compare::BranchReviewStateService;
using microide::workspace::QueryExtendsCaseInsensitive;
using microide::workspace::QuerySupportsLiteralReplace;
using microide::workspace::RefineLiteralSearchMatches;
using microide::workspace::ReplaceLiteralMatchesInText;
using microide::workspace::UsesCaseSensitiveLiteralMatch;

void TestWorkspaceSharedGitSidebarLineHelpers() {
  GitSidebarState git_state;
  git_state.repo_available = true;
  git_state.base_ref = "origin/main";
  git_state.base_label = "origin/main";
  git_state.entries = {
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/alpha.cpp",
                      .relative_path = "src/alpha.cpp",
                      .provider_id = {},
                      .provider_label = {}},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/beta.cpp",
                      .relative_path = "src/beta.cpp",
                      .provider_id = {},
                      .provider_label = {}},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Outgoing,
                      .path = "src/gamma.cpp",
                      .relative_path = "src/gamma.cpp",
                      .provider_id = {},
                      .provider_label = {}},
  };
  const BranchReviewStateService branch_review;
  const auto lines = BuildGitSidebarLineSpecs(BuildGitSidebarViewModel(
      git_state, std::filesystem::path{"/tmp/project"}, branch_review));
  Expect(lines.size() == 13,
         "git sidebar lines should include workflow headers, tree rows, and empty placeholders");
  Expect(lines[4].kind == GitSidebarLineKind::Header && lines[4].label == "Unstaged (2)",
         "git sidebar lines should include the unstaged header with count");
  Expect(lines[5].kind == GitSidebarLineKind::Directory && lines[5].label == "src",
         "git sidebar lines should insert a directory row for nested changed files");
  Expect(lines[6].kind == GitSidebarLineKind::Entry && lines[6].entry_index == 0 &&
             lines[6].depth == 1,
         "git sidebar lines should map the first changed entry under the directory");
  Expect(lines[7].kind == GitSidebarLineKind::Entry && lines[7].entry_index == 1 &&
             lines[7].depth == 1,
         "git sidebar lines should map the second changed entry under the directory");
  Expect(lines[10].kind == GitSidebarLineKind::Header &&
             lines[10].label == "Outgoing (1)  origin/main",
         "git sidebar lines should include the outgoing header with base label");
  Expect(lines[11].kind == GitSidebarLineKind::Directory && lines[11].label == "src",
         "git sidebar lines should also tree-group outgoing files by directory");
  Expect(lines[12].kind == GitSidebarLineKind::Entry && lines[12].entry_index == 2 &&
             lines[12].depth == 1,
         "git sidebar lines should map outgoing entries after the directory row");

  const auto selected_line = FindSelectedGitSidebarLineIndex(lines, 2);
  Expect(selected_line.has_value() && *selected_line == 12,
         "git sidebar selected-line lookup should find the outgoing entry row");
  Expect(!FindSelectedGitSidebarLineIndex(lines, 9).has_value(),
         "git sidebar selected-line lookup should fail for unknown entries");
}

void TestWorkspaceSharedGitSidebarEmptyStates() {
  GitSidebarState clean_state;
  clean_state.repo_available = true;
  const BranchReviewStateService clean_branch_review;
  const auto clean_lines = BuildGitSidebarLineSpecs(BuildGitSidebarViewModel(
      clean_state, std::filesystem::path{"/tmp/project"}, clean_branch_review));
  Expect(clean_lines.size() == 10,
         "empty git sidebar should still show all workflow section headers and empty rows");
  Expect(clean_lines[5].kind == GitSidebarLineKind::Empty &&
             clean_lines[5].label == "No unstaged changes",
         "git sidebar should describe a clean changed section when git is available");

  GitSidebarState no_repo_state;
  const BranchReviewStateService no_repo_branch_review;
  const auto no_repo_lines = BuildGitSidebarLineSpecs(BuildGitSidebarViewModel(
      no_repo_state, std::filesystem::path{"/tmp/project"}, no_repo_branch_review));
  Expect(no_repo_lines[5].label == "Not a git repository",
         "git sidebar should distinguish non-repositories from clean repositories");
}

void TestWorkspaceSharedGitSidebarEntryTextModel() {
  const auto nested = BuildGitSidebarEntryTextModel(std::filesystem::path("src/deep/main.cpp"), false);
  Expect(nested.primary_label == "main.cpp",
         "git sidebar text model should prioritize the filename");
  Expect(nested.secondary_label == "src/deep",
         "git sidebar text model should move the parent path into the secondary label");

  const auto staged = BuildGitSidebarEntryTextModel(std::filesystem::path("main.cpp"), true);
  Expect(staged.primary_label == "main.cpp",
         "git sidebar text model should preserve root-level filenames");
  Expect(staged.secondary_label == "[staged]",
         "git sidebar text model should keep staged state in the secondary label");
}

void TestWorkspaceSharedGitSidebarDirectoryCollapse() {
  GitSidebarState git_state;
  git_state.repo_available = true;
  git_state.entries = {
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/deep/alpha.cpp",
                      .relative_path = "src/deep/alpha.cpp",
                      .provider_id = {},
                      .provider_label = {}},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/deep/beta.cpp",
                      .relative_path = "src/deep/beta.cpp",
                      .provider_id = {},
                      .provider_label = {}},
  };
  const BranchReviewStateService branch_review;
  const auto expanded_lines = BuildGitSidebarLineSpecs(BuildGitSidebarViewModel(
      git_state, std::filesystem::path{"/tmp/project"}, branch_review));
  std::optional<std::string> src_key;
  for (const auto& line : expanded_lines) {
    if (line.kind == GitSidebarLineKind::Directory && line.label == "src") {
      src_key = line.tree_node_key;
      break;
    }
  }
  Expect(src_key.has_value(),
         "expanded git tree should expose a collapsible src directory node key");

  std::unordered_set<std::string> collapsed{*src_key};
  const auto collapsed_lines = BuildGitSidebarLineSpecs(
      BuildGitSidebarViewModel(git_state, std::filesystem::path{"/tmp/project"}, branch_review),
      &collapsed);
  Expect(std::none_of(collapsed_lines.begin(), collapsed_lines.end(),
                      [](const auto& line) { return line.kind == GitSidebarLineKind::Entry; }),
         "collapsed top-level git directory should hide nested file entries");
  Expect(std::any_of(collapsed_lines.begin(), collapsed_lines.end(),
                     [](const auto& line) {
                       return line.kind == GitSidebarLineKind::Directory &&
                              line.label == "src" && !line.expanded;
                     }),
         "collapsed git directory row should expose collapsed visual state");
}

void TestWorkspaceSharedLiteralSearchHelpers() {
  const std::vector<std::string> lines = {
      "Alpha alpha",
      "beta ALPHA",
  };
  const auto matches = FindLiteralSearchMatches(lines, "alpha");
  Expect(matches.size() == 3,
         "literal search helper should find case-insensitive matches across lines");
  Expect(matches[0].start.line == 0 && matches[0].start.column == 0,
         "literal search helper should include the first line-leading match");
  Expect(matches[1].start.line == 0 && matches[1].start.column == 6,
         "literal search helper should include later matches on the same line");
  Expect(matches[2].start.line == 1 && matches[2].start.column == 5,
         "literal search helper should include matches on subsequent lines");

  std::string content = "Alpha alpha ALPHA";
  Expect(ReplaceLiteralMatchesInText(content, "alpha", "beta", false) == 3,
         "literal replace helper should replace case-insensitive matches");
  Expect(content == "beta beta beta", "literal replace helper should rewrite all matching ranges");

  std::string case_sensitive = "Alpha alpha";
  Expect(ReplaceLiteralMatchesInText(case_sensitive, "Alpha", "Beta", true) == 1,
         "literal replace helper should respect case-sensitive replacement mode");
  Expect(case_sensitive == "Beta alpha",
         "literal replace helper should preserve non-matching case variants");

  std::string expanded = "Alpha alpha ALPHA";
  Expect(ReplaceLiteralMatchesInText(expanded, "alpha", "replacement", false) == 3,
         "literal replace helper should replace all case-insensitive matches when expanding text");
  Expect(expanded == "replacement replacement replacement",
         "literal replace helper should keep replacement offsets correct when replacement is longer");

  std::string shrunk = "Alpha alpha ALPHA";
  Expect(ReplaceLiteralMatchesInText(shrunk, "alpha", "x", false) == 3,
         "literal replace helper should replace all case-insensitive matches when shrinking text");
  Expect(shrunk == "x x x",
         "literal replace helper should keep replacement offsets correct when replacement is shorter");
}

void TestWorkspaceSharedLiteralReplaceModeHelpers() {
  Expect(QuerySupportsLiteralReplace("alpha"),
         "literal replace helper should accept plain queries");
  Expect(!QuerySupportsLiteralReplace("alpha.*"),
         "literal replace helper should reject regex metacharacters");
  Expect(!QuerySupportsLiteralReplace(""),
         "literal replace helper should reject empty queries");
  Expect(UsesCaseSensitiveLiteralMatch("Alpha"),
         "uppercase queries should enable case-sensitive mode");
  Expect(!UsesCaseSensitiveLiteralMatch("alpha"),
         "lowercase queries should keep case-insensitive mode");
}

void TestWorkspaceLiteralNeedleScanCaseModeInLine() {
  using microide::workspace::FindLiteralNeedleInLine;

  Expect(FindLiteralNeedleInLine("Foo foo FOO", 0, "foo", /*case_sensitive=*/false) == 0,
         "case-insensitive scan should prefer the earliest offset");
  Expect(FindLiteralNeedleInLine("Foo foo FOO", 1, "foo", /*case_sensitive=*/false) == 4,
         "case-insensitive scan should skip earlier matches outside the slice");
  Expect(!FindLiteralNeedleInLine("x Foo y", 0, "foo", /*case_sensitive=*/true).has_value(),
         "case-sensitive scan should reject byte mismatches against the seeded needle text");
  const auto matched = FindLiteralNeedleInLine("Foo foo", 0, "Foo", /*case_sensitive=*/true);
  Expect(matched.has_value() && *matched == 0,
         "case-sensitive scan should locate exact-case substrings");

  Expect(FindLiteralNeedleInLine("ababa", 0, "aba", /*case_sensitive=*/true) == 0,
         "overlapping case-sensitive occurrences should expose the earliest match index");
}

void TestWorkspaceNextLiteralMatchAfterSeedWrapOnce() {
  using microide::workspace::FindNextLiteralMatchAfterSeedWrapOnce;

  auto expect_pos = [&](const std::optional<microide::editor::TextPosition>& got,
                        std::size_t line,
                        std::size_t column,
                        const char* message) {
    Expect(got.has_value(), message);
    if (got.has_value()) {
      Expect(got->line == line && got->column == column, message);
    }
  };

  auto expect_missing = [&](const std::optional<microide::editor::TextPosition>& got,
                            const char* message) {
    Expect(!got.has_value(), message);
  };

  {
    const std::vector<std::string> dual{"foo foo"};
    const auto forward_then_wrap =
        FindNextLiteralMatchAfterSeedWrapOnce(dual, 0, 4, 7, "foo", /*case_sensitive=*/true);
    expect_pos(forward_then_wrap, 0, 0,
               "after the last occurrence on the line, wrap picks the earliest match");
    const auto forward_only =
        FindNextLiteralMatchAfterSeedWrapOnce(dual, 0, 0, 3, "foo", /*case_sensitive=*/true);
    expect_pos(forward_only, 0, 4,
               "forward scan prefers the occurrence after the seed span");

    const std::vector<std::string> single{"foo"};
    const auto lone =
        FindNextLiteralMatchAfterSeedWrapOnce(single, 0, 0, 3, "foo", /*case_sensitive=*/true);
    expect_missing(lone, "only the seed occurrence yields no candidate after a single wrap");

    const std::vector<std::string> above_first{"foo", "foo"};
    const auto earlier_line = FindNextLiteralMatchAfterSeedWrapOnce(
        above_first, 1, 0, 3, "foo", /*case_sensitive=*/true);
    expect_pos(earlier_line, 0, 0,
               "after the seed line exhausts forward matches, wrap finds an earlier line first");
  }

  {
    const std::vector<std::string> mixed_case_line{"Foo foo"};
    const auto forward_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        mixed_case_line, 0, 0, 3, "Foo", /*case_sensitive=*/false);
    expect_pos(forward_ci, 0, 4,
               "case-insensitive forward finds the next byte-different match on the same line");

    const std::vector<std::string> wrap_mixed{"foo FOO"};
    const auto wrap_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        wrap_mixed, 0, 4, 7, "FOO", /*case_sensitive=*/false);
    expect_pos(wrap_ci, 0, 0,
               "case-insensitive wrap reaches the earlier mixed-case match before the seed span");

    const std::vector<std::string> lone_mixed{"Foo"};
    const auto none_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        lone_mixed, 0, 0, 3, "Foo", /*case_sensitive=*/false);
    expect_missing(none_ci,
                   "case-insensitive wrap still skips the sole seeded span and finds nothing else");
  }

  {
    using Lines = std::vector<std::string>;
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{}, 0, 0, 0, "x", false),
                   "empty lines buffer yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"a"}, 1, 0, 1, "a", false),
                   "seed line past end of buffer yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"a"}, 0, 0, 1, "", false),
                   "empty needle yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"foo"}, 0, 0, 10, "foo", false),
                   "seed end column past the line length yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"foo"}, 0, 4, 7, "foo", false),
                   "seed start column past the line length yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"foo"}, 0, 2, 1, "foo", false),
                   "inverted seed span yields no match");
    expect_missing(FindNextLiteralMatchAfterSeedWrapOnce(Lines{"Foo"}, 0, 0, 3, "foo", true),
                   "case-sensitive scan does not match a different-cased needle to the buffer");
  }
}

void TestWorkspaceSharedProjectSearchLineMapHelpers() {
  const std::vector<microide::project::ProjectSearchResult> results = {
      {.relative_path = std::filesystem::path("src/a.cpp"),
       .relative_path_string = "src/a.cpp",
       .line = 0,
       .column = 1,
       .preview = "first"},
      {.relative_path = std::filesystem::path("src/a.cpp"),
       .relative_path_string = "src/a.cpp",
       .line = 3,
       .column = 2,
       .preview = "second"},
      {.relative_path = std::filesystem::path("src/b.cpp"),
       .relative_path_string = "src/b.cpp",
       .line = 1,
       .column = 0,
       .preview = "third"},
  };
  const auto line_map = BuildProjectSearchResultLineMap(results);
  Expect(line_map.size() == 5, "project search line map should insert header rows per file");
  Expect(line_map[0] == -1 && line_map[1] == 0 && line_map[2] == 1,
         "project search line map should group consecutive results under one header");
  Expect(line_map[3] == -1 && line_map[4] == 2,
         "project search line map should insert a new header when the file changes");
  Expect(FindProjectSearchResultLine(line_map, 2) == 4,
         "project search line lookup should return the visible row for a result index");
  Expect(FindProjectSearchResultLine(line_map, 99) == 0,
         "project search line lookup should fall back to zero for missing results");
}

// The incremental find-as-you-type path must produce exactly what a fresh full
// scan would: for every query that extends a prefix, refining the prefix's match
// set over the buffer equals scanning the whole buffer for the longer query.
void TestWorkspaceIncrementalLiteralSearch() {
  auto same = [](const std::vector<microide::editor::SelectionRange>& a,
                 const std::vector<microide::editor::SelectionRange>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].start.line != b[i].start.line || a[i].start.column != b[i].start.column ||
          a[i].end.line != b[i].end.line || a[i].end.column != b[i].end.column) {
        return false;
      }
    }
    return true;
  };

  Expect(QueryExtendsCaseInsensitive("ab", "abc"), "extend should accept appended characters");
  Expect(QueryExtendsCaseInsensitive("AB", "abc"), "extend should be case-insensitive");
  Expect(QueryExtendsCaseInsensitive("abc", "abc"), "extend should accept the equal query");
  Expect(!QueryExtendsCaseInsensitive("abc", "ab"), "extend should reject a shorter query");
  Expect(!QueryExtendsCaseInsensitive("ax", "abc"), "extend should reject a divergent prefix");
  Expect(QueryExtendsCaseInsensitive("", "abc"), "empty prefix extends anything");

  const std::vector<std::string> lines = {
      "Alpha alphabet ALPHA",
      "no match here",
      "alpha and Alphanumeric alph",
      "aLpHaBeT",
  };
  microide::editor::TextBuffer buffer(lines);

  // The TextBuffer overload must agree with the vector overload.
  Expect(same(FindLiteralSearchMatches(buffer, "alpha"), FindLiteralSearchMatches(lines, "alpha")),
         "buffer-scan overload should match the vector overload");

  // The seed-relative next-match (Ctrl-D) buffer overload must also agree with the
  // vector overload it replaced.
  using microide::workspace::FindNextLiteralMatchAfterSeedWrapOnce;
  for (std::size_t seed = 0; seed < lines.size(); ++seed) {
    const auto from_vec =
        FindNextLiteralMatchAfterSeedWrapOnce(lines, seed, 0, 0, "alpha", false);
    const auto from_buf =
        FindNextLiteralMatchAfterSeedWrapOnce(buffer, seed, 0, 0, "alpha", false);
    Expect(from_vec.has_value() == from_buf.has_value() &&
               (!from_vec.has_value() ||
                (from_vec->line == from_buf->line && from_vec->column == from_buf->column)),
           "next-match buffer overload should match the vector overload");
  }

  // Refining each prefix step must equal a fresh scan for the longer query --
  // this is exactly the substitution RefreshBufferSearch makes per keystroke.
  const std::vector<std::string> queries = {"a", "al", "alp", "alph", "alpha", "alphab", "alphabe"};
  for (std::size_t i = 1; i < queries.size(); ++i) {
    const auto previous = FindLiteralSearchMatches(buffer, queries[i - 1]);
    const auto refined = RefineLiteralSearchMatches(buffer, queries[i], previous);
    const auto fresh = FindLiteralSearchMatches(buffer, queries[i]);
    Expect(same(refined, fresh),
           "incremental refine of a prefix match set must equal a fresh full scan");
  }

  // A refine that starts from an empty match set yields nothing (callers must not
  // refine across a non-extending query); and refine over an empty query is empty.
  Expect(RefineLiteralSearchMatches(buffer, "alpha", {}).empty(),
         "refine over an empty previous set yields nothing");
  Expect(RefineLiteralSearchMatches(buffer, "", FindLiteralSearchMatches(buffer, "a")).empty(),
         "refine with an empty query yields nothing");
}

}  // namespace

void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceGitSidebarPresentation/LineHelpers",
          TestWorkspaceSharedGitSidebarLineHelpers);
  AddTest(tests, "WorkspaceGitSidebarPresentation/EmptyStates",
          TestWorkspaceSharedGitSidebarEmptyStates);
  AddTest(tests, "WorkspaceGitSidebarPresentation/EntryTextModel",
          TestWorkspaceSharedGitSidebarEntryTextModel);
  AddTest(tests, "WorkspaceGitSidebarPresentation/DirectoryCollapse",
          TestWorkspaceSharedGitSidebarDirectoryCollapse);
  AddTest(tests, "WorkspaceTextSearch/LiteralSearchHelpers",
          TestWorkspaceSharedLiteralSearchHelpers);
  AddTest(tests, "WorkspaceTextSearch/LiteralReplaceModeHelpers",
          TestWorkspaceSharedLiteralReplaceModeHelpers);
  AddTest(tests, "WorkspaceTextSearch/LiteralNeedleScanCaseModeInLine",
          TestWorkspaceLiteralNeedleScanCaseModeInLine);
  AddTest(tests, "WorkspaceTextSearch/NextLiteralMatchAfterSeedWrapOnce",
          TestWorkspaceNextLiteralMatchAfterSeedWrapOnce);
  AddTest(tests, "WorkspaceTextSearch/IncrementalLiteralSearch",
          TestWorkspaceIncrementalLiteralSearch);
  AddTest(tests, "WorkspaceProjectSearchPresentation/LineMapHelpers",
          TestWorkspaceSharedProjectSearchLineMapHelpers);
}

}  // namespace microide::tests
