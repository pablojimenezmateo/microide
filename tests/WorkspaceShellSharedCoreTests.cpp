#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::ActionSpec;
using microide::workspace::BuildCompareBreadcrumbLabel;
using microide::workspace::BuildEditorBreadcrumbLabel;
using microide::workspace::BuildMergeBreadcrumbLabel;
using microide::workspace::BuildWorkspaceTabTextModel;
using microide::workspace::CollapseWhitespace;
using microide::workspace::CommandCompletionCandidate;
using microide::workspace::CommonPrefix;
using microide::workspace::FindBuiltinSidebarView;
using microide::workspace::FindSidebarView;
using microide::workspace::FindWorkspaceActionByCommand;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::FindWorkspaceMenuSpec;
using microide::workspace::FormatCommandCompletionToken;
using microide::workspace::JoinCommandArguments;
using microide::workspace::IsLspDrivenMenuAction;
using microide::workspace::IsLspMenuActionReady;
using microide::workspace::LspDrivenMenuActionLabel;
using microide::workspace::MenuId;
using microide::workspace::MenuSpec;
using microide::workspace::ParseCommandLine;
using microide::workspace::ParseSidebarViewRequest;
using microide::workspace::ParseUiScaleValue;
using microide::workspace::PathEqualsOrWithin;
using microide::workspace::PersistedEditorTabState;
using microide::workspace::PersistedEditorViewState;
using microide::workspace::PersistedMessageState;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedConversationState;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::PersistedSplitNodeState;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::PersistedWorkspaceSessionState;
using microide::workspace::QuoteCommandArg;
using microide::workspace::RelativePathLabel;
using microide::workspace::ReplacePathPrefix;
using microide::workspace::SidebarViewIds;
using microide::workspace::SidebarViewRequest;
using microide::workspace::SidebarViewSpec;
using microide::workspace::SidebarViews;
using microide::workspace::SplitSyntaxLines;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::ToLower;
using microide::workspace::WorkspaceCommandNames;
using microide::workspace::WorkspaceMenuSpecs;
using microide::workspace::WorkspaceDocumentedCommandUsages;
using microide::workspace::WorkspaceOutputChannels;
using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceTreeContextMenuItems;
using microide::util::DetectLineEnding;
using microide::util::RemoveLastUtf8Codepoint;
using microide::util::ReadTextFile;
using microide::util::SerializeLines;
using microide::util::Utf8ByteOffsetForCodepointCount;
using microide::util::Utf8CodepointCount;
using microide::util::WriteTextFileAtomically;

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

  const auto content = ReadTextFile(sample);
  Expect(content.has_value(), "read file text should load existing file");
  Expect(*content == "sample text\n", "read file text content mismatch");

  const auto missing = ReadTextFile(temp_root / "missing.txt");
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

void TestWorkspaceOutputChannelsParseAndCacheContextSnippets() {
  WorkspaceOutputChannels channels;
  channels.AppendLine("build", "Build", "src/main.cpp:12:3");
  channels.AppendLine("build", "Build", " > 12 | int value = 42;");

  const auto* parsed = channels.ParsedEntryAt("build", 1);
  Expect(parsed != nullptr, "output channels should retain parsed metadata for appended rows");
  Expect(parsed->kind == WorkspaceOutputChannels::ParsedEntry::Kind::ContextSnippet,
         "context snippet rows should be identified during append");
  Expect(parsed->prefix == " > 12 | ",
         "context snippet parsing should preserve the gutter prefix");
  Expect(parsed->code == "int value = 42;",
         "context snippet parsing should preserve the code payload");

  const auto* highlighted = channels.HighlightedContextSnippet("build", 1, "/tmp/main.cpp");
  Expect(highlighted != nullptr, "context snippets should lazily build syntax-highlight caches");
  Expect(highlighted->tokens.size() == parsed->code.size(),
         "highlighted context snippets should keep one token per byte");

  channels.Clear("build");
  Expect(channels.ParsedEntryAt("build", 0) == nullptr,
         "clearing a channel should drop cached parsed snippet metadata");
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
  const ActionSpec* save = FindWorkspaceActionSpec(ActionId::Save);
  Expect(save != nullptr, "command registry should expose built-in save metadata");
  Expect(save->command_name == "save", "save metadata should preserve the command name");

  const ActionSpec* sidebar_toggle = FindWorkspaceActionByCommand("sidebar-toggle");
  Expect(sidebar_toggle != nullptr, "command registry should resolve actions by command name");
  Expect(sidebar_toggle->id == ActionId::SidebarToggle,
         "command registry should map sidebar-toggle to the toggle action");

  const std::vector<std::string>& command_names = WorkspaceCommandNames();
  Expect(std::find(command_names.begin(), command_names.end(), "project-open") !=
             command_names.end(),
         "command registry should expose project-open completion data");

  Expect(WorkspaceDocumentedCommandUsages() == WorkspaceShell::DocumentedCommandUsages(),
         "workspace shell command docs should delegate to the registry");
}

