#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceOutlineService.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceShellTestAccess.h"
#include "workspace/WorkspaceTabState.h"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace microide::tests {
namespace {

using microide::editor::TextViewport;
using microide::plugin::PluginHost;
using microide::workspace::FindBuiltinSidebarView;
using microide::workspace::LspClient;
using microide::workspace::LspManager;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::SidebarViews;
using microide::workspace::TabEntry;
using microide::workspace::WorkspaceLanguageContract;
using microide::workspace::WorkspaceOutlineService;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::ostringstream encoded;
  encoded << "file://";
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded << static_cast<char>(ch);
      continue;
    }
    encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(ch) << std::nouppercase << std::dec;
  }
  return encoded.str();
}

void RefreshContractsDefaults(WorkspaceLanguageContract& contracts) {
  PluginHost host;
  contracts.Refresh(host);
}

void TestOutlineRegexFallbackBuildsSymbols() {
  TemporaryDirectory tmp;
  const std::filesystem::path path = tmp.path() / "sample.c";
  WriteFile(path, "void hello_world(void) {}\n");

  TextViewport view;
  Expect(view.OpenFile(path), "viewport should open python fixture");

  LspManager lsp;
  WorkspaceOutlineService service;
  WorkspaceLanguageContract contracts;
  RefreshContractsDefaults(contracts);
  ProjectWorkspaceState project;
  project.active_tab_index = 0;
  project.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path.lexically_normal(),
      .title = path.filename().string(),
      .editor_state = WorkspaceShellTestAccess::MakeEditorTabStateForTesting(view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });

  service.ResetCountsForTesting();
  service.SetFixedClockMsForTesting(0);
  service.Poll(0, true, project, lsp, contracts, {});
  Expect(project.sidebar.outline.from_fallback, "outline should use regex fallback without LSP");
  Expect(!project.sidebar.outline.roots.empty(), "regex outline should emit at least one root");
  Expect(project.sidebar.outline.roots.front().name == "hello_world",
         "regex outline should capture function name");
}

void TestOutlineLspPathAppliesDocumentSymbols() {
  TemporaryDirectory tmp;
  const std::filesystem::path path = tmp.path() / "stub.py";
  WriteFile(path, "def x():\n  pass\n");

  TextViewport view;
  Expect(view.OpenFile(path), "viewport should open fixture");

  auto client = std::make_unique<LspClient>();
  client->EnableTestStubMode();
  client->SetTestDocumentSymbolHandler(
      [](std::string /*uri*/, LspClient::DocumentSymbolCallback cb) {
        LspClient::DocumentSymbol sym;
        sym.name = "LspSymbol";
        sym.kind = 12;
        sym.selection_range.start.line = 3;
        sym.selection_range.start.character = 2;
        cb(std::vector<LspClient::DocumentSymbol>{sym});
      });
  const std::string uri = FileUriForPath(path);
  const std::string doc_text =
      microide::util::SerializeLines(view.lines(), microide::util::LineEnding::LF);
  Expect(client->DidOpen(uri, "python", doc_text), "stub DidOpen should succeed");

  LspManager lsp;
  lsp.InstallTestClientForTesting("python", std::move(client));

  WorkspaceOutlineService service;
  WorkspaceLanguageContract contracts;
  RefreshContractsDefaults(contracts);
  ProjectWorkspaceState project;
  project.active_tab_index = 0;
  project.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path.lexically_normal(),
      .title = path.filename().string(),
      .editor_state = WorkspaceShellTestAccess::MakeEditorTabStateForTesting(view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });

  service.ResetCountsForTesting();
  service.SetFixedClockMsForTesting(0);
  service.Poll(0, true, project, lsp, contracts, {});
  lsp.DrainCallbacks();

  Expect(!project.sidebar.outline.from_fallback, "LSP success should not mark fallback");
  Expect(project.sidebar.outline.roots.size() == 1, "LSP should supply one root symbol");
  Expect(project.sidebar.outline.roots.front().name == "LspSymbol",
         "outline should use LSP symbol name");
  Expect(project.sidebar.outline.roots.front().selection_line == 3u,
         "outline should preserve LSP selection line");
  Expect(project.sidebar.outline.roots.front().selection_column == 2u,
         "outline should preserve LSP selection column");
  Expect(service.lsp_request_count_for_testing() == 1, "poll should issue one LSP outline request");
}

