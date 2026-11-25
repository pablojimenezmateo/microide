#include "TestSupport.h"

#include "editor/TextBuffer.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

#include <filesystem>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuildGitSidebarEntryTextModel;
using microide::workspace::BuildGitSidebarViewModel;
using microide::workspace::BuildProjectSearchResultLineMap;
using microide::workspace::FindLiteralSearchMatches;
using microide::workspace::FindProjectSearchResultLine;
using microide::workspace::FindSelectedGitSidebarLineIndex;
using microide::workspace::GitSidebarEntry;
using microide::workspace::BuildGitSidebarLines;
using GitSidebarLineKind = microide::workspace::GitSidebarLine::Kind;
using microide::workspace::GitSidebarState;
using microide::compare::BranchReviewStateService;
using microide::workspace::QueryExtendsCaseInsensitive;
using microide::workspace::QuerySupportsLiteralReplace;
using microide::workspace::RefineLiteralSearchMatches;
using microide::workspace::FindRegexSearchMatches;
using microide::workspace::ReplaceLiteralMatchesInText;
using microide::workspace::ReplaceRegexMatchesInText;
using microide::workspace::SplitRegexMatchHighlightFragments;
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
  const auto lines = BuildGitSidebarLines(BuildGitSidebarViewModel(
      git_state, std::filesystem::path{"/tmp/project"}, branch_review));
  Expect(lines.size() == 7,
         "git sidebar lines should cover the populated groups and their tree rows only");
  Expect(lines[0].kind == GitSidebarLineKind::Header && lines[0].label == "Unstaged (2)",
         "git sidebar lines should include the unstaged header with count");
  Expect(lines[1].kind == GitSidebarLineKind::Directory && lines[1].label == "src",
         "git sidebar lines should insert a directory row for nested changed files");
  Expect(lines[2].kind == GitSidebarLineKind::Entry && lines[2].entry_index == 0 &&
             lines[2].depth == 1,
         "git sidebar lines should map the first changed entry under the directory");
  Expect(lines[3].kind == GitSidebarLineKind::Entry && lines[3].entry_index == 1 &&
             lines[3].depth == 1,
         "git sidebar lines should map the second changed entry under the directory");
  Expect(lines[4].kind == GitSidebarLineKind::Header &&
             lines[4].label == "Outgoing (1)  origin/main",
         "git sidebar lines should include the outgoing header with base label");
  Expect(lines[5].kind == GitSidebarLineKind::Directory && lines[5].label == "src",
         "git sidebar lines should also tree-group outgoing files by directory");
  Expect(lines[6].kind == GitSidebarLineKind::Entry && lines[6].entry_index == 2 &&
             lines[6].depth == 1,
         "git sidebar lines should map outgoing entries after the directory row");

  const auto selected_line = FindSelectedGitSidebarLineIndex(lines, 2);
  Expect(selected_line.has_value() && *selected_line == 6,
         "git sidebar selected-line lookup should find the outgoing entry row");
  Expect(!FindSelectedGitSidebarLineIndex(lines, 9).has_value(),
         "git sidebar selected-line lookup should fail for unknown entries");
}

void TestWorkspaceSharedGitSidebarEmptyStates() {
  GitSidebarState clean_state;
  clean_state.repo_available = true;
  const BranchReviewStateService clean_branch_review;
  const auto clean_lines = BuildGitSidebarLines(BuildGitSidebarViewModel(
      clean_state, std::filesystem::path{"/tmp/project"}, clean_branch_review));
  Expect(clean_lines.size() == 3,
         "a clean git sidebar should collapse to one line plus the outgoing group");
  Expect(clean_lines[0].kind == GitSidebarLineKind::Empty && clean_lines[0].label == "No changes",
         "git sidebar should describe a clean tree once instead of per empty section");
  Expect(clean_lines[1].kind == GitSidebarLineKind::Header &&
             clean_lines[1].label == "Outgoing (0)",
         "the outgoing group should survive an empty tree so its base button stays reachable");
  Expect(clean_lines[2].kind == GitSidebarLineKind::Empty &&
             clean_lines[2].label == "Base branch unavailable",
         "git sidebar should keep reporting a missing base branch");

  GitSidebarState no_repo_state;
  const BranchReviewStateService no_repo_branch_review;
  const auto no_repo_lines = BuildGitSidebarLines(BuildGitSidebarViewModel(
      no_repo_state, std::filesystem::path{"/tmp/project"}, no_repo_branch_review));
  Expect(no_repo_lines.size() == 1 && no_repo_lines[0].label == "Not a git repository",
         "git sidebar should distinguish non-repositories from clean repositories");
}

