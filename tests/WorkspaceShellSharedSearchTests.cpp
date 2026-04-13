#include "TestSupport.h"

#include "workspace/WorkspaceShellShared.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuildGitSidebarLineSpecs;
using microide::workspace::BuildGitSidebarEntryTextModel;
using microide::workspace::BuildProjectSearchResultLineMap;
using microide::workspace::FindLiteralSearchMatches;
using microide::workspace::FindProjectSearchResultLine;
using microide::workspace::FindSelectedGitSidebarLineIndex;
using microide::workspace::GitSidebarLineKind;
using microide::workspace::GitSidebarSection;
using microide::workspace::QuerySupportsLiteralReplace;
using microide::workspace::ReplaceLiteralMatchesInText;
using microide::workspace::UsesCaseSensitiveLiteralMatch;

void TestWorkspaceSharedGitSidebarLineHelpers() {
  const std::vector<GitSidebarSection> sections = {
      GitSidebarSection::Modified,
      GitSidebarSection::Modified,
      GitSidebarSection::Outgoing,
  };
  const auto lines = BuildGitSidebarLineSpecs(sections, true, "origin/main", "origin/main");
  Expect(lines.size() == 5, "git sidebar lines should include both section headers and entries");
  Expect(lines[0].kind == GitSidebarLineKind::Header && lines[0].label == "Changes (2)",
         "git sidebar lines should include the modified header with count");
  Expect(lines[1].kind == GitSidebarLineKind::Entry && lines[1].entry_index == 0,
         "git sidebar lines should map the first modified entry index");
  Expect(lines[2].kind == GitSidebarLineKind::Entry && lines[2].entry_index == 1,
         "git sidebar lines should map the second modified entry index");
  Expect(lines[3].kind == GitSidebarLineKind::Header &&
             lines[3].label == "Outgoing files (1)  origin/main",
         "git sidebar lines should include the outgoing header with base label");
  Expect(lines[4].kind == GitSidebarLineKind::Entry && lines[4].entry_index == 2,
         "git sidebar lines should map outgoing entries after the outgoing header");

  const auto selected_line = FindSelectedGitSidebarLineIndex(lines, 2);
  Expect(selected_line.has_value() && *selected_line == 4,
         "git sidebar selected-line lookup should find the outgoing entry row");
  Expect(!FindSelectedGitSidebarLineIndex(lines, 9).has_value(),
         "git sidebar selected-line lookup should fail for unknown entries");
}

void TestWorkspaceSharedGitSidebarEmptyStates() {
  const auto clean_lines = BuildGitSidebarLineSpecs({}, true, "", "");
  Expect(clean_lines.size() == 4,
         "empty git sidebar should still show both section headers and empty rows");
  Expect(clean_lines[1].kind == GitSidebarLineKind::Empty &&
             clean_lines[1].label == "Working tree is clean",
         "git sidebar should describe a clean working tree when git is available");
  Expect(clean_lines[3].kind == GitSidebarLineKind::Empty &&
             clean_lines[3].label == "Base branch unavailable",
         "git sidebar should describe a missing base branch for outgoing files");

  const auto no_repo_lines = BuildGitSidebarLineSpecs({}, false, "", "");
  Expect(no_repo_lines[1].label == "Not a git repository",
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

void TestWorkspaceSharedProjectSearchLineMapHelpers() {
  const std::vector<microide::project::ProjectSearchResult> results = {
      {.relative_path = std::filesystem::path("src/a.cpp"), .line = 0, .column = 1, .preview = "first"},
      {.relative_path = std::filesystem::path("src/a.cpp"), .line = 3, .column = 2, .preview = "second"},
      {.relative_path = std::filesystem::path("src/b.cpp"), .line = 1, .column = 0, .preview = "third"},
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

}  // namespace

void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/GitSidebarLineHelpers",
          TestWorkspaceSharedGitSidebarLineHelpers);
  AddTest(tests, "WorkspaceShared/GitSidebarEmptyStates",
          TestWorkspaceSharedGitSidebarEmptyStates);
  AddTest(tests, "WorkspaceShared/GitSidebarEntryTextModel",
          TestWorkspaceSharedGitSidebarEntryTextModel);
  AddTest(tests, "WorkspaceShared/LiteralSearchHelpers",
          TestWorkspaceSharedLiteralSearchHelpers);
  AddTest(tests, "WorkspaceShared/LiteralReplaceModeHelpers",
          TestWorkspaceSharedLiteralReplaceModeHelpers);
  AddTest(tests, "WorkspaceShared/ProjectSearchLineMapHelpers",
          TestWorkspaceSharedProjectSearchLineMapHelpers);
}

}  // namespace microide::tests
