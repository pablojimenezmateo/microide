#include "TestSupport.h"

#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellShared.h"

#include <algorithm>
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
using microide::workspace::SidebarToolRequest;
using microide::workspace::SidebarToolSpec;
using microide::workspace::FindBuiltinSidebarTool;
using microide::workspace::FindWorkspaceActionByCommand;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::FormatCommandCompletionToken;
using microide::workspace::BuiltinSidebarModeMenuItems;
using microide::workspace::BuiltinSidebarToolNames;
using microide::workspace::JoinCommandArguments;
using microide::workspace::ParseCommandLine;
using microide::workspace::ParseBuiltinSidebarToolRequest;
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
using microide::workspace::SerializeLines;
using microide::workspace::SerializeProjectConfig;
using microide::workspace::SerializeProjectSession;
using microide::workspace::SerializeUserConfig;
using microide::workspace::SerializeWorkspaceSession;
using microide::workspace::SplitSyntaxLines;
using microide::workspace::ToLower;
using microide::workspace::Utf8ByteOffsetForCodepointCount;
using microide::workspace::Utf8CodepointCount;
using microide::workspace::WorkspaceCommandNames;
using microide::workspace::WorkspaceDocumentedCommandUsages;
using microide::workspace::WriteTextFileAtomically;
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

void TestWorkspaceSharedSerializeLines() {
  const std::vector<std::string> lines = {"alpha", "beta", "gamma"};
  Expect(SerializeLines(lines, microide::editor::TextViewport::LineEnding::LF) ==
             "alpha\nbeta\ngamma",
         "serialize lines should join LF payloads with line feeds");
  Expect(SerializeLines(lines, microide::editor::TextViewport::LineEnding::CRLF) ==
             "alpha\r\nbeta\r\ngamma",
         "serialize lines should join CRLF payloads with CRLF separators");
  Expect(SerializeLines(lines, microide::editor::TextViewport::LineEnding::CR) ==
             "alpha\rbeta\rgamma",
         "serialize lines should join CR payloads with carriage returns");
  Expect(SerializeLines({}, microide::editor::TextViewport::LineEnding::LF).empty(),
         "serialize lines should keep empty buffers empty");
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

void TestWorkspaceSharedAtomicTextWrite() {
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-workspace-atomic-write-test";
  std::filesystem::remove_all(temp_root);

  const std::filesystem::path nested = temp_root / "state" / "config.txt";
  Expect(WriteTextFileAtomically(nested, "first\n"),
         "atomic text writer should create missing parent directories");
  Expect(ReadFile(nested) == "first\n",
         "atomic text writer should persist the initial payload");

  Expect(WriteTextFileAtomically(nested, "second\n"),
         "atomic text writer should overwrite existing files");
  Expect(ReadFile(nested) == "second\n",
         "atomic text writer should replace prior content");
  Expect(!std::filesystem::exists(nested.string() + ".tmp"),
         "atomic text writer should not leave a sibling temp file behind");

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

void TestWorkspaceCommandRegistry() {
  const WorkspaceShell::ActionSpec* save = FindWorkspaceActionSpec(WorkspaceShell::ActionId::Save);
  Expect(save != nullptr, "command registry should expose built-in save metadata");
  Expect(save->command_name == "save", "save metadata should preserve the command name");

  const WorkspaceShell::ActionSpec* sidebar_toggle =
      FindWorkspaceActionByCommand("sidebar-toggle");
  Expect(sidebar_toggle != nullptr,
         "command registry should resolve actions by command name");
  Expect(sidebar_toggle->id == WorkspaceShell::ActionId::SidebarToggle,
         "command registry should map sidebar-toggle to the toggle action");

  const std::vector<std::string>& command_names = WorkspaceCommandNames();
  Expect(std::find(command_names.begin(), command_names.end(), "project-open") !=
             command_names.end(),
         "command registry should expose project-open completion data");

  Expect(WorkspaceDocumentedCommandUsages() == WorkspaceShell::DocumentedCommandUsages(),
         "workspace shell command docs should delegate to the registry");
}

void TestWorkspaceSidebarRegistry() {
  const SidebarToolSpec* tree = FindBuiltinSidebarTool(WorkspaceShell::SidebarMode::Tree);
  Expect(tree != nullptr, "sidebar registry should expose the tree tool");
  Expect(tree->command_name == "tree" && tree->label == "Project",
         "tree sidebar tool metadata mismatch");

  const SidebarToolSpec* git = FindBuiltinSidebarTool("git");
  Expect(git != nullptr && git->mode == WorkspaceShell::SidebarMode::Git,
         "sidebar registry should resolve the git tool by command name");

  const SidebarToolSpec* problems = FindBuiltinSidebarTool("problems");
  Expect(problems != nullptr && problems->mode == WorkspaceShell::SidebarMode::Problems,
         "sidebar registry should resolve the problems tool by command name");

  const std::vector<std::string>& tool_names = BuiltinSidebarToolNames();
  Expect(tool_names.size() == 4 && tool_names[0] == "git" && tool_names[1] == "problems" &&
             tool_names[3] == "tree",
         "sidebar registry should preserve built-in tool completion names");

  const SidebarToolRequest search_request =
      ParseBuiltinSidebarToolRequest({"search", "lint", "errors"});
  Expect(search_request.tool != nullptr &&
             search_request.tool->mode == WorkspaceShell::SidebarMode::Search,
         "sidebar request parser should resolve the search tool");
  Expect(search_request.query == "lint errors",
         "sidebar request parser should join search queries");

  const SidebarToolRequest tree_request =
      ParseBuiltinSidebarToolRequest({"tree", "plugins"});
  Expect(tree_request.tool != nullptr &&
             tree_request.tool->mode == WorkspaceShell::SidebarMode::Tree,
         "sidebar request parser should resolve the tree tool");
  Expect(tree_request.root == std::filesystem::path("plugins"),
         "sidebar request parser should keep explicit tree roots");

  const auto menu_items = BuiltinSidebarModeMenuItems();
  Expect(menu_items.size() == 4,
         "sidebar registry should expose four built-in sidebar menu items");
  Expect(menu_items[0].label == "Project" && menu_items[0].args[0] == "tree",
         "sidebar mode menu should keep the project item first");
  Expect(menu_items[2].label == "Problems" && menu_items[2].args[0] == "problems",
         "sidebar mode menu should expose the problems item");
  Expect(menu_items[3].label == "Source Control" && menu_items[3].args[0] == "git",
         "sidebar mode menu should keep the source-control item");
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

  Expect(BuildEditorBreadcrumbLabel(root, root / "docs" / "notes.md", false) ==
             "docs/notes.md",
         "editor breadcrumb helper should return the relative editor path");
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
      .dirty_snapshot = false,
      .line_ending = microide::editor::TextViewport::LineEnding::LF,
      .buffer_lines = {},
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
  AddTest(tests, "WorkspaceShared/SerializeLines", TestWorkspaceSharedSerializeLines);
  AddTest(tests, "WorkspaceShared/ReadFileText", TestWorkspaceSharedReadFileText);
  AddTest(tests, "WorkspaceShared/AtomicTextWrite", TestWorkspaceSharedAtomicTextWrite);
  AddTest(tests, "WorkspaceShared/CommandCompletionHelpers",
          TestWorkspaceSharedCommandCompletionHelpers);
  AddTest(tests, "Workspace/CommandRegistry", TestWorkspaceCommandRegistry);
  AddTest(tests, "Workspace/SidebarRegistry", TestWorkspaceSidebarRegistry);
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
