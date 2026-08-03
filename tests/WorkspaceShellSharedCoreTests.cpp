#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "editor/SingleLineEditor.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "util/PathMatch.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceOutputReference.h"
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
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedProjectSessionState;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::PersistedWorkspaceSessionState;
using microide::workspace::QuoteCommandArg;
using microide::util::RelativePathWithin;
using microide::util::ReplacePathPrefix;
using microide::workspace::RelativePathLabel;
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

// Regression: ParseCommandLine bounds a pasted-huge command — the scanned length
// and the token count are capped so it cannot allocate/drive completion without
// limit on the UI thread.
void TestWorkspaceSharedParseCommandLineBoundsHugeInput() {
  // Far more space-separated tokens than the 4096 cap.
  std::string many;
  for (int i = 0; i < 10000; ++i) {
    many += "t ";
  }
  const auto parsed = ParseCommandLine(many);
  Expect(parsed.tokens.size() <= 4096, "token count is capped");

  // A megabyte single token is clamped to the scanned-length ceiling.
  const auto huge = ParseCommandLine(std::string(1024 * 1024, 'x'));
  Expect(huge.tokens.size() == 1 && huge.tokens.front().text.size() <= 64 * 1024,
         "an over-long token is truncated to the scanned-length cap");
}

// Regression: CompletePath bounds candidate collection so completing a directory
// with a huge number of entries cannot build and sort an unbounded list on the UI
// thread.
void TestWorkspaceSharedCompletePathIsBounded() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "many";
  std::filesystem::create_directories(root);
  for (int i = 0; i < 2100; ++i) {
    WriteFile(root / ("f" + std::to_string(i) + ".txt"), "x");
  }
  const auto candidates = microide::workspace::CompletePath(root, "", /*directories_only=*/false);
  Expect(candidates.size() <= 2000, "path completion caps the candidate count, got " +
                                        std::to_string(candidates.size()));
  Expect(!candidates.empty(), "path completion still returns candidates up to the cap");
}

// TD-2026-07-16-51: CompletePath must not throw when the search directory vanishes
// during iteration — it advances with the non-throwing increment(ec) and returns the
// candidates gathered so far. We can't force a mid-iteration removal deterministically,
// but a vanished search directory (constructor error) must return empty, not throw.
void TestWorkspaceSharedCompletePathVanishedDirectoryReturnsEmpty() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "gone";
  // Never created: the directory_iterator constructor fails; must return no matches.
  const auto candidates =
      microide::workspace::CompletePath(root, "sub/", /*directories_only=*/false);
  Expect(candidates.empty(),
         "completing under a non-existent directory returns empty, not a thrown exception");
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

  // StatFileSignature should report the durable file's identity and track size.
  const microide::util::FileSignature sig = microide::util::StatFileSignature(nested);
  Expect(sig.exists && !sig.error, "signature of a written file should exist without error");
  Expect(sig.size == std::string("second\n").size(),
         "signature size should match the written payload length");
  const microide::util::FileSignature missing =
      microide::util::StatFileSignature(temp_root / "does-not-exist");
  Expect(!missing.exists && !missing.error,
         "signature of an absent file should report absent, not error");

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

// TD-2026-07-17A-017: ResolvedReferencePath resolves a parsed reference against the
// project root and normalizes it, caching per entry so the output panel render does
// not re-run lexically_normal() for every visible reference row each paint.
void TestWorkspaceOutputChannelsResolvedReferencePathCaches() {
  WorkspaceOutputChannels channels;
  channels.AppendLine("build", "Build", "src/../src/main.cpp:12:3");

  const std::filesystem::path root_a = "/home/proj";
  const std::filesystem::path& resolved =
      channels.ResolvedReferencePath("build", 0, root_a);
  Expect(resolved == std::filesystem::path("/home/proj/src/main.cpp"),
         "a relative reference resolves against the project root and normalizes");
  // Second call with the same root reuses the cached, normalized path object.
  Expect(&channels.ResolvedReferencePath("build", 0, root_a) == &resolved,
         "an unchanged root reuses the cached resolved path");

  // A different root re-resolves.
  const std::filesystem::path& other =
      channels.ResolvedReferencePath("build", 0, "/other/root");
  Expect(other == std::filesystem::path("/other/root/src/main.cpp"),
         "changing the project root re-resolves the reference");

  // A missing entry yields an empty path rather than crashing.
  Expect(channels.ResolvedReferencePath("build", 99, root_a).empty(),
         "an out-of-range index resolves to an empty path");
}