void TestWorkspaceSharedGitSidebarEntryTextModel() {
  const auto nested = BuildGitSidebarEntryTextModel("src/deep/main.cpp", false);
  Expect(nested.primary_label == "main.cpp",
         "git sidebar text model should prioritize the filename");
  Expect(nested.secondary_label == "src/deep",
         "git sidebar text model should move the parent path into the secondary label");

  const auto staged = BuildGitSidebarEntryTextModel("main.cpp", true);
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
  const auto expanded_lines = BuildGitSidebarLines(BuildGitSidebarViewModel(
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
  const auto collapsed_lines = BuildGitSidebarLines(
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

void TestWorkspaceSharedLiteralSearchUtf8Fold() {
  const std::vector<std::string> lines = {"café CAFÉ", "ΔELTA δelta"};
  const auto cafe = FindLiteralSearchMatches(lines, "café");
  Expect(cafe.size() == 2, "buffer search folds café/CAFÉ as a case-insensitive match");
  Expect(cafe[0].start.line == 0 && cafe[0].start.column == 0, "first café at column 0");
  Expect(cafe[1].start.line == 0 && cafe[1].start.column == 6,
         "second (uppercase) match's byte offset is preserved by the length-preserving fold");

  const auto delta = FindLiteralSearchMatches(lines, "δ");
  Expect(delta.size() == 2, "Greek Δ/δ fold as a case-insensitive match");

  Expect(UsesCaseSensitiveLiteralMatch("CAFÉ"),
         "a non-ASCII uppercase query enables case-sensitive smart-case");
  Expect(!UsesCaseSensitiveLiteralMatch("café"),
         "an all-lowercase non-ASCII query stays case-insensitive");

  std::string content = "café CAFÉ";
  Expect(ReplaceLiteralMatchesInText(content, "café", "tea", false) == 2,
         "replace folds non-ASCII case variants like the visible match list");
  Expect(content == "tea tea", "both case variants are replaced");
}

void TestWorkspaceSharedLiteralSearchDoesNotOverlap() {
  const std::vector<std::string> lines = {"aaaa", "banana"};
  const auto matches = FindLiteralSearchMatches(lines, "aa");
  Expect(matches.size() == 2,
         "literal find-all should return non-overlapping matches for a self-overlapping needle");
  Expect(matches[0].start.line == 0 && matches[0].start.column == 0 &&
             matches[0].end.column == 2,
         "first non-overlapping match should cover [0,2)");
  Expect(matches[1].start.line == 0 && matches[1].start.column == 2 &&
             matches[1].end.column == 4,
         "second non-overlapping match should cover [2,4)");

  const auto ana = FindLiteralSearchMatches(lines, "ana");
  Expect(ana.size() == 1,
         "literal find-all should not double-count the overlapping 'ana' in 'banana'");

  std::string content = "aaaa";
  Expect(ReplaceLiteralMatchesInText(content, "aa", "aa", false) == 2 &&
             FindLiteralSearchMatches({content}, "aa").size() == 2,
         "find-all count and replace count must agree for overlapping needles");
}

void TestWorkspaceSharedRegexReplaceInText() {
  const microide::util::CompiledRegex swap("(\\w+)=(\\w+)", 0);
  Expect(swap.valid(), "regex-replace fixture should compile");

  std::string content = "a=1 b=2\nc=3\n";
  const auto count = ReplaceRegexMatchesInText(content, swap, "$2:$1");
  Expect(count.has_value() && *count == 3,
         "regex replace should report the total per-line substitution count");
  Expect(content == "1:a 2:b\n3:c\n",
         "regex replace should expand capture groups and preserve newline terminators");

  const microide::util::CompiledRegex tail("o$", 0);
  std::string crlf = "foo\r\nbar\r\n";
  const auto crlf_count = ReplaceRegexMatchesInText(crlf, tail, "X");
  Expect(crlf_count.has_value() && *crlf_count == 1 && crlf == "foX\r\nbar\r\n",
         "regex replace should anchor per-line and preserve CRLF terminators");

  const microide::util::CompiledRegex none("zzz", 0);
  std::string untouched = "alpha\nbeta";
  const auto zero = ReplaceRegexMatchesInText(untouched, none, "X");
  Expect(zero.has_value() && *zero == 0 && untouched == "alpha\nbeta",
         "a no-match regex replace leaves content untouched with a count of 0");

  const microide::util::CompiledRegex any("a", 0);
  std::string bad = "aaa";
  Expect(!ReplaceRegexMatchesInText(bad, any, "\\q").has_value(),
         "an invalid replacement escape should surface as nullopt");
  Expect(bad == "aaa", "a failed regex replace must not partially rewrite content");

  const microide::util::CompiledRegex caseless("foo", PCRE2_CASELESS);
  std::string mixed = "FOO foo Foo";
  const auto mixed_count = ReplaceRegexMatchesInText(mixed, caseless, "x");
  Expect(mixed_count.has_value() && *mixed_count == 3 && mixed == "x x x",
         "a caseless regex replace folds every case variant");

  const microide::util::CompiledRegex bol("^", 0);
  std::string anchored = "a\nb\n";
  const auto bol_count = ReplaceRegexMatchesInText(anchored, bol, ">");
  Expect(bol_count.has_value() && *bol_count == 2 && anchored == ">a\n>b\n",
         "a line-anchored empty match fires per real line, not on a trailing phantom line");
}

void TestWorkspaceSharedRegexSearchMatches() {
  const microide::editor::TextBuffer buffer(
      std::vector<std::string>{"foo123 bar", "no digits", "9 and 42"});
  const microide::util::CompiledRegex digits("\\d+", 0);
  Expect(digits.valid(), "regex-search fixture should compile");

  const auto matches = FindRegexSearchMatches(buffer, digits);
  Expect(matches.size() == 3, "regex search should find every per-line match");
  Expect(matches[0].start.line == 0 && matches[0].start.column == 3 && matches[0].end.column == 6,
         "first match spans the digits on line 0");
  Expect(matches[1].start.line == 2 && matches[1].start.column == 0 && matches[1].end.column == 1,
         "matches are grouped by ascending line then column");
  Expect(matches[2].start.line == 2 && matches[2].start.column == 6 && matches[2].end.column == 8,
         "the second match on a line resumes past the first");

  const microide::util::CompiledRegex invalid("[unterminated", 0);
  Expect(!invalid.valid() && FindRegexSearchMatches(buffer, invalid).empty(),
         "an invalid regex produces an empty match set");
}

void TestWorkspaceSharedRegexHighlightFragments() {
  const microide::editor::TextBuffer buffer(std::vector<std::string>{"ab", "cd", "ef"});

  const std::vector<microide::editor::SelectionRange> newline_match = {
      {{0, 2}, {1, 0}}};
  const auto frags = SplitRegexMatchHighlightFragments(buffer, newline_match);
  Expect(frags.size() == 2, "a `\\n` match splits into two per-line fragments");
  Expect(frags[0].start.line == 0 && frags[0].start.column == 2 && frags[0].end.line == 0 &&
             frags[0].end.column == 3,
         "the non-final line's fragment ends one past its content (newline marker)");
  Expect(frags[1].start.line == 1 && frags[1].start.column == 0 && frags[1].end.column == 0,
         "the final line's fragment ends at the match end column (no marker)");

  const std::vector<microide::editor::SelectionRange> inline_match = {{{2, 0}, {2, 2}}};
  const auto inline_frags = SplitRegexMatchHighlightFragments(buffer, inline_match);
  Expect(inline_frags.size() == 1 && inline_frags[0].end.column == 2,
         "a single-line match is not extended past its content");
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

  Expect(FindLiteralNeedleInLine("Foo foo FOO", 0, "foo", false) == 0,
         "case-insensitive scan should prefer the earliest offset");
  Expect(FindLiteralNeedleInLine("Foo foo FOO", 1, "foo", false) == 4,
         "case-insensitive scan should skip earlier matches outside the slice");
  Expect(!FindLiteralNeedleInLine("x Foo y", 0, "foo", true).has_value(),
         "case-sensitive scan should reject byte mismatches against the seeded needle text");
  const auto matched = FindLiteralNeedleInLine("Foo foo", 0, "Foo", true);
  Expect(matched.has_value() && *matched == 0,
         "case-sensitive scan should locate exact-case substrings");

  Expect(FindLiteralNeedleInLine("ababa", 0, "aba", true) == 0,
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
        FindNextLiteralMatchAfterSeedWrapOnce(dual, 0, 4, 7, "foo", true);
    expect_pos(forward_then_wrap, 0, 0,
               "after the last occurrence on the line, wrap picks the earliest match");
    const auto forward_only =
        FindNextLiteralMatchAfterSeedWrapOnce(dual, 0, 0, 3, "foo", true);
    expect_pos(forward_only, 0, 4,
               "forward scan prefers the occurrence after the seed span");

    const std::vector<std::string> single{"foo"};
    const auto lone =
        FindNextLiteralMatchAfterSeedWrapOnce(single, 0, 0, 3, "foo", true);
    expect_missing(lone, "only the seed occurrence yields no candidate after a single wrap");

    const std::vector<std::string> above_first{"foo", "foo"};
    const auto earlier_line = FindNextLiteralMatchAfterSeedWrapOnce(
        above_first, 1, 0, 3, "foo", true);
    expect_pos(earlier_line, 0, 0,
               "after the seed line exhausts forward matches, wrap finds an earlier line first");
  }

  {
    const std::vector<std::string> mixed_case_line{"Foo foo"};
    const auto forward_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        mixed_case_line, 0, 0, 3, "Foo", false);
    expect_pos(forward_ci, 0, 4,
               "case-insensitive forward finds the next byte-different match on the same line");

    const std::vector<std::string> wrap_mixed{"foo FOO"};
    const auto wrap_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        wrap_mixed, 0, 4, 7, "FOO", false);
    expect_pos(wrap_ci, 0, 0,
               "case-insensitive wrap reaches the earlier mixed-case match before the seed span");

    const std::vector<std::string> lone_mixed{"Foo"};
    const auto none_ci = FindNextLiteralMatchAfterSeedWrapOnce(
        lone_mixed, 0, 0, 3, "Foo", false);
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

void TestWorkspaceAddCursorMatchRanges() {
  using microide::workspace::CollectAddCursorMatchRanges;
  using microide::editor::TextBuffer;

  {
    const TextBuffer buffer(std::vector<std::string>{"aa aa aa"});
    const auto scan = CollectAddCursorMatchRanges(buffer, 0, 0,
                                                  "aa", true);
    Expect(!scan.truncated, "small match set is not truncated");
    Expect(scan.ranges.size() == 2, "seed occurrence is excluded; two others remain");
    Expect(scan.ranges[0].start.column == 3 && scan.ranges[0].end.column == 5,
           "first collected caret covers the second occurrence");
    Expect(scan.ranges[1].start.column == 6 && scan.ranges[1].end.column == 8,
           "second collected caret covers the third occurrence");
  }

  {
    const TextBuffer buffer(std::vector<std::string>{"Foo foo", "FOO"});
    const auto scan = CollectAddCursorMatchRanges(buffer, 0, 0,
                                                  "foo", false);
    Expect(scan.ranges.size() == 2, "the seed on line 0 col 0 is skipped; two matches remain");
    Expect(scan.ranges[0].start.line == 0 && scan.ranges[0].start.column == 4,
           "the second same-line occurrence is collected");
    Expect(scan.ranges[1].start.line == 1 && scan.ranges[1].start.column == 0,
           "the next-line occurrence is collected");
  }

  {
    const TextBuffer buffer(std::vector<std::string>{"foo FOO"});
    const auto scan = CollectAddCursorMatchRanges(buffer, 0, 0,
                                                  "foo", true);
    Expect(scan.ranges.empty(), "case-sensitive scan skips FOO and the seeded foo");
  }

  {
    const TextBuffer buffer(std::vector<std::string>{"x x x x x x x x"});
    const auto scan = CollectAddCursorMatchRanges(buffer, 0, 0,
                                                  "x", true,
                                                  3);
    Expect(scan.truncated, "exceeding the cap sets the truncation flag");
    Expect(scan.ranges.size() == 3, "no more than max_matches carets are collected");
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

  Expect(same(FindLiteralSearchMatches(buffer, "alpha"), FindLiteralSearchMatches(lines, "alpha")),
         "buffer-scan overload should match the vector overload");

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

  const std::vector<std::string> queries = {"a", "al", "alp", "alph", "alpha", "alphab", "alphabe"};
  for (std::size_t i = 1; i < queries.size(); ++i) {
    const auto previous = FindLiteralSearchMatches(buffer, queries[i - 1]);
    const auto refined = RefineLiteralSearchMatches(buffer, queries[i], previous);
    const auto fresh = FindLiteralSearchMatches(buffer, queries[i]);
    Expect(same(refined, fresh),
           "incremental refine of a prefix match set must equal a fresh full scan");
  }

  Expect(RefineLiteralSearchMatches(buffer, "alpha", {}).empty(),
         "refine over an empty previous set yields nothing");
  Expect(RefineLiteralSearchMatches(buffer, "", FindLiteralSearchMatches(buffer, "a")).empty(),
         "refine with an empty query yields nothing");

  const microide::editor::TextBuffer overlap_buffer(
      std::vector<std::string>{"aaaa", "aaa", "aXaa"});
  const auto overlap_prev = FindLiteralSearchMatches(overlap_buffer, "a");
  const auto overlap_refined = RefineLiteralSearchMatches(overlap_buffer, "aa", overlap_prev);
  const auto overlap_fresh = FindLiteralSearchMatches(overlap_buffer, "aa");
  Expect(same(overlap_refined, overlap_fresh),
         "refine of a self-overlapping needle must de-overlap to equal a fresh scan");
  Expect(!overlap_refined.empty(), "self-overlapping refine should still find matches");

  const microide::editor::TextBuffer border_buffer(
      std::vector<std::string>{"abab abc", "ab", "xabc"});
  const auto border_prev = FindLiteralSearchMatches(border_buffer, "ab");
  Expect(same(RefineLiteralSearchMatches(border_buffer, "abc", border_prev),
              FindLiteralSearchMatches(border_buffer, "abc")),
         "refine from a prefix that cannot self-overlap must equal a fresh scan");
  const microide::editor::TextBuffer skipped_buffer(
      std::vector<std::string>{"aaab", "aab", "baaab"});
  const auto skipped_prev = FindLiteralSearchMatches(skipped_buffer, "aa");
  const auto skipped_refined = RefineLiteralSearchMatches(skipped_buffer, "aab", skipped_prev);
  Expect(same(skipped_refined, FindLiteralSearchMatches(skipped_buffer, "aab")),
         "refine from a self-overlapping prefix must still find hits it de-overlapped away");
  Expect(!skipped_refined.empty(), "the de-overlapped hits are really there");
}

}

void TestWorkspaceSharedRefineMatchesTheColdScan() {
  using microide::editor::SelectionRange;
  using microide::editor::TextBuffer;
  using microide::workspace::BufferSearchOptions;

  const auto same = [](const std::vector<SelectionRange>& a, const std::vector<SelectionRange>& b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].start.line != b[i].start.line || a[i].start.column != b[i].start.column ||
          a[i].end.line != b[i].end.line || a[i].end.column != b[i].end.column) {
        return false;
      }
    }
    return true;
  };
  const std::vector<std::string> alphabet = {"a", "A", "b", "B", "\xc3\xa9", "\xc3\x89", " ", "_"};
  std::mt19937 rng(20260906);
  std::size_t checked = 0;
  for (int round = 0; round < 3000; ++round) {
    std::vector<std::string> lines(1 + rng() % 4);
    for (std::string& line : lines) {
      const std::size_t length = rng() % 12;
      for (std::size_t i = 0; i < length; ++i) {
        line += alphabet[rng() % alphabet.size()];
      }
    }
    const TextBuffer buffer(lines);
    std::string query;
    const std::size_t query_length = 1 + rng() % 4;
    for (std::size_t i = 0; i < query_length; ++i) {
      query += alphabet[rng() % (alphabet.size() - 2)];
    }
    const BufferSearchOptions options{.case_sensitive = (rng() % 2) == 0, .whole_word = false};
    std::vector<SelectionRange> previous;
    std::string typed;
    std::size_t offset = 0;
    while (offset < query.size()) {
      const std::size_t unit = static_cast<unsigned char>(query[offset]) >= 0x80 ? 2 : 1;
      typed += query.substr(offset, unit);
      offset += unit;
      const std::vector<SelectionRange> cold = FindLiteralSearchMatches(buffer, typed, options);
      if (!previous.empty() || typed.size() == unit) {
      }
      if (typed.size() > unit) {
        Expect(options.case_sensitive || QueryExtendsCaseInsensitive(typed.substr(0, typed.size() - unit), typed),
               "the typed query extends the previous one");
        const std::vector<SelectionRange> refined =
            RefineLiteralSearchMatches(buffer, typed, previous, options);
        if (!same(refined, cold)) {
          std::string dump = "refine != cold scan for query '" + typed + "' (case " +
                             (options.case_sensitive ? "on" : "off") + ") over lines:";
          for (const std::string& line : lines) {
            dump += " [" + line + "]";
          }
          dump += " refined=" + std::to_string(refined.size()) + " cold=" + std::to_string(cold.size());
          Expect(false, dump);
          return;
        }
        ++checked;
      }
      previous = cold;
    }
  }
  Expect(checked > 2000, "the enumeration exercised the refine path");
}

void TestWorkspaceSharedReplaceAllPathsAgree() {
  using microide::editor::TextViewport;
  using microide::workspace::BufferSearchOptions;
  using microide::workspace::ReplaceLiteralMatchesInText;

  const auto join = [](const TextViewport& viewport) {
    std::string out;
    for (std::size_t i = 0; i < viewport.lines().LineCount(); ++i) {
      if (i > 0) {
        out += '\n';
      }
      out += viewport.lines().LineView(i);
    }
    return out;
  };
  const std::vector<std::string> letters = {"a", "A", "b", "\xc3\xa9", "\xc3\x89"};
  const std::vector<std::string> content_units = {"a", "A", "b", "\xc3\xa9", "\xc3\x89", " ", "\n"};
  const std::vector<std::string> replacement_units = {"x", "Y", "", "\n", "a"};
  std::mt19937 rng(20260906);
  std::size_t compared = 0;
  for (int round = 0; round < 2000; ++round) {
    std::string content;
    const std::size_t content_length = rng() % 24;
    for (std::size_t i = 0; i < content_length; ++i) {
      content += content_units[rng() % content_units.size()];
    }
    std::string query;
    const std::size_t query_length = 1 + rng() % 3;
    for (std::size_t i = 0; i < query_length; ++i) {
      query += letters[rng() % letters.size()];
    }
    std::string replacement;
    const std::size_t replacement_length = rng() % 3;
    for (std::size_t i = 0; i < replacement_length; ++i) {
      replacement += replacement_units[rng() % replacement_units.size()];
    }
    for (const bool case_sensitive : {false, true}) {
      std::string blob = content;
      const std::size_t blob_count =
          ReplaceLiteralMatchesInText(blob, query, replacement, case_sensitive);

      TextViewport ranged;
      ranged.LoadContent(content, "/tmp/replace-equivalence.txt");
      const BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = false};
      const auto matches = FindLiteralSearchMatches(ranged.lines(), query, options);
      const std::optional<std::size_t> ranged_count = ranged.ReplaceAllRanges(matches, replacement);
      const std::string context = " for query '" + query + "' -> '" + replacement + "' (case " +
                                  (case_sensitive ? "on" : "off") + ") over '" + content + "'";
      Expect(ranged_count.has_value() && *ranged_count == blob_count,
             "the range replace counts what the blob replace counts" + context);
      Expect(join(ranged) == blob, "the range replace produces the blob replace's text" + context);
      ++compared;

      if (!case_sensitive) {
        TextViewport whole;
        whole.LoadContent(content, "/tmp/replace-equivalence.txt");
        const std::size_t whole_count = whole.ReplaceAll(query, replacement);
        Expect(whole_count == blob_count, "ReplaceAll counts what the blob replace counts" + context);
        Expect(join(whole) == blob, "ReplaceAll produces the blob replace's text" + context);
      }
    }
  }
  Expect(compared == 4000, "every round compared both case modes");
}

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
  AddTest(tests, "WorkspaceTextSearch/LiteralSearchDoesNotOverlap",
          TestWorkspaceSharedLiteralSearchDoesNotOverlap);
  AddTest(tests, "WorkspaceTextSearch/RefineMatchesTheColdScan",
          TestWorkspaceSharedRefineMatchesTheColdScan);
  AddTest(tests, "WorkspaceTextSearch/ReplaceAllPathsAgree",
          TestWorkspaceSharedReplaceAllPathsAgree);
  AddTest(tests, "WorkspaceTextSearch/LiteralSearchUtf8Fold",
          TestWorkspaceSharedLiteralSearchUtf8Fold);
  AddTest(tests, "WorkspaceTextSearch/LiteralReplaceModeHelpers",
          TestWorkspaceSharedLiteralReplaceModeHelpers);
  AddTest(tests, "WorkspaceTextSearch/RegexReplaceInText",
          TestWorkspaceSharedRegexReplaceInText);
  AddTest(tests, "WorkspaceTextSearch/RegexSearchMatches",
          TestWorkspaceSharedRegexSearchMatches);
  AddTest(tests, "WorkspaceTextSearch/RegexHighlightFragments",
          TestWorkspaceSharedRegexHighlightFragments);
  AddTest(tests, "WorkspaceTextSearch/LiteralNeedleScanCaseModeInLine",
          TestWorkspaceLiteralNeedleScanCaseModeInLine);
  AddTest(tests, "WorkspaceTextSearch/NextLiteralMatchAfterSeedWrapOnce",
          TestWorkspaceNextLiteralMatchAfterSeedWrapOnce);
  AddTest(tests, "WorkspaceTextSearch/IncrementalLiteralSearch",
          TestWorkspaceIncrementalLiteralSearch);
  AddTest(tests, "WorkspaceTextSearch/AddCursorMatchRanges",
          TestWorkspaceAddCursorMatchRanges);
  AddTest(tests, "WorkspaceProjectSearchPresentation/LineMapHelpers",
          TestWorkspaceSharedProjectSearchLineMapHelpers);
}

}
