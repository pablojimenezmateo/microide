#include "TestSupport.h"

#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellShared.h"

#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuildCompareBreadcrumbLabel;
using microide::workspace::BuildEditorBreadcrumbLabel;
using microide::workspace::BuildMergeBreadcrumbLabel;
using microide::workspace::BuildWorkspaceTabTextModel;
using microide::workspace::CollapseWhitespace;
using microide::workspace::CommandCompletionCandidate;
using microide::workspace::CommonPrefix;
using microide::workspace::DecodeSessionNodePath;
using microide::workspace::DetectLineEnding;
using microide::workspace::EncodeSessionNodePath;
using microide::workspace::FormatCommandCompletionToken;
using microide::workspace::JoinCommandArguments;
using microide::workspace::ParseCommandLine;
using microide::workspace::ParseProjectConfigText;
using microide::workspace::ParseProjectSessionText;
using microide::workspace::ParseUiScaleValue;
using microide::workspace::ParseUserConfigText;
using microide::workspace::ParseWorkspaceSessionText;
using microide::workspace::PathEqualsOrWithin;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedEditorViewState;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::PersistedSplitNodeState;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::PersistedWorkspaceSessionState;
using microide::workspace::QuoteCommandArg;
using microide::workspace::ReadFileText;
using microide::workspace::RelativePathLabel;
using microide::workspace::RemoveLastUtf8Codepoint;
using microide::workspace::ReplacePathPrefix;
using microide::workspace::SerializeProjectConfig;
using microide::workspace::SerializeProjectSession;
using microide::workspace::SerializeUserConfig;
using microide::workspace::SerializeWorkspaceSession;
using microide::workspace::SplitSyntaxLines;
using microide::workspace::ToLower;
using microide::workspace::Utf8ByteOffsetForCodepointCount;
using microide::workspace::Utf8CodepointCount;
using microide::workspace::WorkspaceShell;

void TestWorkspaceSharedParseCommandLine() {
  const auto parsed = ParseCommandLine(R"(open "two words" path\ with\ spaces 'tail value')");
  Expect(parsed.tokens.size() == 4, "command line should produce four tokens");
  Expect(parsed.tokens[0].text == "open", "first token should be command name");
  Expect(parsed.tokens[1].text == "two words", "double-quoted token should preserve spaces");
  Expect(parsed.tokens[2].text == "path with spaces", "escaped spaces should be unescaped");
  Expect(parsed.tokens[3].text == "tail value", "single-quoted token should preserve spaces");
  Expect(!parsed.trailing_space, "command line should not end with trailing space");
  Expect(!parsed.dangling_escape, "command line should not end with dangling escape");
  Expect(parsed.open_quote == '\0', "command line should not leave an open quote");
}

void TestWorkspaceSharedParseCommandLineIncompleteState() {
  const auto escaped = ParseCommandLine(R"(open trailing\)");
  Expect(escaped.tokens.size() == 2, "dangling escape command should keep prior tokens");
  Expect(escaped.dangling_escape, "dangling escape should be reported");

  const auto quoted = ParseCommandLine(R"(open "unterminated)");
  Expect(quoted.tokens.size() == 2, "unterminated quote command should keep current token");
  Expect(quoted.open_quote == '"', "unterminated quote should be reported");
}

void TestWorkspaceSharedUiScaleParsing() {
  const auto percent = ParseUiScaleValue("125%");
  Expect(percent.has_value() && *percent == 1.25f, "percent ui scale should parse to ratio");

  const auto whole_number_percent = ParseUiScaleValue("150");
  Expect(whole_number_percent.has_value() && *whole_number_percent == 1.5f,
         "whole-number percent ui scale should parse to ratio");

  const auto invalid = ParseUiScaleValue("abc");
  Expect(!invalid.has_value(), "invalid ui scale should fail");
}

void TestWorkspaceSharedSessionEncoding() {
  const std::vector<std::size_t> path = {0, 3, 12};
  const std::string encoded = EncodeSessionNodePath(path);
  Expect(encoded == "0/3/12", "session path encoding mismatch");

  const auto decoded = DecodeSessionNodePath(encoded);
  Expect(decoded.has_value(), "session path decoding should succeed");
  Expect(*decoded == path, "decoded session path should match original");

  const auto root = DecodeSessionNodePath(".");
  Expect(root.has_value() && root->empty(), "root session path should decode to empty path");
}