void TestWorkspaceMenuRegistry() {
  const MenuSpec* view = FindWorkspaceMenuSpec(MenuId::View);
  Expect(view != nullptr, "menu registry should expose the view menu");
  Expect(view->label == "View", "view menu should preserve its label");
  Expect(std::find_if(view->items.begin(), view->items.end(),
                      [](const auto& item) {
                        return item.action == ActionId::Wrap && item.checkable &&
                               item.label == "Word Wrap";
                      }) != view->items.end(),
         "view menu should expose a checkable word-wrap toggle");

  const auto menus = WorkspaceMenuSpecs();
  Expect(std::find_if(menus.begin(), menus.end(),
                      [](const MenuSpec& spec) { return spec.id == MenuId::SidebarMode; }) !=
             menus.end(),
         "menu registry should keep the sidebar-mode menu");

  const auto root_items = WorkspaceTreeContextMenuItems(TreeContextTargetKind::Root);
  Expect(std::find_if(root_items.begin(), root_items.end(),
                      [](const auto& item) { return item.action == ActionId::ProjectClose; }) !=
             root_items.end(),
         "tree-context registry should keep the root close-project action");

  const auto file_items = WorkspaceTreeContextMenuItems(TreeContextTargetKind::File);
  Expect(std::find_if(file_items.begin(), file_items.end(),
                      [](const auto& item) {
                        return item.action == ActionId::CompareHead ||
                               item.action == ActionId::DeletePath ||
                               item.action == ActionId::CopyAbsolutePath;
                      }) != file_items.end(),
         "tree-context registry should expose file actions");
}

void TestWorkspaceMenuRegistryLspAvailabilityLabels() {
  using Snapshot = microide::workspace::LspClient::ReadinessSnapshot;
  using State = Snapshot::State;

  const Snapshot starting{
      .state = State::Starting,
      .message = "Starting...",
      .indexed_count = 0,
  };
  const Snapshot indexing{
      .state = State::Indexing,
      .message = "Indexing workspace",
      .indexed_count = 42,
  };
  const Snapshot ready{
      .state = State::Ready,
      .message = "Ready",
      .indexed_count = 0,
  };
  const Snapshot failed{
      .state = State::Failed,
      .message = "Startup failed",
      .indexed_count = 0,
  };

  Expect(IsLspDrivenMenuAction(ActionId::GoToDefinition) &&
             IsLspDrivenMenuAction(ActionId::FindReferences) &&
             !IsLspDrivenMenuAction(ActionId::Completion),
         "menu registry should only treat definition and references as LSP-gated actions");
  Expect(!IsLspMenuActionReady(starting) && !IsLspMenuActionReady(indexing) &&
             IsLspMenuActionReady(ready) && !IsLspMenuActionReady(failed),
         "menu registry should only enable LSP-driven actions when the client is ready");
  Expect(LspDrivenMenuActionLabel(ActionId::GoToDefinition, "Go to Definition", starting) ==
             "Go to Definition (LSP starting...)",
         "menu registry should explain starting LSP state in disabled labels");
  Expect(LspDrivenMenuActionLabel(ActionId::FindReferences, "Find References", indexing) ==
             "Find References (LSP indexing 42...)",
         "menu registry should explain indexing LSP state in disabled labels");
  Expect(LspDrivenMenuActionLabel(ActionId::GoToDefinition, "Go to Definition", ready) ==
             "Go to Definition",
         "menu registry should keep the ready label unchanged");
  Expect(LspDrivenMenuActionLabel(ActionId::FindReferences, "Find References", failed) ==
             "Find References (LSP failed)",
         "menu registry should explain failed LSP state in disabled labels");
}