// RemoveChannel drops a channel entirely (Phase 10 debug-console cleanup): the
// channel disappears from Channels()/Entries() while siblings are untouched.
// TD-2026-07-17A-085: appending to an existing channel must not invalidate the
// channel-metadata view. The Channels() list is a cache rebuilt only when the dirty
// flag is set; appending many lines keeps its cached identity, while a real
// label/insert/remove change still refreshes it.
void TestWorkspaceOutputChannelsAppendKeepsMetadataCacheStable() {
  WorkspaceOutputChannels channels;
  channels.AppendLine("build", "Build", "first");
  // Prime the cache.
  const std::vector<WorkspaceOutputChannels::ChannelInfo>& first = channels.Channels();
  Expect(first.size() == 1 && first.front().label == "Build", "one channel with its label");
  const void* cached_data = first.data();

  // Many content appends with the SAME id/label must not rebuild the metadata cache.
  for (int i = 0; i < 50; ++i) {
    channels.AppendLine("build", "Build", "line");
  }
  const std::vector<WorkspaceOutputChannels::ChannelInfo>& after = channels.Channels();
  Expect(after.data() == cached_data,
         "same-id/label appends must not rebuild the channel-metadata cache");
  Expect(after.size() == 1, "still exactly one channel");

  // A genuine label change still refreshes the metadata view.
  channels.AppendLine("build", "Build (2 errors)", "line");
  Expect(channels.Channels().front().label == "Build (2 errors)",
         "a changed label still refreshes the channel-metadata cache");
}

void TestWorkspaceOutputChannelsRemoveChannel() {
  WorkspaceOutputChannels channels;
  channels.AppendLine("debug.console.1", "Session 1", "hello");
  channels.AppendLine("debug.console.2", "Session 2", "world");
  Expect(channels.Channels().size() == 2, "two channels exist before removal");

  channels.RemoveChannel("debug.console.1");
  Expect(channels.Entries("debug.console.1") == nullptr,
         "a removed channel's entries are gone");
  Expect(channels.Channels().size() == 1 &&
             channels.Channels().front().id == "debug.console.2",
         "removal drops only the named channel and refreshes the channel list");

  channels.RemoveChannel("debug.console.1");  // idempotent / unknown id is a no-op
  Expect(channels.Channels().size() == 1, "removing an unknown channel is a no-op");
}

// A chatty or hostile LSP / debug adapter / plugin can stream output forever;
// each entry retains a heap string plus a parsed cache, so the per-channel
// entry vectors must be bounded (drop-oldest) instead of growing without limit.
void TestWorkspaceOutputChannelsCapsEntries() {
  WorkspaceOutputChannels channels;
  constexpr std::size_t kFlood = 200000;
  for (std::size_t i = 0; i < kFlood; ++i) {
    channels.AppendLine("lsp.log", "LSP Log", "line");
  }
  const std::vector<std::string>* entries = channels.Entries("lsp.log");
  Expect(entries != nullptr, "flooded channel still exists");
  // Cap is 100000; the coalesced trim allows up to +25% headroom before trimming.
  Expect(entries->size() <= 125001,
         "output channel entries must be bounded, not grow with a flood");
  // parsed_entries are trimmed in lockstep: the last-appended entry is still
  // readable at the final index.
  Expect(channels.ParsedEntryAt("lsp.log", entries->size() - 1) != nullptr,
         "the newest parsed entry stays indexable after trimming");
}