void TestWorkspaceSharedQuoteAndLineEndings() {
  Expect(QuoteCommandArg("plain-token") == "plain-token", "plain token should not be quoted");
  Expect(QuoteCommandArg("two words") == "'two words'", "spaced token should be quoted");
  Expect(QuoteCommandArg("can't") == "'can'\\''t'", "single quote should be shell escaped");

  Expect(DetectLineEnding("a\r\nb\r\n") == microide::editor::TextViewport::LineEnding::CRLF,
         "crlf ending should be detected");
  Expect(DetectLineEnding("a\nb\n") == microide::editor::TextViewport::LineEnding::LF,
         "lf ending should be detected");
  Expect(DetectLineEnding("a\rb\r") == microide::editor::TextViewport::LineEnding::CR,
         "cr ending should be detected");
}

void TestWorkspaceSharedSplitSyntaxLines() {
  const auto lines = SplitSyntaxLines("alpha\nbeta\ngamma");
  Expect(lines.size() == 3, "split syntax lines should produce three rows");
  Expect(lines[0] == "alpha", "first syntax line mismatch");
  Expect(lines[2] == "gamma", "last syntax line mismatch");
}

void TestWorkspaceSharedReadFileText() {
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-workspace-shared-test";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);
  const std::filesystem::path sample = temp_root / "sample.txt";
  WriteFile(sample, "sample text\n");

  const auto content = ReadFileText(sample);
  Expect(content.has_value(), "read file text should load existing file");
  Expect(*content == "sample text\n", "read file text content mismatch");

  const auto missing = ReadFileText(temp_root / "missing.txt");
  Expect(!missing.has_value(), "read file text should fail for missing file");

  std::filesystem::remove_all(temp_root);
}

void TestWorkspaceSharedCommandCompletionHelpers() {
  const std::vector<CommandCompletionCandidate> candidates = {
      {"compare", true},
      {"compact", true},
      {"company", false},
  };
  Expect(CommonPrefix(candidates) == "compa", "common prefix should keep the shared stem");
  Expect(FormatCommandCompletionToken({"two words", true}) == "'two words' ",
         "formatted completion should quote spaced tokens and append a space");
  Expect(JoinCommandArguments({"cmd", "left", "right value"}, 1) == "left right value",
         "joined command arguments should preserve argument order");
}

void TestWorkspaceSharedPathAndCaseHelpers() {
  const auto root = std::filesystem::path("/tmp/project");
  Expect(RelativePathLabel(root, root / "src/main.cpp") == "src/main.cpp",
         "relative path label should shorten paths inside the project");
  Expect(RelativePathLabel(root, std::filesystem::path("/tmp/other/file.txt")) ==
             "/tmp/other/file.txt",
         "relative path label should preserve external paths");
  Expect(ToLower("MiXeD 123") == "mixed 123", "to-lower helper should normalize ASCII letters");
}

void TestWorkspaceSharedPathMutationHelpers() {
  const auto root = std::filesystem::path("/tmp/project");
  const auto nested = root / "src/main.cpp";
  Expect(PathEqualsOrWithin(root, root), "path helper should match identical roots");
  Expect(PathEqualsOrWithin(nested, root), "path helper should detect nested paths");
  Expect(!PathEqualsOrWithin(std::filesystem::path("/tmp/other/file.cpp"), root),
         "path helper should reject unrelated paths");

  Expect(ReplacePathPrefix(nested, root / "src", root / "lib") ==
             std::filesystem::path("/tmp/project/lib/main.cpp"),
         "path prefix replacement should preserve the relative suffix");
  Expect(ReplacePathPrefix(root / "docs", root / "docs", root / "manual") ==
             std::filesystem::path("/tmp/project/manual"),
         "path prefix replacement should replace exact-prefix paths");
}