void TestOutlineDebounceWaitsForDeadline() {
  TemporaryDirectory tmp;
  const std::filesystem::path path = tmp.path() / "deb.c";
  WriteFile(path, "void a(void) {}\n");

  TextViewport view;
  Expect(view.OpenFile(path), "viewport should open fixture");

  LspManager lsp;
  WorkspaceOutlineService service;
  WorkspaceLanguageContract contracts;
  RefreshContractsDefaults(contracts);
  ProjectWorkspaceState project;
  project.active_tab_index = 0;
  project.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path.lexically_normal(),
      .title = path.filename().string(),
      .editor_state = WorkspaceShellTestAccess::MakeEditorTabStateForTesting(view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });

  service.ResetCountsForTesting();
  service.SetFixedClockMsForTesting(0);
  service.Poll(0, true, project, lsp, contracts, {});
  const std::size_t after_first = service.refresh_count_for_testing();

  service.ScheduleDebouncedRefresh();
  service.SetFixedClockMsForTesting(100);
  service.Poll(0, true, project, lsp, contracts, {});
  Expect(service.refresh_count_for_testing() == after_first,
         "debounced refresh should not run before deadline");

  service.SetFixedClockMsForTesting(200);
  service.Poll(0, true, project, lsp, contracts, {});
  Expect(service.refresh_count_for_testing() == after_first + 1,
         "debounced refresh should run after deadline");
}

void TestOutlineDisabledSkipsWorkAndClears() {
  TemporaryDirectory tmp;
  const std::filesystem::path path = tmp.path() / "off.c";
  WriteFile(path, "void z(void) {}\n");

  TextViewport view;
  Expect(view.OpenFile(path), "viewport should open fixture");

  LspManager lsp;

  WorkspaceOutlineService service;
  WorkspaceLanguageContract contracts;
  RefreshContractsDefaults(contracts);
  ProjectWorkspaceState project;
  project.active_tab_index = 0;
  project.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path.lexically_normal(),
      .title = path.filename().string(),
      .editor_state = WorkspaceShellTestAccess::MakeEditorTabStateForTesting(view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });

  service.ResetCountsForTesting();
  service.SetFixedClockMsForTesting(0);
  service.Poll(0, true, project, lsp, contracts, {});
  Expect(!project.sidebar.outline.roots.empty(), "enabled poll should populate outline");

  service.ResetCountsForTesting();
  service.Poll(0, false, project, lsp, contracts, {});
  Expect(project.sidebar.outline.roots.empty(), "disabled poll should clear outline");
  Expect(service.lsp_request_count_for_testing() == 0,
         "disabled poll should not issue LSP requests");
}

void TestOutlineRegistryOmitsViewWhenSettingOff() {
  PluginHost host;
  auto enabled = SidebarViews(host, true);
  auto disabled = SidebarViews(host, false);
  const auto enabled_has =
      std::find_if(enabled.begin(), enabled.end(),
                   [](const auto& v) { return v.id == "outline"; }) != enabled.end();
  const auto disabled_has =
      std::find_if(disabled.begin(), disabled.end(),
                   [](const auto& v) { return v.id == "outline"; }) != disabled.end();
  Expect(enabled_has, "outline should appear when editor outline is enabled");
  Expect(!disabled_has, "outline should be omitted when editor outline is disabled");
  Expect(FindBuiltinSidebarView("outline") != nullptr, "builtin outline spec should exist");
}

void TestOutlineClickMovesCaret() {
  TemporaryDirectory tmp;
  const std::filesystem::path path = tmp.path() / "jump.py";
  WriteFile(path, "line0\nline1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, path);

  auto& outline = WorkspaceShellTestAccess::CurrentProjectState(shell).sidebar.outline;
  outline.roots.clear();
  outline.collapsed_paths.clear();
  outline.from_fallback = true;
  outline.indexing = false;
  outline.roots.push_back({});
  outline.roots.back().name = "jump_target";
  outline.roots.back().kind = 1;
  outline.roots.back().selection_line = 7;
  outline.roots.back().selection_column = 0;

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect& sb = layout.sidebar;
  constexpr float kSidebarHeaderHeight = 26.0f;
  constexpr float kListTopGap = 6.0f;
  constexpr float kRowHeight = 20.0f;
  const float click_y = sb.y + kSidebarHeaderHeight + kListTopGap + std::floor(kRowHeight / 2.0f);
  const float click_x = sb.x + std::floor(sb.w * 0.65f);

  WorkspaceShellTestAccess::DispatchOutlineSidebarPointerDown(shell, click_x, click_y);

  Expect(editor.cursor_line() == 7u, "outline click should move caret to symbol line");
  Expect(editor.cursor_column() == 0u, "outline click should move caret to symbol column");
}

}  // namespace

void RegisterWorkspaceOutlineServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceOutline/RegexFallbackBuildsSymbols", TestOutlineRegexFallbackBuildsSymbols);
  AddTest(tests, "WorkspaceOutline/LspPathAppliesDocumentSymbols", TestOutlineLspPathAppliesDocumentSymbols);
  AddTest(tests, "WorkspaceOutline/DebounceWaitsForDeadline", TestOutlineDebounceWaitsForDeadline);
  AddTest(tests, "WorkspaceOutline/DisabledSkipsWorkAndClears", TestOutlineDisabledSkipsWorkAndClears);
  AddTest(tests, "WorkspaceOutline/RegistryOmitsViewWhenSettingOff", TestOutlineRegistryOmitsViewWhenSettingOff);
  AddTest(tests, "WorkspaceOutline/ClickMovesCaret", TestOutlineClickMovesCaret);
}

}  // namespace microide::tests