void TestWorkspaceSidebarRegistry() {
  const SidebarViewSpec* tree = FindBuiltinSidebarView(WorkspaceShell::SidebarMode::Tree);
  Expect(tree != nullptr, "sidebar registry should expose the tree tool");
  Expect(tree->id == "tree" && tree->label == "Project",
         "tree sidebar tool metadata mismatch");

  const SidebarViewSpec* git = FindBuiltinSidebarView("git");
  Expect(git != nullptr && git->mode == WorkspaceShell::SidebarMode::Git,
         "sidebar registry should resolve the git tool by command name");

  const SidebarViewSpec* problems = FindBuiltinSidebarView("problems");
  Expect(problems != nullptr && problems->mode == WorkspaceShell::SidebarMode::Problems,
         "sidebar registry should resolve the problems tool by command name");

  microide::plugin::PluginHost plugin_host;
  const auto views = SidebarViews(plugin_host);
  Expect(views.size() == 5, "sidebar registry should expose five built-in views");
  Expect(views[0].id == "tree" && views[0].label == "Project" &&
             views[1].id == "search" && views[2].id == "problems" &&
             views[3].id == "git" && views[4].id == "tests",
         "sidebar registry should preserve built-in view ordering");

  const auto problems_view = FindSidebarView("problems", plugin_host);
  Expect(problems_view.has_value() &&
             problems_view->mode == WorkspaceShell::SidebarMode::Problems,
         "sidebar registry should resolve the problems view against the host");

  const auto view_ids = SidebarViewIds(plugin_host);
  // Alphabetical order: git, problems, search, tests, tree
  Expect(view_ids.size() == 5 && view_ids[0] == "git" && view_ids[1] == "problems" &&
             view_ids[3] == "tests" && view_ids[4] == "tree",
         "sidebar registry should preserve built-in view completion ids");

  const SidebarViewRequest search_request =
      ParseSidebarViewRequest({"search", "lint", "errors"}, plugin_host);
  Expect(search_request.view.has_value() &&
             search_request.view->mode == WorkspaceShell::SidebarMode::Search,
         "sidebar request parser should resolve the search tool");
  Expect(search_request.query == "lint errors",
         "sidebar request parser should join search queries");

  const SidebarViewRequest tree_request =
      ParseSidebarViewRequest({"tree", "plugins"}, plugin_host);
  Expect(tree_request.view.has_value() &&
             tree_request.view->mode == WorkspaceShell::SidebarMode::Tree,
         "sidebar request parser should resolve the tree tool");
  Expect(tree_request.root == std::filesystem::path("plugins"),
         "sidebar request parser should keep explicit tree roots");
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
  AddTest(tests, "WorkspaceShared/QuoteAndLineEndings", TestWorkspaceSharedQuoteAndLineEndings);
  AddTest(tests, "WorkspaceShared/SplitSyntaxLines", TestWorkspaceSharedSplitSyntaxLines);
  AddTest(tests, "WorkspaceShared/SerializeLines", TestWorkspaceSharedSerializeLines);
  AddTest(tests, "WorkspaceShared/ReadFileText", TestWorkspaceSharedReadFileText);
  AddTest(tests, "WorkspaceShared/AtomicTextWrite", TestWorkspaceSharedAtomicTextWrite);
  AddTest(tests, "WorkspaceShared/OutputChannelsParseAndCacheContextSnippets",
          TestWorkspaceOutputChannelsParseAndCacheContextSnippets);
  AddTest(tests, "WorkspaceShared/CommandCompletionHelpers",
          TestWorkspaceSharedCommandCompletionHelpers);
  AddTest(tests, "Workspace/CommandRegistry", TestWorkspaceCommandRegistry);
  AddTest(tests, "Workspace/MenuRegistry", TestWorkspaceMenuRegistry);
  AddTest(tests, "Workspace/MenuRegistryLspAvailabilityLabels",
          TestWorkspaceMenuRegistryLspAvailabilityLabels);
  AddTest(tests, "Workspace/SidebarRegistry", TestWorkspaceSidebarRegistry);
  AddTest(tests, "WorkspaceShared/PathAndCaseHelpers", TestWorkspaceSharedPathAndCaseHelpers);
  AddTest(tests, "WorkspaceShared/PathMutationHelpers", TestWorkspaceSharedPathMutationHelpers);
  AddTest(tests, "WorkspaceShared/Utf8Editing", TestWorkspaceSharedUtf8Editing);
  AddTest(tests, "WorkspaceShared/Utf8Metrics", TestWorkspaceSharedUtf8Metrics);
  AddTest(tests, "WorkspaceShared/CollapseWhitespace", TestWorkspaceSharedCollapseWhitespace);
  AddTest(tests, "WorkspaceShared/ChromeTextModel", TestWorkspaceSharedChromeTextModel);
  AddTest(tests, "Workspace/ReadmeCommandDocsStayInSync",
          TestWorkspaceReadmeCommandDocsStayInSync);
}

}  // namespace microide::tests