void TestWorkspaceSharedUtf8Editing() {
  std::string text = "a😀b";
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should succeed for non-empty text");
  Expect(text == "a😀", "utf8 deletion should remove the last full codepoint");
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should remove a multibyte codepoint");
  Expect(text == "a", "utf8 deletion should preserve earlier codepoints");
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should remove the last ascii codepoint");
  Expect(text.empty(), "utf8 deletion should empty the string");
  Expect(!RemoveLastUtf8Codepoint(&text), "utf8 deletion should fail for empty strings");
}

void TestWorkspaceSharedUtf8Metrics() {
  const std::string text = "a😀β";
  Expect(Utf8CodepointCount(text) == 3,
         "utf8 codepoint count should treat multibyte sequences as one codepoint");
  Expect(Utf8ByteOffsetForCodepointCount(text, 0) == 0,
         "utf8 byte offset should return zero for zero codepoints");
  Expect(Utf8ByteOffsetForCodepointCount(text, 1) == 1,
         "utf8 byte offset should advance by one for ASCII codepoints");
  Expect(Utf8ByteOffsetForCodepointCount(text, 2) == 5,
         "utf8 byte offset should skip the full emoji sequence");
  Expect(Utf8ByteOffsetForCodepointCount(text, 3) == text.size(),
         "utf8 byte offset should clamp at the end of the string");
}

void TestWorkspaceSharedCollapseWhitespace() {
  Expect(CollapseWhitespace("  alpha\t beta\n\ngamma  ") == "alpha beta gamma",
         "collapse-whitespace should normalize runs of whitespace to single spaces");
  Expect(CollapseWhitespace("single") == "single",
         "collapse-whitespace should preserve strings without whitespace runs");
  Expect(CollapseWhitespace(" \t\r\n ") == "",
         "collapse-whitespace should discard all-whitespace input");
}

void TestWorkspaceSharedChromeTextModel() {
  const std::filesystem::path root = "/tmp/project";
  const auto dirty_tab =
      BuildWorkspaceTabTextModel(root, root / "src" / "main.cpp", "fallback", true);
  Expect(dirty_tab.display_title == "*main.cpp",
         "tab text model should prefix dirty tab titles");
  Expect(dirty_tab.tooltip_label == "src/main.cpp",
         "tab text model should shorten tooltips to project-relative paths");

  const auto untitled_tab = BuildWorkspaceTabTextModel(root, {}, "scratch", false);
  Expect(untitled_tab.display_title == "scratch",
         "tab text model should fall back to the provided title");
  Expect(untitled_tab.tooltip_label == "scratch",
         "tab text model should reuse the fallback title for untitled tooltips");

  Expect(BuildEditorBreadcrumbLabel(root, root / "docs" / "notes.md", false, true) ==
             "docs/notes.md  |  large file mode",
         "editor breadcrumb helper should append the large-file marker");
  Expect(BuildCompareBreadcrumbLabel(root, root / "src" / "compare.txt", "HEAD", "Working tree") ==
             "src/compare.txt  |  HEAD -> Working tree",
         "compare breadcrumb helper should include both refs");
  Expect(BuildMergeBreadcrumbLabel(root, root / "src" / "result.txt", "Incoming", "Current") ==
             "src/result.txt  |  Incoming -> Current",
         "merge breadcrumb helper should include both merge sides");
}