void TestWorkspaceOutputChannelsCapLargeLineBytes() {
  WorkspaceOutputChannels channels;
  channels.AppendLine("debug.console.1", "Debug", std::string(2u * 1024 * 1024, 'x'));

  const std::vector<std::string>* entries = channels.Entries("debug.console.1");
  Expect(entries != nullptr && entries->size() == 1, "large-line channel should retain one entry");
  Expect(entries->front().size() <= 1024u * 1024u,
         "one huge output line must be truncated before retention");
  Expect(entries->front().find("[truncated]") != std::string::npos,
         "truncated output should carry a visible marker");
}

void TestWorkspaceOutputChannelsCapRetainedBytes() {
  WorkspaceOutputChannels channels;
  constexpr std::size_t kLineBytes = 256u * 1024u;
  for (int i = 0; i < 100; ++i) {
    channels.AppendLine("plugins.log", "Plugin Log", std::string(kLineBytes, 'p'));
  }

  const std::vector<std::string>* entries = channels.Entries("plugins.log");
  Expect(entries != nullptr && !entries->empty(), "byte-capped channel should retain newest rows");
  std::size_t retained = 0;
  for (const std::string& entry : *entries) {
    retained += entry.size();
  }
  Expect(retained <= 20u * 1024u * 1024u,
         "output channel retained bytes must stay near the byte budget");
  Expect(channels.ParsedEntryAt("plugins.log", entries->size() - 1) != nullptr,
         "byte-budget trimming should keep parsed entries in lockstep");
}

// OutputReference documents both line and column as 1-based, so ParseOutputReference
// must reject a zero in EITHER field. The parser previously guarded only line == 0,
// silently accepting a bogus ":<line>:0" column.
void TestWorkspaceOutputReferenceRejectsZeroLineOrColumn() {
  using microide::workspace::ParseOutputReference;

  const auto normal = ParseOutputReference("file.cpp:10:5");
  Expect(normal.has_value(), "a normal 1-based reference should parse");
  Expect(normal->line == 10 && normal->column == 5,
         "parsed reference should carry the 1-based line and column");

  const auto column_one = ParseOutputReference("file.cpp:1:1");
  Expect(column_one.has_value() && column_one->line == 1 && column_one->column == 1,
         "the smallest valid 1-based reference (:1:1) should parse");

  Expect(!ParseOutputReference("file.cpp:1:0").has_value(),
         "a zero column (:1:0) must be rejected as malformed");
  Expect(!ParseOutputReference("file.cpp:0:1").has_value(),
         "a zero line (:0:1) must be rejected as malformed");
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

// Command-line completion pulls plugin command names by reference from the host-owned
// vector (TD-2026-07-17A-012): the operation returns `const std::vector<std::string>&`,
// so opening/completing the command line no longer copies the whole plugin registry. A
// plugin-only command prefix still completes, proving the reference-returned names take
// part in the merged candidate set.
void TestCommandLineCompletionIncludesPluginCommandsByReference() {
  microide::workspace::ProjectWorkspaceState state;
  std::vector<std::string> colorschemes;
  // Stable, host-owned command vector; the operation hands back a reference to it (never
  // a copy). A distinctive name avoids colliding with any built-in command verb.
  const std::vector<std::string> plugin_commands = {"zzsampleplugincommand"};

  microide::workspace::CommandLineCoordinator::Operations ops;
  ops.plugin_command_names = [&plugin_commands]() -> const std::vector<std::string>& {
    return plugin_commands;
  };
  ops.sidebar_view_ids = []() { return std::vector<std::string>{}; };
  microide::workspace::CommandLineCoordinator coordinator(state, colorschemes, std::move(ops));

  microide::editor::SingleLineEditor input;
  input.SetText("zzsamplepl");
  coordinator.CompleteInput(input);
  Expect(input.text() == "zzsampleplugincommand ",
         "a unique plugin-command prefix should complete to the full command name");
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
                        // No label override: the item inherits "Word Wrap" from the
                        // command registry, so the menu and the palette cannot drift.
                        return item.action == ActionId::Wrap && item.checkable &&
                               item.label.empty();
                      }) != view->items.end(),
         "view menu should expose a checkable word-wrap toggle");

  const auto menus = WorkspaceMenuSpecs();
  Expect(std::find_if(menus.begin(), menus.end(),
                      [](const MenuSpec& spec) { return spec.id == MenuId::SidebarMode; }) !=
             menus.end(),
         "menu registry should keep the sidebar-mode menu");

  const auto root_items = WorkspaceTreeContextMenuItems(TreeContextTargetKind::Root);
  Expect(std::find_if(root_items.begin(), root_items.end(),
                      [](const auto& item) { return item.action == ActionId::ProjectClose; }) ==
             root_items.end(),
         "tree-context registry should not expose close-project on the root (project tab owns it)");

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

  // `message` now carries only detail beyond the state's own name (a server progress
  // title, a startup error); the displayed word comes from LspReadinessWord.
  const Snapshot starting{
      .state = State::Starting,
      .message = {},
      .indexed_count = 0,
  };
  const Snapshot indexing{
      .state = State::Indexing,
      .message = "Indexing workspace",
      .indexed_count = 42,
  };
  const Snapshot ready{
      .state = State::Ready,
      .message = {},
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
  std::string scratch;
  Expect(LspDrivenMenuActionLabel(ActionId::GoToDefinition, "Go to Definition", starting,
                                  scratch) == "Go to Definition (LSP: Starting…)",
         "menu registry should explain starting LSP state in disabled labels");
  Expect(LspDrivenMenuActionLabel(ActionId::FindReferences, "Find References", indexing,
                                  scratch) == "Find References (LSP: Indexing 42…)",
         "menu registry should explain indexing LSP state in disabled labels");
  Expect(LspDrivenMenuActionLabel(ActionId::GoToDefinition, "Go to Definition", ready,
                                  scratch) == "Go to Definition",
         "menu registry should keep the ready label unchanged");
  Expect(LspDrivenMenuActionLabel(ActionId::FindReferences, "Find References", failed,
                                  scratch) == "Find References (LSP: Failed)",
         "menu registry should explain failed LSP state in disabled labels");

  // The menu suffix and the status bar must name the state with the same word; they
  // used to keep two vocabularies ("LSP starting..." vs "Starting...").
  std::string word_scratch;
  Expect(microide::workspace::LspReadinessText(indexing, word_scratch) == "Indexing 42…" &&
             microide::workspace::LspReadinessText(starting, word_scratch) == "Starting…" &&
             microide::workspace::LspReadinessText(ready, word_scratch) == "Ready",
         "readiness text should fold the indexed count in and otherwise be the state word");
}