void TestWorkspaceSharedPersistenceSerializers() {
  PersistedUserConfigState user_config{.ui_scale = 1.75f};
  PersistedUserConfigState parsed_user_config{.ui_scale = 1.0f};
  Expect(ParseUserConfigText(SerializeUserConfig(user_config), &parsed_user_config),
         "user-config serializer should round-trip");
  Expect(parsed_user_config.ui_scale == 1.75f,
         "user-config serializer should preserve ui scale");

  PersistedProjectConfigState project_config{
      .editor_tab_size = 8,
      .editor_indent_width = 2,
      .editor_soft_tabs = true,
      .colorscheme_name = "sunny-day custom",
      .project_base_color = SDL_Color{0x12, 0x34, 0x56, 0xff},
  };
  PersistedProjectConfigState parsed_project_config;
  Expect(ParseProjectConfigText(SerializeProjectConfig(project_config), &parsed_project_config),
         "project-config serializer should round-trip");
  Expect(parsed_project_config.editor_tab_size == 8,
         "project-config serializer should preserve tab size");
  Expect(parsed_project_config.editor_indent_width == 2,
         "project-config serializer should preserve indent width");
  Expect(parsed_project_config.editor_soft_tabs,
         "project-config serializer should preserve soft-tab mode");
  Expect(parsed_project_config.colorscheme_name == "sunny-day custom",
         "project-config serializer should preserve quoted colorscheme names");
  Expect(parsed_project_config.project_base_color.has_value() &&
             parsed_project_config.project_base_color->r == 0x12 &&
             parsed_project_config.project_base_color->g == 0x34 &&
             parsed_project_config.project_base_color->b == 0x56,
         "project-config serializer should preserve project base colors");

  PersistedEditorTabState compare_tab;
  compare_tab.kind = "compare";
  compare_tab.compare_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_left_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_right_path = "/tmp/project/src/compare.txt";
  compare_tab.compare_commit_hash = "abcdef123456";
  compare_tab.compare_commit_short_hash = "abcdef1";
  compare_tab.compare_right_ref = "WORKTREE";
  compare_tab.compare_right_label = "Working tree";
  compare_tab.compare_selected_row = 3;
  compare_tab.compare_scroll_row = 4;
  compare_tab.compare_horizontal_scroll = 5;

  PersistedEditorTabState merge_tab;
  merge_tab.kind = "merge";
  merge_tab.merge_base_path = "/tmp/project/base.txt";
  merge_tab.merge_incoming_path = "/tmp/project/incoming.txt";
  merge_tab.merge_current_path = "/tmp/project/current.txt";
  merge_tab.merge_output_path = "/tmp/project/result.txt";
  merge_tab.merge_selected_hunk = 1;
  merge_tab.merge_scroll_row = 6;
  merge_tab.merge_horizontal_scroll = 7;
  merge_tab.merge_left_divider_fraction = 0.25f;
  merge_tab.merge_right_divider_fraction = 0.75f;
  merge_tab.merge_hunk_choices = {"base", "incoming", "both"};

  PersistedEditorTabState editor_tab;
  editor_tab.kind = "editor";
  editor_tab.active_leaf_id = 9;
  editor_tab.views.push_back(PersistedEditorViewState{
      .leaf_id = 9,
      .path = "/tmp/project/src/main.cpp",
      .cursor_line = 12,
      .cursor_column = 4,
      .scroll_line = 8,
      .horizontal_scroll = 2,
  });
  editor_tab.split_nodes.push_back(PersistedSplitNodeState{
      .path = {},
      .orientation = "leaf",
      .size_fraction = 1.0f,
      .leaf_id = 9,
  });

  PersistedProjectSessionState project_session;
  project_session.sidebar_visible = false;
  project_session.sidebar_width = 320.0f;
  project_session.bottom_panel_height = 208.0f;
  project_session.active_tab_index = 2;
  project_session.tabs = {compare_tab, merge_tab, editor_tab};
  PersistedProjectSessionState parsed_project_session;
  Expect(ParseProjectSessionText(SerializeProjectSession(project_session), &parsed_project_session),
         "project-session serializer should round-trip");
  Expect(!parsed_project_session.sidebar_visible,
         "project-session serializer should preserve sidebar visibility");
  Expect(std::fabs(parsed_project_session.sidebar_width - 320.0f) < 0.001f,
         "project-session serializer should preserve sidebar width");
  Expect(std::fabs(parsed_project_session.bottom_panel_height - 208.0f) < 0.001f,
         "project-session serializer should preserve bottom-panel height");
  Expect(parsed_project_session.active_tab_index == 2,
         "project-session serializer should preserve the active tab index");
  Expect(parsed_project_session.tabs.size() == 3,
         "project-session serializer should preserve every tab kind");
  Expect(parsed_project_session.tabs[0].compare_right_label == "Working tree" &&
             parsed_project_session.tabs[0].compare_horizontal_scroll == 5,
         "project-session serializer should preserve compare-tab metadata");
  Expect(parsed_project_session.tabs[1].merge_hunk_choices.size() == 3 &&
             parsed_project_session.tabs[1].merge_hunk_choices[2] == "both",
         "project-session serializer should preserve merge choices");
  Expect(parsed_project_session.tabs[2].views.size() == 1 &&
             parsed_project_session.tabs[2].split_nodes.size() == 1,
         "project-session serializer should preserve editor views and split nodes");

  PersistedWorkspaceSessionState workspace_session{
      .project_roots = {"/tmp/project-a", "/tmp/project b"},
      .active_project_index = 1,
  };
  PersistedWorkspaceSessionState parsed_workspace_session;
  Expect(ParseWorkspaceSessionText(SerializeWorkspaceSession(workspace_session),
                                   &parsed_workspace_session),
         "workspace-session serializer should round-trip");
  Expect(parsed_workspace_session.project_roots.size() == 2,
         "workspace-session serializer should preserve every project root");
  Expect(parsed_workspace_session.project_roots[1] == "/tmp/project b",
         "workspace-session serializer should preserve quoted project roots");
  Expect(parsed_workspace_session.active_project_index == 1,
         "workspace-session serializer should preserve the active project index");
}

void TestWorkspaceReadmeCommandDocsStayInSync() {
  const std::string readme = ReadFile(TestRoot().parent_path() / "README.md");
  const std::string start_marker = "Current commands:\n";
  const std::string end_marker = "\nMerge example:\n";
  const std::size_t start = readme.find(start_marker);
  const std::size_t end = readme.find(end_marker, start == std::string::npos ? 0 : start);
  Expect(start != std::string::npos && end != std::string::npos && end > start,
         "README should keep a bounded current-commands section");

  std::vector<std::string> documented_commands;
  std::istringstream stream(
      readme.substr(start + start_marker.size(), end - (start + start_marker.size())));
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.starts_with("- `")) {
      continue;
    }
    const std::size_t first_tick = line.find('`');
    const std::size_t last_tick = line.rfind('`');
    if (first_tick == std::string::npos || last_tick == std::string::npos || last_tick <= first_tick) {
      continue;
    }
    documented_commands.push_back(line.substr(first_tick + 1, last_tick - first_tick - 1));
  }

  Expect(documented_commands == WorkspaceShell::DocumentedCommandUsages(),
         "README command docs should stay aligned with the action table");
}

}  // namespace

void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/ParseCommandLine", TestWorkspaceSharedParseCommandLine);
  AddTest(tests, "WorkspaceShared/ParseCommandLineIncompleteState",
          TestWorkspaceSharedParseCommandLineIncompleteState);
  AddTest(tests, "WorkspaceShared/UiScaleParsing", TestWorkspaceSharedUiScaleParsing);
  AddTest(tests, "WorkspaceShared/SessionEncoding", TestWorkspaceSharedSessionEncoding);
  AddTest(tests, "WorkspaceShared/QuoteAndLineEndings", TestWorkspaceSharedQuoteAndLineEndings);
  AddTest(tests, "WorkspaceShared/SplitSyntaxLines", TestWorkspaceSharedSplitSyntaxLines);
  AddTest(tests, "WorkspaceShared/ReadFileText", TestWorkspaceSharedReadFileText);
  AddTest(tests, "WorkspaceShared/CommandCompletionHelpers",
          TestWorkspaceSharedCommandCompletionHelpers);
  AddTest(tests, "WorkspaceShared/PathAndCaseHelpers", TestWorkspaceSharedPathAndCaseHelpers);
  AddTest(tests, "WorkspaceShared/PathMutationHelpers", TestWorkspaceSharedPathMutationHelpers);
  AddTest(tests, "WorkspaceShared/Utf8Editing", TestWorkspaceSharedUtf8Editing);
  AddTest(tests, "WorkspaceShared/Utf8Metrics", TestWorkspaceSharedUtf8Metrics);
  AddTest(tests, "WorkspaceShared/CollapseWhitespace", TestWorkspaceSharedCollapseWhitespace);
  AddTest(tests, "WorkspaceShared/ChromeTextModel", TestWorkspaceSharedChromeTextModel);
  AddTest(tests, "WorkspaceShared/PersistenceSerializers",
          TestWorkspaceSharedPersistenceSerializers);
  AddTest(tests, "Workspace/ReadmeCommandDocsStayInSync",
          TestWorkspaceReadmeCommandDocsStayInSync);
}

}  // namespace microide::tests