void TestWorkspaceSidebarRegistry() {
  const SidebarViewSpec* tree = FindBuiltinSidebarView(WorkspaceShell::SidebarMode::Tree);
  Expect(tree != nullptr, "sidebar registry should expose the tree tool");
  Expect(tree->id == "tree" && tree->label == "Project",
         "tree sidebar tool metadata mismatch");

  const SidebarViewSpec* git = FindBuiltinSidebarView("git");
  Expect(git != nullptr && git->mode == WorkspaceShell::SidebarMode::Git,
         "sidebar registry should resolve the git tool by command name");

  microide::plugin::PluginHost plugin_host;
  const auto views = SidebarViews(plugin_host);
  Expect(views.size() == 6, "sidebar registry should expose six built-in views");
  Expect(views[0].id == "tree" && views[0].label == "Project" &&
             views[1].id == "search" && views[2].id == "git" && views[3].id == "problems" &&
             views[4].id == "tests" && views[5].id == "outline",
         "sidebar registry should preserve built-in view declaration ordering");

  const auto view_ids = SidebarViewIds(plugin_host);
  // Alphabetical order: git, outline, problems, search, tests, tree
  Expect(view_ids.size() == 6 && view_ids[0] == "git" && view_ids[1] == "outline" &&
             view_ids[2] == "problems" && view_ids[3] == "search" && view_ids[4] == "tests" &&
             view_ids[5] == "tree",
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

  // Lexical semantics (must not touch the filesystem or depend on cwd): empty
  // inputs are never "within", `.`/`..` segments resolve without I/O, and a
  // candidate that escapes the root via `..` is rejected. These guard the
  // consolidation onto util::PathEqualsOrWithin, which replaced per-store copies
  // that used std::filesystem::relative (a cwd-dependent syscall).
  Expect(!PathEqualsOrWithin(std::filesystem::path{}, root),
         "empty candidate should never be within a root");
  Expect(!PathEqualsOrWithin(nested, std::filesystem::path{}),
         "empty root should never contain a candidate");
  Expect(PathEqualsOrWithin(root / "src/./main.cpp", root),
         "unnormalized nested paths should still be detected");
  Expect(!PathEqualsOrWithin(root / "../sibling/file.cpp", root),
         "a candidate escaping the root via .. should be rejected");
  Expect(PathEqualsOrWithin(std::filesystem::path("a/b/c.cpp"), std::filesystem::path("a/b")),
         "relative (non-absolute) paths should resolve lexically without cwd");

  // NormalizedPathEqualsOrWithin is the allocation-free variant used by the
  // per-entry project traversal filter. It must agree with the general helper on
  // every already-normalized input — including the sibling-prefix trap ("/tmp/pro"
  // is a string prefix of "/tmp/project" but not a parent of it) and a "." root,
  // whose members are spelled without any "./" prefix once normalized.
  {
    using microide::util::NormalizedPathEqualsOrWithin;
    const auto same = [](const std::filesystem::path& c, const std::filesystem::path& r) {
      return NormalizedPathEqualsOrWithin(c, r) == PathEqualsOrWithin(c, r);
    };
    Expect(NormalizedPathEqualsOrWithin(nested, root), "normalized variant detects nesting");
    Expect(NormalizedPathEqualsOrWithin(root, root), "normalized variant matches the root");
    Expect(!NormalizedPathEqualsOrWithin(std::filesystem::path("/tmp/projectile/x.cpp"), root),
           "a sibling sharing a string prefix must not count as nested");
    Expect(same(std::filesystem::path("/tmp/projectile/x.cpp"), root), "sibling-prefix agrees");
    Expect(same(nested, root), "nested agrees");
    Expect(same(root, root), "identity agrees");
    Expect(same(std::filesystem::path("/tmp"), root), "parent-of-root agrees");
    Expect(same(std::filesystem::path{}, root), "empty candidate agrees");
    Expect(same(nested, std::filesystem::path{}), "empty root agrees");
    Expect(same(std::filesystem::path("a/b/c.cpp"), std::filesystem::path("a/b")),
           "relative nesting agrees");
    Expect(same(std::filesystem::path("a/b/c.cpp"), std::filesystem::path(".")),
           "a '.' root agrees for a relative member");
    Expect(same(std::filesystem::path("../out/x"), std::filesystem::path(".")),
           "a '.' root agrees for a path climbing out");
    Expect(same(std::filesystem::path("/abs/x"), std::filesystem::path(".")),
           "a '.' root agrees for an absolute path");
    Expect(NormalizedPathEqualsOrWithin(std::filesystem::path("/tmp/x"),
                                        std::filesystem::path("/")),
           "a '/' root contains every absolute path");
  }

  Expect(ReplacePathPrefix(nested, root / "src", root / "lib") ==
             std::filesystem::path("/tmp/project/lib/main.cpp"),
         "path prefix replacement should preserve the relative suffix");
  Expect(ReplacePathPrefix(root / "docs", root / "docs", root / "manual") ==
             std::filesystem::path("/tmp/project/manual"),
         "path prefix replacement should replace exact-prefix paths");

  // RelativePathWithin backs display labels (e.g. review-session summaries): it is a
  // purely lexical relative-string that must never call std::filesystem::relative
  // (which stats/canonicalizes and can fail on deleted paths/symlinks/dead mounts).
  Expect(RelativePathWithin(nested, root) == std::optional<std::string>("src/main.cpp"),
         "relative-within should return the forward-slashed nested suffix");
  Expect(RelativePathWithin(root / "src/./main.cpp", root) ==
             std::optional<std::string>("src/main.cpp"),
         "relative-within should normalize `.` segments without I/O");
  Expect(RelativePathWithin(root, root) == std::nullopt,
         "relative-within should reject the root itself (not a nested file)");
  Expect(RelativePathWithin(root / "../sibling/file.cpp", root) == std::nullopt,
         "relative-within should reject a candidate escaping the root via ..");
  Expect(RelativePathWithin(std::filesystem::path{}, root) == std::nullopt,
         "relative-within should reject an empty candidate");
  Expect(RelativePathWithin(nested, std::filesystem::path{}) == std::nullopt,
         "relative-within should reject an empty root");
  // A path pointing at a non-existent file still yields a label (no filesystem probe).
  Expect(RelativePathWithin(root / "deleted/gone.cpp", root) ==
             std::optional<std::string>("deleted/gone.cpp"),
         "relative-within should label a non-existent path without touching disk");
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
  const std::string readme =
      microide::util::NormalizeLineEndings(ReadFile(TestRoot().parent_path() / "README.md"));
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
  AddTest(tests, "WorkspaceShellSharedCore/ParseCommandLineBoundsHugeInput",
          TestWorkspaceSharedParseCommandLineBoundsHugeInput);
  AddTest(tests, "WorkspaceShared/UiScaleParsing", TestWorkspaceSharedUiScaleParsing);
  AddTest(tests, "WorkspaceShared/CompletePathIsBounded", TestWorkspaceSharedCompletePathIsBounded);
  AddTest(tests, "WorkspaceShared/CompletePathVanishedDirectoryReturnsEmpty",
          TestWorkspaceSharedCompletePathVanishedDirectoryReturnsEmpty);
  AddTest(tests, "WorkspaceShared/QuoteAndLineEndings", TestWorkspaceSharedQuoteAndLineEndings);
  AddTest(tests, "WorkspaceShared/SplitSyntaxLines", TestWorkspaceSharedSplitSyntaxLines);
  AddTest(tests, "WorkspaceShared/SerializeLines", TestWorkspaceSharedSerializeLines);
  AddTest(tests, "WorkspaceShared/ReadFileText", TestWorkspaceSharedReadFileText);
  AddTest(tests, "WorkspaceShared/AtomicTextWrite", TestWorkspaceSharedAtomicTextWrite);
  AddTest(tests, "WorkspaceShared/OutputChannelsParseAndCacheContextSnippets",
          TestWorkspaceOutputChannelsParseAndCacheContextSnippets);
  AddTest(tests, "WorkspaceShared/OutputChannelsResolvedReferencePathCaches",
          TestWorkspaceOutputChannelsResolvedReferencePathCaches);
  AddTest(tests, "WorkspaceShared/OutputChannelsRemoveChannel",
          TestWorkspaceOutputChannelsRemoveChannel);
  AddTest(tests, "WorkspaceShared/OutputChannelsAppendKeepsMetadataCacheStable",
          TestWorkspaceOutputChannelsAppendKeepsMetadataCacheStable);
  AddTest(tests, "WorkspaceShared/OutputChannelsCapsEntries",
          TestWorkspaceOutputChannelsCapsEntries);
  AddTest(tests, "WorkspaceShared/OutputChannelsCapLargeLineBytes",
          TestWorkspaceOutputChannelsCapLargeLineBytes);
  AddTest(tests, "WorkspaceShared/OutputChannelsCapRetainedBytes",
          TestWorkspaceOutputChannelsCapRetainedBytes);
  AddTest(tests, "WorkspaceShared/OutputReferenceRejectsZeroLineOrColumn",
          TestWorkspaceOutputReferenceRejectsZeroLineOrColumn);
  AddTest(tests, "WorkspaceShared/CommandCompletionHelpers",
          TestWorkspaceSharedCommandCompletionHelpers);
  AddTest(tests, "WorkspaceShared/CommandLineCompletionIncludesPluginCommandsByReference",
          TestCommandLineCompletionIncludesPluginCommandsByReference);
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
