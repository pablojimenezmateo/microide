#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "TerminalSessionTestAccess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "WorkspaceShellEventHelpers.h"
#include "app/DirtyRegionPolicy.h"
#include "render/ScopedRenderClip.h"
#include "support/FlowFixture.h"
#include "support/GitSidebarWait.h"
#include "support/MockDapAdapter.h"
#include "support/ProjectSearchWait.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "editor/FoldingModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/TextViewport.h"
#include "util/JsonValue.h"
#include "workspace/HoverTextWrap.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/git/CompareTabLoad.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/render/CompareRowUnits.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/SidebarModeRowPaint.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

std::function<void()> PinCaretOf(WorkspaceShell& shell) {
  return [&shell]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
}


void TestLeavingAProblemsRowRepaintsItsHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.cpp", "int x = ;\nint y = ;\n");
  TestAccess::ShowProblemsSidebar(fixture.shell);
  std::vector<editor::Diagnostic> diagnostics;
  for (std::size_t line = 0; line < 2; ++line) {
    diagnostics.push_back(editor::Diagnostic{
        .range = editor::SelectionRange{.start = editor::TextPosition{.line = line, .column = 0},
                                        .end = editor::TextPosition{.line = line, .column = 3}},
        .severity = editor::DiagnosticSeverity::Error,
        .message = "expected expression " + std::to_string(line)});
  }
  Expect(TestAccess::PublishDiagnostics(fixture.shell, "diagnostics", fixture.file, diagnostics),
         "the fixture should publish two diagnostics");
  Expect(TestAccess::RefreshProblemsSidebar(fixture.shell) &&
             TestAccess::ProblemsSidebarEntries(fixture.shell).size() >= 2,
         "the Problems view should list them");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the Problems view with two rows");
  const SDL_FRect row = TestAccess::ProblemsSidebarRowRect(fixture.shell, 1);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(row.x + row.w / 2.0f, row.y + row.h / 2.0f)}),
                      "the pointer onto a Problems row");
  const auto file = fixture.ElementRect("menu:File");
  Expect(file.has_value(), "the File menu item should be on screen");
  if (!file) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(file->x + file->w / 2.0f, file->y + 4.0f)}),
                      "the pointer leaving the Problems row for the menu bar");
}

void TestAnEditThatOpensABlockCommentRepaintsTheLinesBelow() {
  FlowFixture fixture("main.cpp",
                      "#include <cstdio>\n\nint main() {\n  int x = 1;\n  return x;\n}\n");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 0);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("/*")}),
                      "typing /* at the top of the file, which comments out every line below");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}),
                      "a keystroke inside the comment, which leaves the lines below as they are");
  Expect(!TestAccess::ActiveEditor(fixture.shell).LastEditChangedCarriedSyntaxState(0),
         "a keystroke that leaves the carried state alone does not restyle the lines below");
}

void TestMarkingAnOutgoingFileReviewedMarksItsRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "base.txt", "base\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  RequireGitCommandSuccess(root, {"checkout", "-b", "qa-review"}, "git checkout qa-review");
  WriteFile(root / "notes" / "r1.txt", "one\n");
  CommitAll(root, "feature", "feature");

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the review fixture should open its project");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1), "the git view should list the outgoing file");
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);

  Expect(!TestAccess::ExecuteCommandLine(shell, "mark-branch-file-reviewed"),
         "marking a file reviewed with no branch-review diff open should be refused, not report success");

  std::optional<SDL_FRect> row;
  const util::JsonValue before = TestAccess::ControlUi(shell);
  for (const util::JsonValue& entry : before["elements"].AsArray()) {
    if (entry["name"].AsString() == "row:git/r1.txt") {
      const auto& r = entry["rect"].AsArray();
      row = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                      static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(row.has_value(), "the git view should show r1.txt as an outgoing row");
  if (!row) {
    return;
  }
  const float x = row->x + row->w / 2.0f;
  const float y = row->y + row->h / 2.0f;
  (void)DispatchEvents(shell, {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  Expect(TestAccess::ActiveTabIsCompare(shell), "clicking the outgoing row should open its diff");
  Expect(!TestAccess::ExecuteCommandLine(shell, "mark-branch-file-reviewed"),
         "a diff still loading has no changes to mark");
  TestAccess::SettleCompareLoads(shell);
  Expect(TestAccess::ExecuteCommandLine(shell, "mark-branch-file-reviewed"),
         "mark-branch-file-reviewed should run on the outgoing diff");

  const auto& tab = TestAccess::ActiveCompare(shell);
  const auto sidebar = microide::workspace::OutgoingBranchReviewTarget(
      TestAccess::CurrentProjectState(shell).sidebar.git, root);
  const auto describe = [](const microide::compare::BranchReviewTargetIdentity& t) {
    return t.repository_root.string() + " base=" + t.base_commit + " head=" + t.head_commit +
           " merge-base=" + t.merge_base_commit + " gen=" + std::to_string(t.snapshot_generation);
  };
  Expect(sidebar.has_value() && *sidebar == tab.branch_target,
         "the diff should mark the target the git view reads: tab {" + describe(tab.branch_target) +
             "} view {" + (sidebar ? describe(*sidebar) : std::string("none")) + "}");
  const auto& project = TestAccess::CurrentProjectState(shell);
  const microide::workspace::GitSidebarViewModel view =
      microide::workspace::BuildGitSidebarViewModel(project.sidebar.git, root, project.branch_review);
  std::string markers;
  for (const auto& section : view.sections) {
    for (const auto& r : section.rows) {
      markers += r.relative_path + "=[" + r.review_marker_label + "] ";
    }
  }
  Expect(markers.find("r1.txt=[]") == std::string::npos && markers.find("r1.txt=[") != std::string::npos,
         "the git view's row for r1.txt should carry the reviewed marker: " + markers);
  std::string label;
  const util::JsonValue after = TestAccess::ControlUi(shell);
  for (const util::JsonValue& entry : after["elements"].AsArray()) {
    if (entry["name"].AsString() == "row:git/r1.txt") {
      label = entry["label"].AsString();
    }
  }
  Expect(label.size() > std::string("r1.txt").size() && label.ends_with("r1.txt") && label.front() == '[',
         "the ui label of a reviewed row should carry its marker, not \"" + label + "\"");
}

void TestTheGitViewReportsTheSelectedEntrysRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "keys" / "alpha.txt", "alpha\n");
  WriteFile(root / "keys" / "beta.txt", "beta\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(root / "keys" / "alpha.txt", "alpha changed\n");
  WriteFile(root / "keys" / "beta.txt", "beta changed\n");
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  TestAccess::SetWindowSize(shell, 1280, 720);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 2), "the git view should list both changes");
  TestAccess::SetFocusSidebar(shell);
  SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE);
  SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE);
  const util::JsonValue ui = TestAccess::ControlUi(shell);
  std::vector<std::string> selected;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() == "row" && entry["selected"].AsBool()) {
      selected.push_back(entry["name"].AsString());
    }
  }
  Expect(selected == std::vector<std::string>{"row:git/beta.txt"},
         "the selected row is the second entry's, not the line two above it (" +
             (selected.empty() ? std::string("none") : selected.front()) + ")");
}

void TestReloadFromDiskRefusesADirtyBufferWithWhy() {
  FlowFixture fixture("disk.txt", "disk\n");
  TestAccess::ActiveEditor(fixture.shell).InsertText(" edited");
  Expect(TestAccess::ActiveEditor(fixture.shell).dirty(), "typing dirties the buffer");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "reopen"), "reopen on a dirty buffer is refused");
  const std::string feedback = TestAccess::CommandFeedbackText(fixture.shell);
  Expect(feedback.find("unsaved") != std::string::npos, "and says why (\"" + feedback + "\")");
  Expect(TestAccess::ActiveEditor(fixture.shell).dirty(), "the edit is kept");
  TestAccess::ActiveEditor(fixture.shell).Undo();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "reopen"), "a clean buffer reloads");
}

void TestSaveAsRevealsTheNewFileInTheTree() {
  FlowFixture fixture("a.txt", "a\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "save notes/copy.txt"), "save as a new path");
  Expect(TestAccess::SelectedTreePath(fixture.shell) == fixture.root / "notes" / "copy.txt",
         "the tree selects the file the buffer was saved as (" +
             TestAccess::SelectedTreePath(fixture.shell).string() + ")");
}

void TestTypingIntoAMirroredPlaceholderRepaintsEveryMirrorsLine() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("mirrors.txt", "top\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(1, 0, false);
  Expect(TestAccess::PerfExpandSnippetAtCaret(fixture.shell, "${1:ab} first\nmiddle\n${1:ab} second"),
         "the snippet expands");
  Expect(TestAccess::SnippetSessionActive(fixture.shell), "its session is live");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the expansion, painted in full");
  const workspace::RenderInvalidation redraw = DispatchEvents(fixture.shell, {TextInputEvent("Q")});
  Expect(viewport.SerializeText() == "top\nQ first\nmiddle\nQ second", "both mirrors took the keystroke: " +
                                                                           viewport.SerializeText());
  fixture.ExpectFresh(redraw, "a keystroke into a placeholder mirrored on two lines");
}

void TestABreakpointFollowsAnEditOnceNotOnEveryRedraw() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("breaks.txt", "zero\none\ntwo\nthree\nfour\nfive\n");
  TestAccess::ToggleBreakpointForTest(fixture.shell, fixture.file, 5);
  const auto breakpoint_lines = [&] {
    std::vector<std::size_t> lines;
    if (const auto* list = TestAccess::BreakpointStore(fixture.shell).FindByPath(fixture.file)) {
      for (const auto& bp : *list) lines.push_back(bp.line);
    }
    return lines;
  };
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(1, 0, false);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)});
  Expect(breakpoint_lines() == std::vector<std::size_t>{6}, "a line inserted above moves the breakpoint down one");
  viewport.MoveCursorTo(0, 0, false);
  Expect(TestAccess::PerfExpandSnippetAtCaret(fixture.shell, "${1:a}\n${2:b}\n${3:c}\n"), "a snippet expands");
  Expect(TestAccess::SnippetSessionActive(fixture.shell), "its session is live");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB)});
  Expect(breakpoint_lines() == std::vector<std::size_t>{9}, "the three lines above move it down three");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB)});
  Expect(viewport.SerializeText().starts_with("a\nb\nc\nzero\n"), "Tab only moved between placeholders: " +
                                                                    viewport.SerializeText().substr(0, 20));
  const std::vector<std::size_t> after = breakpoint_lines();
  Expect(after == std::vector<std::size_t>{9},
         "moving between placeholders leaves the breakpoint where it is (at " +
             (after.empty() ? std::string("none") : std::to_string(after.front())) + ")");
}

void TestCtrlQQuits() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("quit.txt", "text\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Q, SDL_KMOD_CTRL)});
  Expect(fixture.shell.ConsumeQuitRequested(), "Ctrl+Q in the editor should quit");
  if (TestAccess::TerminalTabCount(fixture.shell) == 0) {
    TestAccess::OpenTerminalViaService(fixture.shell);
  }
  TestAccess::SetPanelContentTerminal(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus panel"), "focus the terminal");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Q, SDL_KMOD_CTRL)});
  Expect(!fixture.shell.ConsumeQuitRequested(), "Ctrl+Q in a focused terminal reaches the program");
}

void TestClosingTheLastTerminalUnderAnOutputChannelRepaintsTheTabStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("term.txt", "text\n");
  if (TestAccess::TerminalTabCount(fixture.shell) == 0) {
    TestAccess::OpenTerminalViaService(fixture.shell);
  }
  while (TestAccess::TerminalTabCount(fixture.shell) > 1) {
    (void)TestAccess::ExecuteCommandLine(fixture.shell, "term-close");
  }
  Expect(TestAccess::TerminalTabCount(fixture.shell) == 1, "the fixture should have one terminal");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa", 3);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term-close"), "term-close should close the terminal");
  Expect(TestAccess::TerminalTabCount(fixture.shell) == 0, "no terminal should be left");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(),
                      "closing the last terminal under an output channel");
}

void TestATerminalTitleRepaintsItsTab() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("title.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["panel"]["visible"].AsBool(), "the panel should show the terminal");
  const auto& panel = ui["surfaces"]["bottomPanel"].AsArray();

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]0;qa-title\x07title-set");
  workspace::RenderInvalidation body;
  body.rects.push_back(SDL_FRect{static_cast<float>(panel[0].AsInt()),
                                 static_cast<float>(panel[1].AsInt()) + workspace::kWorkspaceBottomPanelHeaderHeight,
                                 static_cast<float>(panel[2].AsInt()),
                                 static_cast<float>(panel[3].AsInt()) - workspace::kWorkspaceBottomPanelHeaderHeight});
  fixture.ExpectFresh(body, "the shell setting its title");
}

void TestATerminalPaneCursorChangeRepaintsItsCaret() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("caret.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term-split"), "term-split should split the terminal");
  Expect(TestAccess::TerminalPaneCount(fixture.shell) == 2, "the terminal should have two panes");
  const std::size_t inactive = TestAccess::ActiveTerminalPaneIndex(fixture.shell) == 0 ? 1 : 0;
  auto& session = TestAccess::TerminalPaneSession(fixture.shell, inactive);
  TerminalSessionTestAccess::Reset(session, 8, 40);
  TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?25l");
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f});
  fixture.ExpectFresh(elsewhere, "an inactive pane's cursor hiding");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?25h\x1b[3;5H");
  fixture.ExpectFresh(elsewhere, "an inactive pane's cursor showing again elsewhere");
}

void TestTerminalOutputSampledByFramePrepRepaintsItsPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("term.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 8, 40);
  TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  TerminalSessionTestAccess::AppendOutput(session, "\r\n-- INSERT --\r\nrow two\r\nrow three");
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f});
  fixture.ExpectFresh(elsewhere, "terminal output sampled by frame prep");
}

void TestASidebarScrollOfOneRowRepaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("f" + std::to_string(i) + ".txt"), "base\n");
  }
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("f" + std::to_string(i) + ".txt"), "changed\n");
  }

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 40), "the git view should list the forty changed files");
  TestAccess::NoteLayoutInputsChanged(shell);
  TestAccess::SetSidebarScrollRow(shell, 2);
  SoftwareCanvas canvas(kWidth, kHeight);
  TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::SidebarScrollRow(shell) == 2, "the list should stay scrolled to row 2");

  TestAccess::SetSidebarScrollRow(shell, 3);
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{kWidth - 6.0f, kHeight - 6.0f, 4.0f, 4.0f});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, elsewhere,
                                                [&]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); });
  Expect(diff.identical(), "a one-row scroll of the git view should repaint it; " + DescribePixelDiff(diff));
}

void TestNothingBeneathAnOpenMenuHovers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("menu.txt", "text\n");
  const auto help = fixture.ElementRect("menu:Help");
  const auto view = fixture.ElementRect("sidebar-view:search");
  Expect(help.has_value() && view.has_value(), "the Help menu and the search view button should be on screen");
  if (!help || !view) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*help), "opening the Help menu");
  Expect(TestAccess::MenuBarOpen(fixture.shell), "the Help menu should be open");
  const float x = view->x + view->w / 2.0f;
  const float y = view->y + view->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}),
                      "the pointer moving over the sidebar beneath an open menu");
}

void TestClosingAMenuOverAFindToggleRepaintsItsHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.cpp", "alpha\nbeta\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search zzz"),
         "the fixture should open the find widget");
  TestAccess::SetFocusSidebar(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "opening the find widget");
  const workspace::FindWidgetLayout fw = TestAccess::FindWidgetControls(fixture.shell, false);
  const auto help = fixture.ElementRect("menu:Help");
  Expect(help.has_value(), "the Help menu should be on screen");
  if (!help) {
    return;
  }
  for (std::size_t index = 0; index < fw.toggle_count; ++index) {
    fixture.ExpectFresh(fixture.Click(*help), "opening the Help menu");
    Expect(TestAccess::MenuBarOpen(fixture.shell), "the Help menu should be open");
    const SDL_FRect& toggle = fw.toggle_buttons[index];
    fixture.ExpectFresh(fixture.Click(toggle),
                        "a click that closes the menu over find toggle " + std::to_string(index));
    Expect(!TestAccess::MenuBarOpen(fixture.shell), "the click should close the menu");
  }
}

void TestLeavingAFindToggleOverTheSidebarRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const auto& [w, h] : {std::pair{290, 218}, std::pair{360, 260}, std::pair{640, 480}}) {
    FlowFixture fixture("main.cpp", "alpha\nbeta\n", w, h);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search zzz"), "the fixture opens the find widget");
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "opening the find widget");
    const workspace::FindWidgetLayout fw = TestAccess::FindWidgetControls(fixture.shell, false);
    const std::string size = std::to_string(w) + "x" + std::to_string(h);
    for (std::size_t index = 0; index < fw.toggle_count; ++index) {
      const SDL_FRect& toggle = fw.toggle_buttons[index];
      Expect(toggle.w <= 0.0f || (toggle.x >= fw.widget.x && toggle.x + toggle.w <= fw.widget.x + fw.widget.w),
             size + ": find toggle " + std::to_string(index) + " is inside the card or not laid out");
    }
    for (std::size_t index = 0; index < fw.toggle_count; ++index) {
      const SDL_FRect& toggle = fw.toggle_buttons[index];
      if (toggle.w <= 0.0f) {
        continue;
      }
      const float y = toggle.y + toggle.h / 2.0f;
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(toggle.x + toggle.w / 2.0f, y)}),
                          size + ": onto find toggle " + std::to_string(index));
      const float away = fw.widget.x + fw.widget.w - 2.0f;
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(away, fw.widget.y + fw.widget.h - 2.0f)}),
                          size + ": off find toggle " + std::to_string(index) + " within the card");
    }
  }
}

void TestAFocusMoveByTheMaximizedPanelRulePaintsTheRingInItsFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.cpp", "alpha\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  TestAccess::TogglePanelMaximizedForTest(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "maximizing the panel");
  auto& state = TestAccess::CurrentProjectState(fixture.shell);
  state.panel.maximized_seen_focus.reset();
  TestAccess::SetFocusEditor(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(state.surface.focus == workspace::FocusTarget::Panel,
         "the maximized panel should take the keyboard from the editor it hides");
  const SDL_FRect menu_bar = TestAccess::CurrentLayout(fixture.shell).menu_bar;
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = false, .rects = {menu_bar}},
                      "a later frame repainting only the menu bar");
}

void TestADiffPaintsInsideItsPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.txt", "alpha\nbeta\ngamma\n");
  const std::filesystem::path right = fixture.root / "right.txt";
  WriteFile(right, "alpha\nBETA\ngamma\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.font_size 32"), "a large font");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.wrap word"), "word wrap");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.panel", 60);
  TestAccess::SetBottomPanelHeight(fixture.shell, static_cast<float>(FlowFixture::kHeight) - 200.0f);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const auto panel_pixels = [&]() {
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    const auto& panel = ui["surfaces"]["bottomPanel"].AsArray();
    std::vector<std::uint32_t> pixels;
    if (panel.size() == 4) {
      for (int y = static_cast<int>(panel[1].AsInt()); y < panel[1].AsInt() + panel[3].AsInt(); ++y) {
        for (int x = static_cast<int>(panel[0].AsInt()); x < panel[0].AsInt() + panel[2].AsInt(); x += 3) {
          const SDL_Color c = fixture.canvas.PixelAt(x, y);
          pixels.push_back((std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b);
        }
      }
    }
    return std::pair{pixels, ui};
  };
  const auto [file_panel, file_ui] = panel_pixels();
  Expect(!file_panel.empty(), "the panel is shown");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + right.string()),
         "compare-files should open a diff");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const auto [diff_panel, diff_ui] = panel_pixels();
  const auto& editor = diff_ui["surfaces"]["editorSurface"].AsArray();
  Expect(editor.size() == 4 && editor[3].AsInt() < 140,
         "the editor area is too short for the diff's summary, header and a row");
  Expect(diff_panel == file_panel, "the diff paints nothing over the panel below it");
}

void TestTypingThatReshapesADiffRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string body;
  for (int i = 1; i <= 30; ++i) {
    body += "line " + std::to_string(i) + "\n";
  }
  FlowFixture fixture("left.txt", "# title\n\n" + body);
  const std::filesystem::path right = fixture.root / "right.txt";
  WriteFile(right, "\n" + body);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "compare-files " + fixture.file.string() + " " + right.string()),
         "compare-files should open a diff");
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(0, 0);
  TestAccess::SetFocusEditor(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const int scroll_before = compare.scroll_row;

  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("b")}),
                      "a keystroke that grows a hunk and moves every row below it");
  Expect(TestAccess::ActiveCompare(fixture.shell).scroll_row == scroll_before,
         "the keystroke should not scroll the diff (the test is about its shape)");
}

void TestAClickBelowADiffsLastRowRepaintsTheWorkingSide() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.txt", "alpha\nbeta\ngamma\n");
  const std::filesystem::path right = fixture.root / "right.txt";
  WriteFile(right, "alpha\nBETA\ngamma\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "compare-files " + fixture.file.string() + " " + right.string()),
         "compare-files should open a diff");
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(1, 0);
  compare.right_viewport.MoveCursorTo(1, 4, true);
  TestAccess::SetFocusEditor(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the diff with a selection on the working side");
  const auto surface = TestAccess::ControlUi(fixture.shell)["surfaces"]["editorSurface"].AsArray();
  const float x = static_cast<float>(surface[0].AsInt() + surface[2].AsInt() * 3 / 4);
  const float y = static_cast<float>(surface[1].AsInt() + surface[3].AsInt() - 40);
  fixture.ExpectFresh(fixture.Click(SDL_FRect{x, y, 2.0f, 2.0f}), "a click below the last row");
  Expect(!TestAccess::ActiveCompare(fixture.shell).right_view_active,
         "the click leaves the working side inactive (the case under test)");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after it");
}

void TestFindWidgetReplaceRepaintsTheDirtyTab() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("find.txt", "total total\n");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_H, SDL_KMOD_CTRL), TextInputEvent("total")}),
      "Ctrl+H and a query");
  const auto replace_field = fixture.ElementRect("field:find/replace");
  Expect(replace_field.has_value(), "the replace widget should name its replace field");
  if (!replace_field) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*replace_field), "focusing the replace field");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("sum")}), "typing the replacement");
  const auto replace_all = fixture.ElementRect("button:find/replace-all");
  Expect(replace_all.has_value(), "the replace widget should name Replace All");
  if (!replace_all) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*replace_all), "Replace All (the tab goes dirty)");
  Expect(TestAccess::ControlQuery(fixture.shell, "buffer")["text"].AsString() == "sum sum\n",
         "Replace All should replace both matches");
}

void TestBannerReloadRepaintsTheCleanTab() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("disk.txt", "one\n");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "typing (the tab goes dirty)");
  WriteFile(fixture.file, "two\n");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!TestAccess::HasExternalChangeBanner(fixture.shell, fixture.file) &&
         std::chrono::steady_clock::now() < deadline) {
    TestAccess::ReloadProjectIfFilesChanged(fixture.shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(TestAccess::HasExternalChangeBanner(fixture.shell, fixture.file),
         "the external change should raise the banner on the dirty buffer");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the banner appearing");
  const auto reload = fixture.ElementRect("button:banner/Reload");
  Expect(reload.has_value(), "the banner should name its Reload button");
  if (!reload) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*reload), "Reload (the tab goes clean)");
  Expect(TestAccess::ControlQuery(fixture.shell, "buffer")["text"].AsString() == "two\n",
         "Reload should take the disk's text");
}

void TestABannerRepaintsFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("disk.txt", "one\n");
  workspace::RenderInvalidation frame;
  frame.frame = true;
  workspace::SetEditorBanner(TestAccess::CurrentProjectState(fixture.shell),
                             workspace::EditorBannerState::Kind::ExternalChange, fixture.file, "changed on disk");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(frame, "a banner raised with no request");
  const auto reload = fixture.ElementRect("button:banner/Reload");
  Expect(reload.has_value(), "the banner should name its Reload button");
  if (!reload) {
    return;
  }
  TestAccess::SetMousePositionForTest(fixture.shell, reload->x + reload->w * 0.5f, reload->y + reload->h * 0.5f);
  fixture.ExpectFresh(frame, "the pointer resting on Reload");
  TestAccess::SetMousePositionForTest(fixture.shell, reload->x + reload->w * 0.5f, reload->y + reload->h + 40.0f);
  fixture.ExpectFresh(frame, "the pointer gone from Reload");
  TestAccess::CurrentProjectState(fixture.shell).focused_group().active_tab().editor_state->content =
      workspace::TabEntry::EditorTabState::Content::Failed;
  workspace::SetEditorBanner(TestAccess::CurrentProjectState(fixture.shell),
                             workspace::EditorBannerState::Kind::OpenFailed, fixture.file, "permission denied");
  fixture.ExpectFresh(frame, "the banner turned into a failed open's");
  workspace::SetEditorBanner(TestAccess::CurrentProjectState(fixture.shell),
                             workspace::EditorBannerState::Kind::OpenFailed, fixture.file,
                             "the file is being written by another process");
  fixture.ExpectFresh(frame, "the failed open's reason reworded with no request");
}

void TestThePointerLeavingATabOverflowChipRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("t00.txt", "zero\n");
  for (int i = 1; i < 16; ++i) {
    const std::filesystem::path file =
        fixture.root / ("t" + std::to_string(i) + "_a_long_name_so_the_strip_overflows.txt");
    WriteFile(file, "tab\n");
    TestAccess::OpenFile(fixture.shell, file);
  }
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto overflow = TestAccess::EditorTabOverflowControls(fixture.shell);
  Expect(overflow.hidden_left > 0, "the last tab active hides tabs on the strip's left");
  if (overflow.hidden_left == 0) {
    return;
  }
  const SDL_FRect chip = overflow.left_button;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(chip.x + chip.w / 2.0f,
                                                                      chip.y + chip.h / 2.0f)}),
                      "the pointer moving onto the overflow chip");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(chip.x + chip.w / 2.0f,
                                                                      chip.y + chip.h + 200.0f)}),
                      "the pointer leaving it for the editor");
}

void TestThePointerOnABannerButtonRepaintsItsHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("disk.txt", "one\n");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "typing (the tab goes dirty)");
  WriteFile(fixture.file, "two\n");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!TestAccess::HasExternalChangeBanner(fixture.shell, fixture.file) &&
         std::chrono::steady_clock::now() < deadline) {
    TestAccess::ReloadProjectIfFilesChanged(fixture.shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(TestAccess::HasExternalChangeBanner(fixture.shell, fixture.file),
         "the external change should raise the banner on the dirty buffer");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the banner appearing");
  const auto reload = fixture.ElementRect("button:banner/Reload");
  Expect(reload.has_value(), "the banner should name its Reload button");
  if (!reload) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(reload->x + reload->w / 2.0f,
                                                                      reload->y + reload->h / 2.0f)}),
                      "the pointer moving onto the banner's Reload button");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(reload->x + reload->w / 2.0f,
                                                                      reload->y + reload->h + 40.0f)}),
                      "the pointer moving off it, into the editor");
}

void TestTheBannerSitsInTheFocusedGroupAndLeavesWithIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("top.txt", "top\n");
  WriteFile(fixture.root / "disk.txt", "one\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-down disk.txt"), "split-down should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right other.txt"), "split-right should run");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "three groups: top.txt over disk.txt | other.txt");
  const SDL_FRect disk_pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 1);
  const SDL_FRect other_pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 2);
  const float px = disk_pane.x + disk_pane.w / 2.0f;
  const float py = disk_pane.y + disk_pane.h - 20.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, px, py),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, px, py),
                                                     TextInputEvent("x")}),
                      "a click into disk.txt's group and typing (the tab goes dirty)");
  WriteFile(fixture.root / "disk.txt", "two\n");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!TestAccess::HasExternalChangeBanner(fixture.shell, fixture.root / "disk.txt") &&
         std::chrono::steady_clock::now() < deadline) {
    TestAccess::ReloadProjectIfFilesChanged(fixture.shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(TestAccess::HasExternalChangeBanner(fixture.shell, fixture.root / "disk.txt"),
         "the external change should raise the banner on the dirty buffer");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the banner appearing");
  const auto reload = fixture.ElementRect("button:banner/Reload");
  Expect(reload.has_value(), "the banner should name its Reload button");
  if (!reload) {
    return;
  }
  Expect(reload->x >= disk_pane.x && reload->x + reload->w <= disk_pane.x + disk_pane.w &&
             reload->y >= disk_pane.y && reload->y + reload->h <= disk_pane.y + disk_pane.h,
         "the banner's Reload button should sit in disk.txt's group, the focused one");
  const float ox = other_pane.x + other_pane.w / 2.0f;
  const float oy = other_pane.y + other_pane.h - 20.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, ox, oy),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, ox, oy)}),
                      "a click into other.txt's group (the banner leaves with the focus)");
}

void TestTypingOnAFoldedOpenerRepaintsItsRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("fold.cpp",
                      "#include <x>\n\nint a = 1;\n\nnamespace n {\n\nvoid f() {\n  g();\n}\n\n}\n");
  editor::FoldingModel* folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && folds->FoldStartingAt(6).has_value(), "f's body should be foldable");
  if (folds == nullptr) {
    return;
  }
  (void)folds->Collapse(6);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "collapsing the fold");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(6, 10);
  fixture.ExpectFresh(full, "placing the caret");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(" ")}),
                      "typing at the end of the folded opener");
  Expect(TestAccess::ControlQuery(fixture.shell, "buffer")["text"].AsString().find("void f() { \n") !=
             std::string::npos,
         "the space should land on the opener line");
}

void TestAFoldChordRightAfterAClickFoldsTheClickedBlock() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("scheduler.cpp",
                      "#include \"scheduler.hpp\"\n\n#include <algorithm>\n#include <cstdio>\n\n"
                      "namespace taskflow {\n\n"
                      "void Scheduler::add(std::string name, int priority, int cost) {\n"
                      "    queue_.push_back(Task{std::move(name), priority, cost});\n}\n\n"
                      "int Scheduler::run() {\n    int total = 0;\n    return total;\n}\n\n"
                      "}  // namespace taskflow\n");
  editor::FoldingModel* folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && folds->FoldStartingAt(7).has_value(), "add's body should be foldable");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"].AsArray()[0];
  const auto row_y = [&pane](std::int64_t line) {
    for (const util::JsonValue& row : pane["rows"].AsArray()) {
      if (row["line"].AsInt() == line) {
        return static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() / 2.0);
      }
    }
    return -1.0f;
  };
  const float line9_y = row_y(9);
  const float line12_y = row_y(12);
  Expect(line9_y >= 0.0f && line12_y >= 0.0f, "lines 9 and 12 should be on screen");
  const float fold_x = static_cast<float>(
      (pane["foldHitX"].AsDouble() + pane["gutterX"].AsDouble() + pane["gutterWidth"].AsDouble()) / 2.0);
  workspace::RenderInvalidation full;
  full.full = true;
  for (int toggle = 0; toggle < 2; ++toggle) {
    (void)DispatchEvents(fixture.shell, {MouseMotionEvent(fold_x, line12_y),
                                         MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, fold_x, line12_y),
                                         MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, fold_x, line12_y)});
    fixture.ExpectFresh(full, "a frame after the gutter click");
    SDL_Delay(600);
  }
  folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && !folds->has_any_collapsed_fold(), "the two gutter clicks should leave nothing folded");
  const float x = static_cast<float>(pane["textX"].AsDouble() + 4.2 * pane["charWidth"].AsDouble());
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseMotionEvent(x, line9_y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, line9_y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, line9_y),
                                     KeyDownEvent(SDLK_LEFTBRACKET, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)}),
      "the click and the fold chord in one batch");
  Expect(TestAccess::ActiveEditor(fixture.shell).cursor_line() == 8, "the click should put the caret on line 9");
  folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && folds->IsCollapsedAtOpener(7),
         "Ctrl+Shift+[ right after the click should fold the clicked block (lines 8-10)");
}

void TestAnEditThatAddsAFoldMarkerRepaintsTheGutter() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("indent.cpp", "int f() {\nreturn 1;\n}\na\nb\n");
  editor::FoldingModel* folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && !folds->FoldStartingAt(2).has_value(), "the closing brace starts no fold yet");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file as opened");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"].AsArray()[0];
  float line4_y = 0.0f;
  for (const util::JsonValue& row : pane["rows"].AsArray()) {
    if (row["line"].AsInt() == 4) {
      line4_y = static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() / 2.0);
    }
  }
  const float text_x = static_cast<float>(pane["textX"].AsDouble()) + 2.0f;
  fixture.ExpectFresh(fixture.Click(SDL_FRect{text_x - 1.0f, line4_y - 1.0f, 2.0f, 2.0f}), "the click on line 4");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN, SDL_KMOD_SHIFT),
                                                     KeyDownEvent(SDLK_END, SDL_KMOD_SHIFT)}),
                      "selecting the two lines under the brace");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB, SDL_KMOD_NONE)}),
                      "Tab over the two lines under the brace");
  folds = TestAccess::EnsureActiveFoldingModelFresh(fixture.shell);
  Expect(folds != nullptr && folds->FoldStartingAt(2).has_value(),
         "the indented lines should make the brace's line a fold opener");
}

void TestTabDragDropPreviewPaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("shared.txt", "shared\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "other.txt");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the split");
  const auto tab = fixture.ElementRect("tab:0/other.txt");
  Expect(tab.has_value(), "the left group should show other.txt's tab");
  if (!tab) {
    return;
  }
  const float x0 = tab->x + tab->w / 2.0f;
  const float y0 = tab->y + tab->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y0),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0)}),
                      "pressing on a tab");
  const float x1 = static_cast<float>(FlowFixture::kWidth) * 0.8f;
  const float body_y = static_cast<float>(FlowFixture::kHeight) * 0.45f;
  bool previewed = false;
  for (int step = 1; step <= 8; ++step) {
    const float x = x0 + (x1 - x0) * static_cast<float>(step) / 8.0f;
    const float y = y0 + (body_y - y0) * static_cast<float>(step) / 8.0f;
    SDL_Event motion = MouseMotionEvent(x, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the tab, step " + std::to_string(step));
    previewed = previewed || TestAccess::TabDragBodyDropActive(fixture.shell);
  }
  Expect(previewed, "dragging a tab over a pane body should show the drop preview");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP,
                                                      x1, body_y)}),
      "dropping the tab");
}

void TestATabDraggedAcrossStackedPanesPaintsItsGhostInTheTargetStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("first.txt", "first\n");
  WriteFile(fixture.root / "second.txt", "second\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "second.txt");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-down"), "split-down should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-up"), "focus-group-up should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "two panes over one");
  std::optional<SDL_FRect> tab;
  std::optional<SDL_FRect> bottom_tab;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() != "tab") {
      continue;
    }
    const auto& r = entry["rect"].AsArray();
    const SDL_FRect rect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    if (rect.x > FlowFixture::kWidth / 2.0f && rect.y < FlowFixture::kHeight / 4.0f &&
        (!tab.has_value() || rect.x < tab->x)) {
      tab = rect;
    }
    if (rect.y > FlowFixture::kHeight / 4.0f && (!bottom_tab.has_value() || rect.x < bottom_tab->x)) {
      bottom_tab = rect;
    }
  }
  Expect(tab.has_value() && bottom_tab.has_value(), "the top-right and bottom panes should show tabs");
  if (!tab.has_value() || !bottom_tab.has_value()) {
    return;
  }
  const float x0 = tab->x + tab->w / 2.0f;
  const float y0 = tab->y + tab->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y0),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0)}),
                      "pressing the top-right pane's tab");
  const float x1 = bottom_tab->x + 20.0f;
  const float y1 = bottom_tab->y + bottom_tab->h / 2.0f;
  for (int step = 1; step <= 8; ++step) {
    const float x = x0 + (x1 - x0) * static_cast<float>(step) / 8.0f;
    const float y = y0 + (y1 - y0) * static_cast<float>(step) / 8.0f;
    SDL_Event motion = MouseMotionEvent(x, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the tab down and left, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y1)}),
                      "dropping it in the bottom strip");
}

void TestATabGhostOverANarrowerStripStaysInsideIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a-rather-long-file-name-for-the-ghost.txt", "first\n", 640, 300);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right again, halving the right pane");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "a wide pane and two narrow ones");
  std::vector<SDL_FRect> tabs;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() != "tab") {
      continue;
    }
    const auto& r = entry["rect"].AsArray();
    tabs.push_back(SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                             static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())});
  }
  std::sort(tabs.begin(), tabs.end(), [](const SDL_FRect& a, const SDL_FRect& b) { return a.x < b.x; });
  Expect(tabs.size() == 3, "one tab in each of the three panes");
  if (tabs.size() != 3) {
    return;
  }
  const SDL_FRect source = tabs[0];
  const SDL_FRect middle = tabs[1];
  Expect(source.w > tabs[2].x - middle.x, "the left pane's tab is wider than the middle strip (the case under test)");
  const float x0 = source.x + source.w / 2.0f;
  const float y0 = source.y + source.h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y0),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0)}),
                      "pressing the left pane's tab");
  const float x1 = middle.x + 10.0f;
  for (int step = 1; step <= 6; ++step) {
    const float x = x0 + (x1 - x0) * static_cast<float>(step) / 6.0f;
    SDL_Event motion = MouseMotionEvent(x, y0);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the tab over the middle strip, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y0)}),
                      "dropping it in the middle strip");
}

void TestAPanelShownMaximizedTakesTheKeyboardAtOnce() {
  for (const bool by_wake : {true, false}) {
    FlowFixture fixture("notes.txt", "notes\n");
    TestAccess::OpenTerminalViaService(fixture.shell);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "panel-maximize should run");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-toggle"), "panel-toggle should hide the panel");
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the panel hidden, still maximized");
    TestAccess::SetFocusSidebar(fixture.shell);
    TestAccess::ShowOutputChannelWithLines(fixture.shell, "git", 3);
    Expect(TestAccess::BottomPanelVisible(fixture.shell), "the output shows the panel again");
    if (by_wake) {
      (void)TestAccess::HandleScheduledWake(fixture.shell);
    } else {
      (void)DispatchEvents(fixture.shell, {MouseMotionEvent(10.0f, 10.0f)});
    }
    Expect(!TestAccess::FocusIsSidebar(fixture.shell),
           std::string("after ") + (by_wake ? "a wake" : "an event") +
               ", the keyboard should leave the sidebar the maximized panel covers");
  }
}

void TestATabDraggedAcrossACloseButtonLeavesItIdle() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("first.txt", "first\n");
  WriteFile(fixture.root / "second.txt", "second\n");
  WriteFile(fixture.root / "third.txt", "third\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "second.txt");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "third.txt");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "three tabs");
  const auto dragged = fixture.ElementRect("tab:0/third.txt");
  const auto close = fixture.ElementRect("button:close-tab/0/second.txt");
  Expect(dragged.has_value() && close.has_value(), "the strip should name third.txt and second.txt's close button");
  if (!dragged || !close) {
    return;
  }
  const float y = dragged->y + dragged->h / 2.0f;
  const float x0 = dragged->x + dragged->w / 2.0f;
  const float x1 = close->x - 12.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing on third.txt's tab");
  for (int step = 1; step <= 8; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 + (x1 - x0) * static_cast<float>(step) / 8.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the tab, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y)}),
                      "dropping the tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x1 - 3.0f, y)}),
                      "the next pointer motion after the drop");

  const auto again = fixture.ElementRect("tab:0/second.txt");
  const auto over = fixture.ElementRect("button:close-tab/0/first.txt");
  if (!again || !over) {
    return;
  }
  const float ax = again->x + again->w / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(ax, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, ax, y)}),
                      "pressing on second.txt's tab");
  const float cx = over->x + over->w / 2.0f;
  for (int step = 1; step <= 4; ++step) {
    SDL_Event motion = MouseMotionEvent(ax + (cx - ax) * static_cast<float>(step) / 4.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging onto first.txt's close button, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, cx - 14.0f, y)}),
                      "a release beside the close button the last motion was over");
  for (int wake = 0; wake < 40; ++wake) {
    SDL_Delay(10);
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "the settle glide, wake " + std::to_string(wake));
  }
}

void TestAHeldTabDragPaintsItsLastSnap() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  WriteFile(fixture.root / "b.txt", "b\n");
  WriteFile(fixture.root / "c.txt", "c\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "b.txt");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "c.txt");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "three tabs");
  const auto dragged = fixture.ElementRect("tab:0/c.txt");
  const auto first = fixture.ElementRect("tab:0/a.txt");
  Expect(dragged.has_value() && first.has_value(), "the strip should name c.txt and a.txt");
  if (!dragged || !first) {
    return;
  }
  const float y = dragged->y + dragged->h / 2.0f;
  const float x0 = dragged->x + dragged->w / 2.0f;
  const float x1 = first->x + 4.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing on c.txt's tab");
  for (int step = 1; step <= 8; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 + (x1 - x0) * static_cast<float>(step) / 8.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the tab, step " + std::to_string(step));
  }
  for (int wake = 0; wake < 40; ++wake) {
    SDL_Delay(10);
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "the held drag's ease, wake " + std::to_string(wake));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y)}),
                      "dropping the tab");
  for (int wake = 0; wake < 40; ++wake) {
    SDL_Delay(10);
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "the settle glide, wake " + std::to_string(wake));
  }
}

void TestACodeActionsTitleStaysInsideItsMenu() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n", 213, 160);
  TestAccess::SetSidebarVisible(fixture.shell, true);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a 213x160 window with the sidebar");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "indentation"), "indentation should run");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the indentation picker opening");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "code-actions"), "code-actions should run");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the Code Actions menu opening");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}),
                      "a key in the open menu (a partial frame clipped to the card)");
}

void TestAPaletteCardOverTheMenuBarKeepsItsItemUnlit() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n", 213, 160);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "layout-mode-toggle"), "layout-mode-toggle should run");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a 213x160 window");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F1)}), "the command palette opening");
  const auto file = fixture.ElementRect("menu:File");
  Expect(file.has_value(), "the menu bar should name menu:File");
  if (!file) {
    return;
  }
  const SDL_FRect card = TestAccess::ActiveOverlayRect(fixture.shell);
  Expect(card.y > file->y && card.y < file->y + file->h, "the palette card should cover the lower part of File");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(36.0f, 25.0f)}), "the pointer on the list");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(file->x + 30.0f, card.y + 4.0f)}),
                      "the pointer onto the card where it covers File");
}

void TestTabIndentingASecondarySelectionRepaintsEachOfItsLines() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n");
  auto& editor = TestAccess::ActiveEditor(fixture.shell);
  editor.MoveCursorTo(9, 2);
  const editor::SelectionRange spanning{.start = {.line = 0, .column = 3}, .end = {.line = 1, .column = 2}};
  editor.SetSecondaryCaretsWithRanges(std::span<const editor::SelectionRange>(&spanning, 1));
  fixture.PinCaret();
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a primary caret on line 10 and a selection over lines 1-2");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB)}), "Tab");
  Expect(editor.lines()[1].starts_with("\t") || editor.lines()[1].starts_with(" "),
         "the selection's second line should be indented");
}

void TestHoveringARowAfterAltRightToTheNextMenuRepaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the window");
  const auto rows_of = [&](std::string_view menu) {
    std::vector<SDL_FRect> rows;
    const std::string prefix = "menu-item:" + std::string(menu) + "/";
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString().starts_with(prefix)) {
        const auto& r = entry["rect"].AsArray();
        rows.push_back(SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                                 static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())});
      }
    }
    return rows;
  };
  const auto git = fixture.ElementRect("menu:Git");
  const auto remote = fixture.ElementRect("menu:Remote");
  Expect(git.has_value() && remote.has_value(), "the menu bar should name menu:Git and menu:Remote");
  if (!git || !remote) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*remote), "opening Remote");
  const std::vector<SDL_FRect> remote_rows = rows_of("Remote");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "closing it");
  Expect(remote_rows.size() >= 6, "the Remote menu should list rows");
  if (remote_rows.size() < 6) {
    return;
  }
  const SDL_FRect resting = remote_rows[4];
  const SDL_FRect target = remote_rows.back();
  fixture.ExpectFresh(fixture.Click(*git), "opening Git");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseMotionEvent(resting.x + 120.0f, resting.y + resting.h / 2.0f)}),
      "the pointer at rest over Git's popup");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHT, SDL_KMOD_ALT)}),
                      "Alt+Right opening Remote under the pointer");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(target.x + 120.0f, target.y + target.h / 2.0f)}),
                      "the pointer on to Remote's last row");
}

void TestAnOpenMenuRepaintsARowWhoseEnablementChanges() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "term should run");
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::SetRunning(session, true);
  TestAccess::SetFocusEditor(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the window with a terminal");
  const auto terminal = fixture.ElementRect("menu:Terminal");
  Expect(terminal.has_value(), "the menu bar should name menu:Terminal");
  if (!terminal) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*terminal), "opening the Terminal menu");
  TerminalSessionTestAccess::EmitProcessExitMarker(session);
  TerminalSessionTestAccess::SetRunning(session, false);
  TestAccess::ConsumeTerminalSessionUpdates(fixture.shell);
  Expect(TestAccess::MenuBarOpen(fixture.shell), "the menu should stay open while the shell exits");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the shell exiting under the open menu");
}

void TestATabDraggedAcrossThreePanesPaintsFresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const std::string scale : {"1", "1.6"}) {
    FlowFixture fixture("a.txt", "a\n");
    WriteFile(fixture.root / "b.txt", "b\n");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale " + scale),
           "set-setting ui.scale should run");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "a second split-right should run");
    TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "b.txt");
    workspace::RenderInvalidation full;
    full.full = true;
    fixture.ExpectFresh(full, "three panes at scale " + scale);
    const auto dragged = fixture.ElementRect("tab:2/b.txt");
    const auto target = fixture.ElementRect("tab:0/a.txt");
    Expect(dragged.has_value() && target.has_value(), "the strips should name tab:2/b.txt and tab:0/a.txt");
    if (!dragged || !target) {
      return;
    }
    const float y = dragged->y + dragged->h / 2.0f;
    const float x0 = dragged->x + dragged->w / 2.0f;
    const float x1 = target->x + 4.0f;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                        "pressing on b.txt's tab at scale " + scale);
    for (int step = 1; step <= 24; ++step) {
      SDL_Event motion = MouseMotionEvent(x0 + (x1 - x0) * static_cast<float>(step) / 24.0f, y);
      motion.motion.state = SDL_BUTTON_LMASK;
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                          "dragging across the panes at scale " + scale + ", step " + std::to_string(step));
    }
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y)}),
                        "dropping the tab at scale " + scale);
    for (int wake = 0; wake < 30; ++wake) {
      SDL_Delay(10);
      fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                          "the settle glide at scale " + scale + ", wake " + std::to_string(wake));
    }
  }
}

void TestATabDragAbandonedWithEscapeThenReleasedPaintsFresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  WriteFile(fixture.root / "b.txt", "b\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "a second split-right should run");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "b.txt");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "three panes");
  const auto dragged = fixture.ElementRect("tab:2/b.txt");
  const auto target = fixture.ElementRect("tab:0/a.txt");
  Expect(dragged.has_value() && target.has_value(), "the strips should name tab:2/b.txt and tab:0/a.txt");
  if (!dragged || !target) {
    return;
  }
  const float y = dragged->y + dragged->h / 2.0f;
  const float x0 = dragged->x + dragged->w / 2.0f;
  const float x1 = target->x + 4.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing on b.txt's tab");
  for (int step = 1; step <= 12; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 + (x1 - x0) * static_cast<float>(step) / 12.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}), "dragging into the first strip, step " +
                                                                     std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape abandons the drag");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y)}),
                      "releasing after Escape");
  for (int wake = 0; wake < 30; ++wake) {
    SDL_Delay(10);
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "the settle after Escape, wake " + std::to_string(wake));
  }
}

void TestADraggedTabPaintsNothingUnderTheStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  WriteFile(fixture.root / "b.txt", "b\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "b.txt");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "two tabs");
  const auto dragged = fixture.ElementRect("tab:0/b.txt");
  Expect(dragged.has_value(), "the strip names tab:0/b.txt");
  if (!dragged) {
    return;
  }
  const SDL_FRect strip = TestAccess::CurrentLayout(fixture.shell).tab_strip;
  const int below = static_cast<int>(std::ceil(strip.y + strip.h));
  const auto rows_under_strip = [&]() {
    SDL_FlushRenderer(fixture.canvas.renderer());
    std::vector<Uint32> pixels;
    SDL_Surface* surface = fixture.canvas.surface();
    for (int y = below; y < below + 4; ++y) {
      for (int x = static_cast<int>(strip.x); x < static_cast<int>(strip.x + strip.w); ++x) {
        Uint8 r = 0, g = 0, b = 0, a = 0;
        (void)SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
        pixels.push_back((Uint32{r} << 24) | (Uint32{g} << 16) | (Uint32{b} << 8) | a);
      }
    }
    return pixels;
  };
  const std::vector<Uint32> before = rows_under_strip();
  const float y = dragged->y + dragged->h / 2.0f;
  const float x0 = dragged->x + dragged->w / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing on b.txt's tab");
  for (int step = 1; step <= 6; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 - static_cast<float>(step), y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}), "nudging the tab, step " + std::to_string(step));
  }
  Expect(TestAccess::TabDrag(fixture.shell).dragging, "the nudge is a drag");
  Expect(rows_under_strip() == before, "the dragged tab paints nothing under the strip");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x0 - 6.0f, y)}),
                      "dropping the tab");
}

void TestSavingADeletedFileWritesItBack() {
  FlowFixture fixture("gone.txt", "gone\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_NONE)});
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "type _dirty"), "type should edit the buffer");
  std::filesystem::remove(fixture.file);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "save"), "saving a deleted file's buffer should succeed");
  Expect(std::filesystem::exists(fixture.file) && ReadFile(fixture.file).rfind("gone_dirty", 0) == 0,
         "the save should write the buffer back, got: " +
             (std::filesystem::exists(fixture.file) ? ReadFile(fixture.file) : std::string("(no file)")));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "type !"), "type should edit the buffer");
  WriteFile(fixture.file, "someone else's edit\n");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "save"), "a save over an external change is refused");
  Expect(ReadFile(fixture.file) == "someone else's edit\n", "the external edit stays on disk");
}

void TestDetectedIndentationSurvivesTabSwitchesAndSettings() {
  FlowFixture fixture("start.txt", "start\n");
  WriteFile(fixture.root / "two.py", "def f():\n  if x:\n    return 1\n  return 2\n");
  WriteFile(fixture.root / "tabs.c", "int f() {\n\tif (x) {\n\t\treturn 1;\n\t}\n}\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open two.py"), "open two.py");
  const auto active = [&]() -> const editor::TextViewport& {
    return TestAccess::GroupActiveViewport(fixture.shell, 0);
  };
  const auto describe = [&]() {
    return std::string(active().soft_tabs() ? "spaces " : "tabs ") + std::to_string(active().tab_size());
  };
  Expect(active().soft_tabs() && active().tab_size() == 2, "two.py should open as two spaces: " + describe());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open tabs.c"), "open tabs.c");
  Expect(!active().soft_tabs(), "tabs.c should open as tabs: " + describe());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open two.py"), "back to two.py");
  Expect(active().soft_tabs() && active().tab_size() == 2,
         "two.py should keep its two spaces after a tab switch: " + describe());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.tab_size 8"), "set tab_size");
  Expect(active().soft_tabs() && active().tab_size() == 2,
         "a settings change should not undo the detected indentation: " + describe());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "tab-size 3"), "tab-size 3");
  Expect(active().tab_size() == 3, "tab-size should change the buffer it is run on: " + describe());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open tabs.c") &&
             TestAccess::ExecuteCommandLine(fixture.shell, "open two.py"),
         "away and back again");
  Expect(active().tab_size() == 3, "the buffer keeps the size it was given: " + describe());
}

void TestReverseExecutionIsRefusedWithoutAReverseAdapter() {
  FlowFixture fixture("main.txt", "main\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "debug-step-back"),
         "debug-step-back should be refused without an adapter that steps back");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "debug-reverse-continue"),
         "debug-reverse-continue should be refused without an adapter that runs backwards");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "breakpoint-exception-condition throw x > 1"),
         "a condition on an exception filter no adapter advertised should be refused");
}

void TestCommandPaletteListsRecentCommandsFirst() {
  FlowFixture fixture("main.txt", "b\na\n");
  const auto run = [&](std::string_view query) {
    TestAccess::OpenCommandPalette(fixture.shell);
    TestAccess::SetCommandPaletteQueryAndRefresh(fixture.shell, query);
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  };
  run("word wrap");
  run("sort lines ascending");
  TestAccess::OpenCommandPalette(fixture.shell);
  TestAccess::SetCommandPaletteQueryAndRefresh(fixture.shell, "");
  const std::string first = TestAccess::CommandPaletteMatchLabelAt(fixture.shell, 0);
  const std::string second = TestAccess::CommandPaletteMatchLabelAt(fixture.shell, 1);
  Expect(first.starts_with("Sort Lines") && second == "Word Wrap",
         "the last two commands run lead the empty palette, newest first: " + first + ", " + second);
}

void TestEscapeReturnsFocusToWhereTheOverlayOpened() {
  FlowFixture fixture("main.txt", "alpha beta\n");
  const SDL_Keymod ctrl_shift = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);

  TestAccess::SetFocusEditor(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl_shift)});
  Expect(TestAccess::OverlayVisible(fixture.shell), "Ctrl+Shift+P should open the palette");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  Expect(!TestAccess::OverlayVisible(fixture.shell), "Escape should close the palette");
  Expect(TestAccess::FocusIsEditor(fixture.shell),
         "the palette opened from the editor gives focus back to the editor, not the visible sidebar");

  TestAccess::SetFocusEditor(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_SPACE, SDL_KMOD_CTRL)});
  if (TestAccess::OverlayVisible(fixture.shell)) {
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
    Expect(TestAccess::FocusIsEditor(fixture.shell),
           "the completion list closed by Escape gives focus back to the editor");
  }

  TestAccess::SetFocusSidebar(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl_shift)});
  Expect(TestAccess::OverlayVisible(fixture.shell), "Ctrl+Shift+P from the tree should open the palette");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  Expect(TestAccess::FocusIsSidebar(fixture.shell), "the palette opened from the tree gives focus back to the tree");
}

void TestTheCompactLayoutReturnsWhenTheWindowGrows() {
  FlowFixture fixture("main.txt", "alpha\n");
  const auto menus = [&]() {
    std::size_t count = 0;
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      count += entry["kind"].AsString() == "menu" ? 1 : 0;
    }
    return count;
  };
  Expect(menus() > 0, "the regular layout shows the menu bar's titles");
  TestAccess::SetWindowSize(fixture.shell, 640, 480);
  fixture.shell.Render(fixture.canvas.renderer(), 640, 480);
  Expect(menus() == 0, "a 640-px window folds the menus into the chevron");
  TestAccess::SetWindowSize(fixture.shell, FlowFixture::kWidth, FlowFixture::kHeight);
  (void)menus();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  Expect(menus() > 0, "grown back, the menu bar shows its titles again");
}

void TestTheMenuOverflowChevronIsAHoverTarget() {
  FlowFixture fixture("main.txt", "alpha\n");
  TestAccess::SetWindowSize(fixture.shell, 640, 480);
  fixture.shell.Render(fixture.canvas.renderer(), 640, 480);
  const std::optional<SDL_FRect> chevron = TestAccess::MenuOverflowChevronRect(fixture.shell);
  Expect(chevron.has_value(), "a 640-px window folds the menus into the chevron");
  if (!chevron.has_value()) {
    return;
  }
  const std::optional<SDL_FRect> hovered = TestAccess::HoveredInteractiveRect(
      fixture.shell, chevron->x + chevron->w / 2.0f, chevron->y + chevron->h / 2.0f);
  Expect(hovered.has_value() && hovered->x == chevron->x && hovered->w == chevron->w,
         "the pointer on the chevron hovers the chevron");
}

void TestTabOverASelectionEndingOnTheLastLineRepaintsTheLineTheCaretLeft() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("data.json", "{\"a\": [1, 2, {\"b\": null}], \"c\": 1}\n\n");
  editor::TextViewport& view = TestAccess::ActiveEditor(fixture.shell);
  view.MoveCursorTo(0, view.lines()[0].size());
  view.MoveCursorTo(view.line_count() - 1, 0, true);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a selection from the first line's end to the last line");
  for (int press = 1; press <= 2; ++press) {
    const std::size_t caret_before = view.cursor_line();
    const auto redraw = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB, SDL_KMOD_NONE)});
    fixture.ExpectFresh(redraw, "Tab " + std::to_string(press) + " over the selection (caret line " +
                                    std::to_string(caret_before) + " -> " + std::to_string(view.cursor_line()) +
                                    ")");
  }
}

void TestAnEditThatAddsTheHorizontalScrollbarRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("long.txt", "short\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a short file, no horizontal scrollbar");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 5);
  const std::string wide(400, 'w');
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(wide.c_str())}),
                      "a paste wider than the editor (the horizontal scrollbar appears)");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}),
                      "undoing it (the scrollbar goes)");
}

void TestClosingAPromptOverTheFinderGivesTheFinderTheKeyboard() {
  FlowFixture fixture("main.txt", "alpha\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, SDL_KMOD_CTRL)});
  Expect(TestAccess::OverlayVisible(fixture.shell), "Ctrl+P opens the finder");
  TestAccess::SetFocusSidebar(fixture.shell);
  TestAccess::OpenPromptSurfaceForTest(fixture.shell, workspace::PromptSurfaceState::Action::RenamePath,
                                       workspace::PromptSurfaceState::Kind::TextInput, fixture.file, "",
                                       "renamed.txt");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "sidebar-hide"), "sidebar-hide should run");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  Expect(TestAccess::OverlayVisible(fixture.shell), "the finder is still open under the closed prompt");
  Expect(!TestAccess::FocusIsSidebar(fixture.shell), "focus is not left on the hidden sidebar");
  Expect(TestAccess::FocusIsOverlay(fixture.shell), "the finder still on screen takes the keyboard");
}

void TestConfirmingASidebarPromptOverAHiddenSidebarKeepsTheKeyboard() {
  using Action = workspace::PromptSurfaceState::Action;
  for (const Action action : {Action::DeletePath, Action::RenamePath, Action::CreateDirectory}) {
    FlowFixture fixture("main.txt", "alpha\n");
    WriteFile(fixture.root / "victim.txt", "victim\n");
    TestAccess::SetFocusSidebar(fixture.shell);
    const std::string input = action == Action::RenamePath ? "renamed.txt" : action == Action::CreateDirectory ? "newdir" : "";
    TestAccess::OpenPromptSurfaceForTest(fixture.shell, action,
                                         action == Action::DeletePath ? workspace::PromptSurfaceState::Kind::Confirm
                                                                      : workspace::PromptSurfaceState::Kind::TextInput,
                                         action == Action::CreateDirectory ? fixture.root : fixture.root / "victim.txt",
                                         "", input);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "sidebar-hide"), "sidebar-hide should run");
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
    Expect(!TestAccess::PromptSurfaceVisible(fixture.shell),
           "Enter confirms the prompt (action " + std::to_string(static_cast<int>(action)) + ")");
    Expect(!TestAccess::FocusIsSidebar(fixture.shell),
           "after confirming, focus is not left on the hidden sidebar (action " +
               std::to_string(static_cast<int>(action)) + ")");
  }
}

void TestClickingTheOtherGroupAfterATreeDragRepaintsTheTabStrips() {
  FlowFixture fixture("main.txt", "alpha\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the start");
  const float tree_x = 120.0f;
  const float tree_y = static_cast<float>(FlowFixture::kHeight) * 0.52f;
  SDL_Event drag = MouseMotionEvent(tree_x - 28.0f, tree_y + 10.0f);
  drag.motion.state = SDL_BUTTON_LMASK;
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseMotionEvent(tree_x, tree_y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, tree_x, tree_y), drag,
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, tree_x - 28.0f, tree_y + 10.0f)}),
      "a drag in the tree's empty space");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  fixture.ExpectFresh(full, "the split");
  const auto left_tab = fixture.ElementRect("tab:0/main.txt");
  Expect(left_tab.has_value(), "the left group shows main.txt's tab");
  if (!left_tab) {
    return;
  }
  const SDL_FRect strip_space{left_tab->x + left_tab->w + 60.0f, left_tab->y, 20.0f, left_tab->h};
  fixture.ExpectFresh(fixture.Click(strip_space), "a click in the left strip's empty space");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after it");
  const SDL_FRect left_pane{300.0f, 300.0f, 200.0f, 100.0f};
  fixture.ExpectFresh(fixture.Click(left_pane), "a click in the left pane");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after that");
}

void TestThePromptsCaretGoesWhenTheWindowLosesTheFocus() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha\n");
  workspace::RenderInvalidation full;
  full.full = true;
  TestAccess::OpenPromptSurfaceForTest(fixture.shell, workspace::PromptSurfaceState::Action::GoToLine,
                                       workspace::PromptSurfaceState::Kind::TextInput, fixture.root, "");
  fixture.ExpectFresh(full, "the goto-line prompt");
  Expect(TestAccess::CurrentTextInputSurface(fixture.shell) == workspace::TextInputSurface::PromptInput,
         "the prompt holds the keyboard");
  SDL_Event lost{};
  lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {lost}), "the window losing the focus");
  SDL_Event gained{};
  gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {gained}), "the window taking it back");
}

void TestATreeDragsPreviewAndGhostRepaintFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the start");
  const auto& entries = TestAccess::TreeEntries(fixture.shell);
  std::optional<std::size_t> index;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].path.filename() == "main.txt") {
      index = i;
    }
  }
  const std::optional<SDL_FRect> row =
      index.has_value() ? TestAccess::TreeSidebarRowRect(fixture.shell, *index) : std::nullopt;
  Expect(row.has_value(), "main.txt's row is on screen");
  if (!row) {
    return;
  }
  const float x = row->x + row->w * 0.5f;
  const float y = row->y + row->h * 0.5f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y)}),
                      "the press on the row");
  const SDL_FRect editor = TestAccess::CurrentLayout(fixture.shell).editor_surface;
  for (const auto& [px, py, what] :
       {std::tuple{x + 30.0f, y + 12.0f, "the drag leaving the row"},
        std::tuple{editor.x + editor.w * 0.5f, editor.y + editor.h * 0.5f, "the drag over the pane"},
        std::tuple{editor.x + editor.w - 12.0f, editor.y + editor.h * 0.5f, "the drag at the pane's right edge"},
        std::tuple{editor.x + editor.w * 0.4f, editor.y + editor.h * 0.6f, "the drag back over the pane"}}) {
    SDL_Event motion = MouseMotionEvent(px, py);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}), what);
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP,
                                                                      editor.x + editor.w * 0.4f,
                                                                      editor.y + editor.h * 0.6f)}),
                      "the drop");
}

void TestAClickThatClearsASelectionUnderTheFindWidgetRepaintsIt() {
  FlowFixture fixture("main.txt", "alpha beta gamma delta\nsecond line here\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the start");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search"), "search opens the find widget");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the find widget");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto& editor = ui["editors"].AsArray().front();
  const float text_x = static_cast<float>(editor["textX"].AsDouble());
  const float first_y = static_cast<float>(editor["firstLineY"].AsDouble());
  const float line_h = static_cast<float>(editor["lineHeight"].AsDouble());
  const float char_w = static_cast<float>(editor["charWidth"].AsDouble());
  const float word_x = text_x + char_w * 8.0f;
  const float word_y = first_y + line_h * 0.5f;
  SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, word_x, word_y);
  SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, word_x, word_y);
  down.button.clicks = 2;
  up.button.clicks = 2;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(word_x, word_y), down, up}),
                      "a double-click selecting a word");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after it");
  const float other_y = first_y + line_h * 1.5f;
  fixture.ExpectFresh(fixture.Click(SDL_FRect{text_x + char_w * 3.0f, other_y - 2.0f, 4.0f, 4.0f}),
                      "a click on the next line that clears the selection");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after that");
}

void TestAClickThatClearsATerminalSelectionUnderTheFindWidgetRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::AppendOutput(session, "To run a command as administrator\r\nsecond line\r\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "the panel maximizes");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the maximized terminal");
  TestAccess::SetFocusPanel(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "terminal-find"), "terminal-find opens the find bar");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the find widget");
  Expect(TestAccess::TerminalFind(fixture.shell).visible(), "the find bar is open");
  const SDL_FPoint word = TestAccess::TerminalCellPoint(fixture.shell, 0, 12);
  const SDL_FPoint other = TestAccess::TerminalCellPoint(fixture.shell, 1, 3);
  for (int round = 0; round < 2; ++round) {
    workspace::RenderInvalidation damage = DispatchEvents(fixture.shell, {MouseMotionEvent(word.x, word.y)});
    for (const Uint8 clicks : {Uint8{1}, Uint8{2}}) {
      SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, word.x, word.y);
      SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, word.x, word.y);
      down.button.clicks = clicks;
      up.button.clicks = clicks;
      damage.Merge(DispatchEvents(fixture.shell, {down, up}));
    }
    fixture.ExpectFresh(damage, "a double-click selecting a word in the terminal");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after it");
    fixture.ExpectFresh(fixture.Click(SDL_FRect{other.x - 1.0f, other.y - 1.0f, 2.0f, 2.0f}),
                        "a click on the next row that clears the selection");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after that");
  }
}

void TestATerminalSelectionChangeWithNoDamageRepaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::AppendOutput(session, "To run a command as administrator\r\n");
  TestAccess::SetFocusPanel(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the terminal");
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f});
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, workspace::TerminalSelectionPoint{0, 9},
                                                workspace::TerminalSelectionPoint{0, 16});
  fixture.ExpectFresh(elsewhere, "a selection made with no damage of its own");
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, std::nullopt, std::nullopt);
  fixture.ExpectFresh(elsewhere, "the selection cleared with no damage of its own");
}

void TestFocusingAnotherGroupByCommandRepaintsBothPanes() {
  FlowFixture fixture("main.txt", "alpha\nbeta\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-left"), "focus-group-left runs");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the split, left group focused");
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f});
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-right"), "focus-group-right runs");
  fixture.ExpectFresh(elsewhere, "focus moved to the right group by a command");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-left"), "focus-group-left runs");
  fixture.ExpectFresh(elsewhere, "and back to the left group");
}

void TestRedoAfterASaveInASplitKeepsBothPanesFresh() {
  FlowFixture fixture("main.txt", "alpha\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the start");
  for (const char* command : {"open on", "copy-line-down", "split-right", "focus-group-left", "save", "focus-group-right",
                              "redo", "redo"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command) || std::string(command) == "redo", command);
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), std::string("after ") + command);
  }
}

void TestAFocusedPaneShortenedFromAnotherGroupShowsItsReconciledCaret() {
  FlowFixture fixture("main.txt", "one\ntwo\nthree\nfour\nfive\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  const auto frame = [&fixture] { fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height); };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right runs");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "goto 5"), "goto 5 runs in the right pane");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open other.txt"), "the right pane opens another file");
  frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-left"), "focus-group-left runs");
  TestAccess::ActiveEditor(fixture.shell).ReplaceRange(editor::SelectionRange{{0, 0}, {5, 0}}, "only\n");
  frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-right"), "focus-group-right runs");
  frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open main.txt"), "the right pane's main.txt tab comes back");
  frame();

  const editor::TextViewport& right = TestAccess::ActiveEditor(fixture.shell);
  const std::string caret = "Ln " + std::to_string(right.cursor_line() + 1) + ", Col " +
                            std::to_string(right.cursor_visual_column() + 1);
  const std::string label =
      TestAccess::StatusBarSegmentText(fixture.shell, workspace::StatusBarSegmentId::LineColumn);
  Expect(right.cursor_line() < right.line_count(), "the right pane's caret is back inside the document");
  Expect(label == caret, "the status bar names the caret the frame drew: '" + label + "' vs '" + caret + "'");
}

void TestANarrowSearchToggleHoverKeepsEveryFrameFresh() {
  FlowFixture fixture("main.txt", "alpha\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "sidebar-show search"), "the search view shows");
  TestAccess::SetSidebarWidth(fixture.shell, 120.0f);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the narrow search view");
  const auto hidden = fixture.ElementRect("button:search/hidden");
  Expect(hidden.has_value(), "the Hidden toggle is on screen");
  if (!hidden) {
    return;
  }
  const float x = hidden->x + hidden->w / 2.0f;
  const float y = hidden->y + hidden->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}), "the pointer onto Hidden");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y + 200.0f)}), "and off it");
}

void TestToastsShowWithTheStatusBarHidden() {
  FlowFixture fixture("main.txt", "alpha\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "status-bar-toggle"), "status-bar-toggle should run");
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Warning, "a toast");
  bool seen = false;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["surfaces"]["statusBar"].IsNull(), "the status bar is hidden");
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() != "notification") {
      continue;
    }
    seen = true;
    const auto& r = entry["rect"].AsArray();
    Expect(r[0].AsInt() >= 0 && r[1].AsInt() >= 0 && r[0].AsInt() + r[2].AsInt() <= FlowFixture::kWidth &&
               r[1].AsInt() + r[3].AsInt() <= FlowFixture::kHeight,
           "the toast is inside the window (" + std::to_string(r[0].AsInt()) + "," +
               std::to_string(r[1].AsInt()) + ")");
  }
  Expect(seen, "the toast is laid out");
}

void TestOpeningAModalUnderThePointerDropsTheHoverItTakes() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("first.txt", "first\n");
  WriteFile(fixture.root / "second.txt", "second\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "second.txt");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_EQUALS, SDL_KMOD_CTRL)});
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "two tabs");
  const auto tab = fixture.ElementRect("tab:0/first.txt");
  Expect(tab.has_value(), "the strip should name first.txt");
  if (!tab) {
    return;
  }
  const float x = tab->x + tab->w * 0.3f;
  const float y = tab->y + tab->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}), "the pointer on first.txt's tab");
  fixture.ExpectFresh(fixture.Click(*tab), "first.txt's tab clicked");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "an edit (the tab goes dirty)");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x - 20.0f, y)}), "the pointer back on the tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))}),
      "Ctrl+Shift+P with the pointer on a tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x + 3.0f, y)}),
                      "a pointer move on the tab under the open palette");
}

void TestDraggingTheSidebarShutKeepsEveryFrameFresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha beta\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the editor beside the tree");
  const auto divider = fixture.ElementRect("divider:sidebar");
  Expect(divider.has_value(), "the ui names the sidebar divider");
  if (!divider) {
    return;
  }
  const float y = divider->y + divider->h * 0.8f;
  const float x0 = divider->x + divider->w / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing the sidebar divider");
  for (int step = 1; step <= 12; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 + (1.0f - x0) * static_cast<float>(step) / 12.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}), "dragging the divider, step " + std::to_string(step));
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "a scheduled frame mid-drag, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, 1.0f, y)}),
                      "releasing at the window's edge");
  for (int wake = 0; wake < 20; ++wake) {
    SDL_Delay(15);
    fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                        "a scheduled frame after the release, " + std::to_string(wake));
  }
}

void DragEveryDividerToItsExtremes(FlowFixture& fixture) {
  std::vector<std::string> dividers;
  {
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["kind"].AsString() == "divider") {
        dividers.push_back(entry["name"].AsString());
      }
    }
  }
  Expect(dividers.size() >= 3, "the ui names the dividers (" + std::to_string(dividers.size()) + ")");
  for (const std::string& name : dividers) {
    for (const int direction : {-1, 1}) {
      const auto rect = fixture.ElementRect(name);
      if (!rect) {
        continue;
      }
      const bool vertical = rect->h > rect->w;
      const float x0 = rect->x + rect->w / 2.0f;
      const float y0 = rect->y + rect->h / 2.0f;
      const float extreme_x = direction < 0 ? 1.0f : static_cast<float>(FlowFixture::kWidth) - 2.0f;
      const float extreme_y = direction < 0 ? 1.0f : static_cast<float>(FlowFixture::kHeight) - 2.0f;
      const std::string what = name + (direction < 0 ? " toward the start" : " toward the end");
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y0),
                                                         MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0)}),
                          "pressing " + what);
      for (int step = 1; step <= 8; ++step) {
        const float t = static_cast<float>(step) / 8.0f;
        SDL_Event motion = MouseMotionEvent(vertical ? x0 + (extreme_x - x0) * t : x0,
                                            vertical ? y0 : y0 + (extreme_y - y0) * t);
        motion.motion.state = SDL_BUTTON_LMASK;
        fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}), "dragging " + what + ", step " + std::to_string(step));
        fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw,
                            "a scheduled frame dragging " + what + ", step " + std::to_string(step));
      }
      const float x1 = vertical ? extreme_x : x0;
      const float y1 = vertical ? y0 : extreme_y;
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y1)}),
                          "releasing " + what);
      fixture.ExpectFresh(TestAccess::HandleScheduledWake(fixture.shell).redraw, "a scheduled frame after " + what);
    }
  }
}

void TestEveryDividerDraggedToItsExtremesKeepsEveryFrameFresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha beta gamma delta\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right other.txt"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-toggle"), "the debug pane opens");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "sidebar, split, debug pane, panel");
  DragEveryDividerToItsExtremes(fixture);
}

void TestDividersDraggedInANarrowEditorColumnKeepEveryFrameFresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.txt", "alpha beta gamma delta\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right other.txt"), "split-right should run");
  for (int i = 0; i < 6; ++i) {
    const std::filesystem::path more = fixture.root / ("more-" + std::to_string(i) + ".txt");
    WriteFile(more, "more\n");
    TestAccess::OpenFileInNewTab(fixture.shell, more);
  }
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-toggle"), "the debug pane opens");
  TestAccess::SetSidebarWidth(fixture.shell, 700.0f);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "a wide sidebar, a split, the debug pane");
  DragEveryDividerToItsExtremes(fixture);
}

void CollectControlOverlaps(const util::JsonValue& ui, const std::string& what,
                            std::vector<std::string>& out) {
  std::vector<std::pair<std::string, SDL_FRect>> controls;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string kind = entry["kind"].AsString();
    if (kind != "button" && kind != "sidebar-view" && kind != "debug-tab" && kind != "panel-tab") {
      continue;
    }
    const auto& r = entry["rect"].AsArray();
    const SDL_FRect rect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
      continue;
    }
    const auto inside = [](const SDL_FRect& a, const SDL_FRect& b) {
      return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
    };
    for (const auto& [other_name, other] : controls) {
      const float ix = std::min(rect.x + rect.w, other.x + other.w) - std::max(rect.x, other.x);
      const float iy = std::min(rect.y + rect.h, other.y + other.h) - std::max(rect.y, other.y);
      if (ix > 1.0f && iy > 1.0f && !inside(rect, other) && !inside(other, rect) && out.size() < 40) {
        out.push_back(what + ": " + entry["name"].AsString() + " overlaps " + other_name);
      }
    }
    controls.emplace_back(entry["name"].AsString(), rect);
  }
}

void TestEveryElementStaysInsideTheWindowAtEverySize() {
  FlowFixture fixture("main.txt", "alpha beta gamma delta\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right other.txt"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-toggle"), "the debug pane opens");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  std::vector<std::pair<int, int>> sizes;
  for (int w = 1280, h = 720; w >= 320; w -= 80, h = std::max(240, h - 40)) {
    sizes.emplace_back(w, h);
  }
  sizes.emplace_back(360, 900);
  sizes.emplace_back(1400, 260);
  const float preferred_sidebar = TestAccess::SidebarWidth(fixture.shell);
  std::vector<std::string> violations;
  const auto check = [&](const std::string& what, int width, int height) {
    TestAccess::SetWindowSize(fixture.shell, width, height);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    SoftwareCanvas canvas{width, height};
    fixture.shell.Render(canvas.renderer(), width, height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      const auto& r = entry["rect"].AsArray();
      const std::int64_t x = r[0].AsInt(), y = r[1].AsInt(), w = r[2].AsInt(), h = r[3].AsInt();
      if (!(x >= -1 && y >= -1 && x + w <= width + 1 && y + h <= height + 1) && violations.size() < 40) {
        violations.push_back(what + " at " + std::to_string(width) + "x" + std::to_string(height) + ": " +
                             entry["name"].AsString() + " [" + std::to_string(x) + "," + std::to_string(y) +
                             "," + std::to_string(w) + "," + std::to_string(h) + "]");
      }
    }
    if (what == "every surface open") {
      CollectControlOverlaps(ui, what + " at " + std::to_string(width) + "x" + std::to_string(height),
                             violations);
    }
  };
  for (const auto& [w, h] : sizes) {
    check("every surface open", w, h);
  }
  Expect(TestAccess::SidebarWidth(fixture.shell) == preferred_sidebar,
         "the sidebar keeps its width through a 320-px window");
  const SDL_Keymod ctrl_shift = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  TestAccess::SetWindowSize(fixture.shell, FlowFixture::kWidth, FlowFixture::kHeight);
  TestAccess::SetFocusEditor(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl_shift)});
  for (const auto& [w, h] : sizes) {
    check("the palette open", w, h);
  }
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  TestAccess::SetWindowSize(fixture.shell, FlowFixture::kWidth, FlowFixture::kHeight);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_COMMA, SDL_KMOD_CTRL)});
  for (const auto& [w, h] : sizes) {
    check("Settings open", w, h);
  }
  std::string all;
  for (const std::string& v : violations) {
    all += "\n  " + v;
  }
  Expect(violations.empty(), "every element stays inside the window, none overlapping another:" + all);
}

void TestEveryElementStaysInsideItsSurfaceAtEverySize() {
  FlowFixture fixture("main.txt", "alpha beta gamma delta\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-breakpoints"), "the debug pane opens");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  const std::vector<std::pair<std::string, std::string>> owners = {
      {"sidebar-view:", "sidebar"}, {"row:", "sidebar"},          {"button:sidebar/", "sidebar"},
      {"button:search/", "sidebar"}, {"field:search/", "sidebar"}, {"debug-tab:", "rightPane"},
      {"debug-row:", "rightPane"},   {"panel-tab:", "bottomPanel"}, {"button:panel/", "bottomPanel"},
      {"close-panel-tab", "bottomPanel"},
  };
  std::vector<std::string> violations;
  const auto check = [&](const std::string& what) {
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      const std::string name = entry["name"].AsString();
      for (const auto& [prefix, surface] : owners) {
        if (!name.starts_with(prefix) && !name.starts_with("button:" + prefix)) {
          continue;
        }
        const util::JsonValue& owner = ui["surfaces"][surface];
        if (owner.IsNull()) {
          break;
        }
        const auto& o = owner.AsArray();
        const auto& r = entry["rect"].AsArray();
        const std::int64_t x = r[0].AsInt(), y = r[1].AsInt(), w = r[2].AsInt(), h = r[3].AsInt();
        const std::int64_t ox = o[0].AsInt(), oy = o[1].AsInt(), ow = o[2].AsInt(), oh = o[3].AsInt();
        if (w > 0 && h > 0 && !(x >= ox - 1 && y >= oy - 1 && x + w <= ox + ow + 1 && y + h <= oy + oh + 1) &&
            violations.size() < 40) {
          violations.push_back(what + ": " + name + " [" + std::to_string(x) + "," + std::to_string(y) + "," +
                               std::to_string(w) + "," + std::to_string(h) + "] outside " + surface + " [" +
                               std::to_string(ox) + "," + std::to_string(oy) + "," + std::to_string(ow) + "," +
                               std::to_string(oh) + "]");
        }
        break;
      }
    }
    CollectControlOverlaps(ui, what, violations);
  };
  for (const char* view : {"tree", "search", "git", "problems", "tests", "outline"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, std::string("sidebar-show ") + view), view);
    for (float width = 20.0f; width <= 420.0f; width += 13.0f) {
      TestAccess::SetSidebarWidth(fixture.shell, width);
      check(std::string(view) + " sidebar " + std::to_string(static_cast<int>(width)) + " px");
    }
  }
  TestAccess::SetSidebarWidth(fixture.shell, 240.0f);
  for (float width = 20.0f; width <= 520.0f; width += 17.0f) {
    TestAccess::SetDebugPaneWidth(fixture.shell, width);
    check("debug pane " + std::to_string(static_cast<int>(width)) + " px");
  }
  TestAccess::SetDebugPaneWidth(fixture.shell, 288.0f);
  for (const char* content : {"term"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, content), content);
    for (float height = 20.0f; height <= 600.0f; height += 19.0f) {
      TestAccess::SetBottomPanelHeight(fixture.shell, height);
      check(std::string(content) + " panel " + std::to_string(static_cast<int>(height)) + " px");
    }
  }
  std::string all;
  for (const std::string& v : violations) {
    all += "\n  " + v;
  }
  Expect(violations.empty(), "every element stays inside its surface; these leave it:" + all);
}

void TestAnEditInOneHalfOfASplitShowsInTheOther() {
  FlowFixture fixture("split.txt", "split\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  const auto render = [&]() {
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  };
  render();
  Expect(TestAccess::EditorGroupCount(fixture.shell) == 2, "split-right opens a second group");
  const std::size_t focused = TestAccess::FocusedGroupIndex(fixture.shell);
  const std::size_t other = focused == 0 ? 1 : 0;
  Expect(TestAccess::GroupActiveViewport(fixture.shell, other)
             .SharesDocumentWith(TestAccess::GroupActiveViewport(fixture.shell, focused)),
         "the two panes show one document");
  Expect(TestAccess::GroupActiveViewport(fixture.shell, other).VisibleLineLayoutRef(0).text == "split",
         "the other pane lays out the line as it is");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_NONE), TextInputEvent(" edited")});
  render();
  Expect(TestAccess::GroupActiveViewport(fixture.shell, focused).VisibleLineLayoutRef(0).text == "split edited",
         "the edit lands in the focused pane");
  const std::string shown = TestAccess::GroupActiveViewport(fixture.shell, other).VisibleLineLayoutRef(0).text;
  Expect(shown == "split edited", "the other pane draws the edited line, not '" + shown + "'");
}

void TestTogglingTheMergeBasePaneKeepsTheToolbarAndEveryFrameFresh() {
  FlowFixture fixture("notes.txt", "notes\n");
  WriteFile(fixture.root / "base.txt", "zero\none\ntwo\n");
  WriteFile(fixture.root / "incoming.txt", "zero\none incoming\ntwo\n");
  WriteFile(fixture.root / "current.txt", "zero\none current\ntwo\n");
  WriteFile(fixture.root / "result.txt", "zero\n<<<<<<< HEAD\none current\n=======\none incoming\n>>>>>>> in\ntwo\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto toggle = fixture.ElementRect("button:merge/Toggle Base");
  const auto prev_before = fixture.ElementRect("button:merge/prev");
  Expect(toggle.has_value() && prev_before.has_value(), "the toolbar shows Toggle Base and Previous");
  if (!toggle.has_value()) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*toggle), "Toggle Base (shown)");
  Expect(TestAccess::ActiveMerge(fixture.shell).base_pane_visible, "the base pane is out");
  const auto accept = fixture.ElementRect("button:merge/0/result-incoming");
  const auto toggle_after = fixture.ElementRect("button:merge/Toggle Base");
  Expect(toggle_after.has_value() && accept.has_value() &&
             accept->y > toggle_after->y + toggle_after->h + 3.0f * 14.0f,
         "the panes move down below the band, which sits under the toolbar");
  fixture.ExpectFresh(fixture.Click(*toggle_after), "Toggle Base (hidden)");
}

void TestAMergeToolbarButtonLosesItsHoverWhenThePointerLeaves() {
  FlowFixture fixture("notes.txt", "notes\n", 1440, 600);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale 1.1"), "a fractional ui scale");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  WriteFile(fixture.root / "base.txt", "zero\none\ntwo\n");
  WriteFile(fixture.root / "incoming.txt", "zero\none incoming\ntwo\n");
  WriteFile(fixture.root / "current.txt", "zero\none current\ntwo\n");
  WriteFile(fixture.root / "result.txt", "zero\n<<<<<<< HEAD\none current\n=======\none incoming\n>>>>>>> in\ntwo\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the merge editor");
  const auto toggle = fixture.ElementRect("button:merge/Toggle Base");
  Expect(toggle.has_value(), "the toolbar shows Toggle Base");
  if (!toggle.has_value()) {
    return;
  }
  const auto menu = fixture.ElementRect("menu:Remote");
  Expect(menu.has_value(), "the menu bar shows Remote");
  if (!menu.has_value()) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*menu), "opening the Remote menu");
  fixture.ExpectFresh(fixture.Click(*toggle), "a click on Toggle Base that closes the menu");
  const auto line_ending = fixture.ElementRect("status:line-ending");
  Expect(line_ending.has_value(), "the status bar shows the line ending");
  if (!line_ending.has_value()) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(line_ending->x + line_ending->w / 2.0f,
                                                                      line_ending->y + line_ending->h / 2.0f)}),
                      "the pointer leaving Toggle Base for the line ending's tooltip");
}

void TestACaretMoveThatScrollsTheMergePanesRepaintsThemAll() {
  FlowFixture fixture("notes.txt", "notes\n");
  std::string side;
  for (int i = 1; i <= 80; ++i) {
    side += "line " + std::to_string(i) + "\n";
  }
  WriteFile(fixture.root / "base.txt", side);
  WriteFile(fixture.root / "incoming.txt", "line 0 incoming\n" + side);
  WriteFile(fixture.root / "current.txt", "line 0 current\n" + side);
  WriteFile(fixture.root / "result.txt",
            "<<<<<<< HEAD\nline 0 current\n=======\nline 0 incoming\n>>>>>>> in\n" + side);
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::ActiveMerge(fixture.shell).result_viewport.cursor_line() == 0, "the caret starts on line 1");
  const auto surface = TestAccess::CurrentLayout(fixture.shell).editor_surface;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseWheelEvent(surface.x + 20.0f,
                                                                     surface.y + surface.h / 2.0f, -10.0f)}),
                      "wheeling the merge panes down");
  const int wheeled = TestAccess::ActiveMerge(fixture.shell).scroll_row;
  Expect(wheeled > 0, "the wheel scrolls the panes (row " + std::to_string(wheeled) + ")");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "Down in the result pane");
  Expect(TestAccess::ActiveMerge(fixture.shell).scroll_row != wheeled,
         "Down moves the shared row (the case under test)");
}

void TestTheStatusBarRepaintsWhatChangedWithNoDamageRequest() {
  FlowFixture fixture("notes.txt", "one\ntwo\nthree\nfour\n");
  TestAccess::SetStatusBarSegment(fixture.shell, workspace::StatusBarSegmentId::Remote,
                                  workspace::StatusBarSegmentValue{.text = "worky", .visible = true});
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "a status-bar segment set with no damage request");
}

void TestEditorRowsRepaintWhatChangedWithNoDamageRequest() {
  FlowFixture fixture("rows.txt", "alpha one\nbravo two\ncharlie three\ndelta four\necho five\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  const auto settle = [&] {
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  };
  const auto nothing_announced = [] {
    workspace::RenderInvalidation nothing;
    nothing.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
    return nothing;
  };
  settle();
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(3, 5, true);
  fixture.ExpectFresh(nothing_announced(), "a selection made with no damage request");
  settle();
  viewport.MoveCursorTo(4, 0);
  viewport.InsertText("XYZ ");
  fixture.ExpectFresh(nothing_announced(), "an edit with no damage request");
  settle();
  std::vector<editor::Diagnostic> squiggles{editor::Diagnostic{
      .range = editor::SelectionRange{.start = {.line = 0, .column = 0}, .end = {.line = 0, .column = 5}},
      .severity = editor::DiagnosticSeverity::Error,
      .message = "bad alpha"}};
  (void)TestAccess::CurrentProjectState(fixture.shell)
      .diagnostics_store.ReplaceForOwnerFile("qa", viewport.path(), std::move(squiggles));
  fixture.ExpectFresh(nothing_announced(), "a diagnostic published with no damage request");
}

void TestEditorRowsRecolourWhenACommentOpensAboveThem() {
  FlowFixture fixture("rows.cpp", "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  viewport.MoveCursorTo(1, 0);
  viewport.InsertText("/*");
  workspace::RenderInvalidation nothing;
  nothing.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing, "a block comment opened with no damage request");
}

void TestEditorRowsRepaintTheirIndentGuides() {
  FlowFixture fixture("guides.cpp",
                      "int a;\nvoid run() {\n    if (a) {\n\n        a = 0;\n\n    }\n\n}\nint b;\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  const auto nothing = [] {
    workspace::RenderInvalidation none;
    none.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
    return none;
  };
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  viewport.MoveCursorTo(4, 8);
  fixture.ExpectFresh(nothing(), "the caret moved into the inner block");
  viewport.MoveCursorTo(9, 0);
  fixture.ExpectFresh(nothing(), "the caret moved out of every block");
  viewport.MoveCursorTo(4, 0);
  viewport.InsertText("        ");
  fixture.ExpectFresh(nothing(), "a line beside blank rows indented deeper");
  (void)viewport.ReplaceRange(editor::SelectionRange{.start = {.line = 4, .column = 0}, .end = {.line = 4, .column = 16}},
                              "");
  fixture.ExpectFresh(nothing(), "a line beside blank rows dedented to column 0");
  (void)viewport.ReplaceRange(editor::SelectionRange{.start = {.line = 6, .column = 0}, .end = {.line = 6, .column = 5}},
                              "");
  fixture.ExpectFresh(nothing(), "a block's closing brace removed");
}

void TestCompareRowDamageFollowsEveryStateTransition() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string left;
  std::string right;
  for (int i = 1; i <= 60; ++i) {
    left += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    right += (i % 7 == 0 ? "long value" : "int value") + std::to_string(i) + " = " + std::to_string(i * (i % 5 == 0 ? 2 : 1)) +
             ";\n";
    if (i % 11 == 0) {
      right += "// inserted after " + std::to_string(i) + "\n";
    }
  }
  for (const std::uint32_t seed : {3u, 11u}) {
    FlowFixture fixture("left.cpp", left);
    const std::filesystem::path right_path = fixture.root / "right.cpp";
    WriteFile(right_path, right);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                          "compare-files " + fixture.file.string() + " " + right_path.string()),
           "compare-files should open a diff");
    auto& compare = TestAccess::ActiveCompare(fixture.shell);
    compare.right_view_active = true;
    compare.right_viewport.MoveCursorTo(0, 0);
    TestAccess::SetFocusEditor(fixture.shell);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    std::mt19937 rng(seed);
    const std::string kInserts[] = {"x", "/*", "*/", "\"", " ", "\n", "value"};
    std::string last_op = "opening it";
    for (int step = 0; step < 50; ++step) {
      auto* tab_ptr = TestAccess::ActiveCompareOrNull(fixture.shell);
      Expect(tab_ptr != nullptr, "seed " + std::to_string(seed) + ": the diff is still in front after " + last_op);
      auto& tab = *tab_ptr;
      std::string op;
      const unsigned pick = rng() % 8;
      if (!tab.right_view_active && (pick <= 2 || pick == 7)) {
        tab.right_view_active = true;
        op = "working side active";
      }
      switch (op.empty() ? pick : 99u) {
        case 99u:
          break;
        case 0: {
          const std::string& insert = kInserts[rng() % std::size(kInserts)];
          (void)DispatchEvents(fixture.shell, {insert == "\n" ? KeyDownEvent(SDLK_RETURN) : TextInputEvent(insert.c_str())});
          op = insert == "\n" ? std::string("Return") : "type '" + insert + "'";
          break;
        }
        case 1:
          (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_BACKSPACE)});
          op = "backspace";
          break;
        case 2: {
          const SDL_Keycode keys[] = {SDLK_DOWN, SDLK_UP, SDLK_RIGHT, SDLK_LEFT, SDLK_END, SDLK_HOME};
          const bool extend = rng() % 3 == 0;
          (void)DispatchEvents(fixture.shell,
                               {KeyDownEvent(keys[rng() % std::size(keys)], extend ? SDL_KMOD_SHIFT : SDL_KMOD_NONE)});
          op = extend ? "extend selection" : "move caret";
          break;
        }
        case 3:
          tab.scroll_row = static_cast<int>(rng() % 40);
          op = "scroll to " + std::to_string(tab.scroll_row);
          break;
        case 4:
          tab.horizontal_scroll = rng() % 6;
          op = "horizontal scroll";
          break;
        case 5:
          tab.selected_row = rng() % std::max<std::size_t>(1, TestAccess::ActiveComparePresentationRowCount(fixture.shell));
          op = "select row " + std::to_string(tab.selected_row);
          break;
        case 6:
          tab.right_view_active = !tab.right_view_active;
          op = tab.right_view_active ? "working side active" : "working side inactive";
          break;
        default:
          (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_PAGEDOWN)});
          op = "page down";
          break;
      }
      (void)fixture.shell.ConsumePendingRenderInvalidation();
      workspace::RenderInvalidation nothing;
      nothing.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
      last_op = "step " + std::to_string(step) + " (" + op + ")";
      fixture.ExpectFresh(nothing, "seed " + std::to_string(seed) + " " + last_op);
    }
  }
}

void TestMergeRowDamageFollowsEveryStateTransition() {
  FlowFixture fixture("notes.txt", "notes\n");
  std::string side;
  for (int i = 1; i <= 50; ++i) {
    side += "int line" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(fixture.root / "base.txt", side);
  WriteFile(fixture.root / "incoming.txt", "int head = 1;\n" + side + "int tail = 9;\n");
  WriteFile(fixture.root / "current.txt", "int head = 2;\n" + side + "int tail = 8;\n");
  WriteFile(fixture.root / "result.txt", "<<<<<<< HEAD\nint head = 2;\n=======\nint head = 1;\n>>>>>>> in\n" + side +
                                             "<<<<<<< HEAD\nint tail = 8;\n=======\nint tail = 9;\n>>>>>>> in\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  std::mt19937 rng(5);
  for (int step = 0; step < 60; ++step) {
    auto& merge = TestAccess::ActiveMerge(fixture.shell);
    std::string op;
    switch (rng() % 8) {
      case 0: {
        const char* const kInserts[] = {"x", "/*", "*/", " ", "line"};
        const char* insert = kInserts[rng() % std::size(kInserts)];
        (void)DispatchEvents(fixture.shell, {TextInputEvent(insert)});
        op = std::string("type '") + insert + "'";
        break;
      }
      case 1:
        (void)DispatchEvents(fixture.shell, {KeyDownEvent(rng() % 2 == 0 ? SDLK_RETURN : SDLK_BACKSPACE)});
        op = "return or backspace";
        break;
      case 2: {
        const SDL_Keycode keys[] = {SDLK_DOWN, SDLK_UP, SDLK_RIGHT, SDLK_LEFT, SDLK_END, SDLK_HOME, SDLK_PAGEDOWN};
        const bool extend = rng() % 3 == 0;
        (void)DispatchEvents(fixture.shell,
                             {KeyDownEvent(keys[rng() % std::size(keys)], extend ? SDL_KMOD_SHIFT : SDL_KMOD_NONE)});
        op = extend ? "extend selection" : "move caret";
        break;
      }
      case 3:
        merge.scroll_row = static_cast<int>(rng() % 30);
        op = "scroll to " + std::to_string(merge.scroll_row);
        break;
      case 4:
        merge.selected_hunk = merge.conflicts.empty() ? 0 : rng() % merge.conflicts.size();
        op = "select conflict " + std::to_string(merge.selected_hunk);
        break;
      case 5:
        if (merge.conflicts.empty() || rng() % 3 == 0) {
          merge.hover_state.reset();
          op = "hover off";
        } else {
          const workspace::MergeHoverState::Kind kinds[] = {
              workspace::MergeHoverState::Kind::IncomingConflict, workspace::MergeHoverState::Kind::CurrentConflict,
              workspace::MergeHoverState::Kind::IncomingAccept, workspace::MergeHoverState::Kind::ResultConflict};
          merge.hover_state = workspace::MergeHoverState{.kind = kinds[rng() % std::size(kinds)],
                                                         .conflict_index = rng() % merge.conflicts.size()};
          op = "hover a conflict";
        }
        break;
      case 6:
        merge.horizontal_scroll = rng() % 5;
        op = "horizontal scroll";
        break;
      default:
        merge.base_pane_visible = !merge.base_pane_visible;
        op = merge.base_pane_visible ? "base pane shown" : "base pane hidden";
        break;
    }
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    workspace::RenderInvalidation nothing;
    nothing.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
    fixture.ExpectFresh(nothing, "step " + std::to_string(step) + " (" + op + ")");
  }
}

void TestEditorRowDamageFollowsEveryStateTransition() {
  std::string text;
  const char* const kLines[] = {"#include <vector>", "", "int count = 0;  // a counter", "struct Box {",
                                "  int width = 4;", "  int height = 8;", "};", "const char* name = \"box\";",
                                "for (int i = 0; i < 10; ++i) {", "  count += i;", "}", "/* trailing note */",
                                "void run() {", "    if (count) {", "", "        count = 0;", "",
                                "    }", "", "}"};
  for (int i = 0; i < 4; ++i) {
    for (const char* line : kLines) {
      text += line;
      text += '\n';
    }
  }
  const std::string kInserts[] = {"x", "/*", "*/", "\"", "count", " ", "\t", "{", "}", "\n", "// c"};
  for (const std::uint32_t seed : {1u, 7u, 42u}) {
    FlowFixture fixture("prop.cpp", text);
    editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
    std::mt19937 rng(seed);
    const auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % std::max<std::size_t>(n, 1)); };
    const auto random_position = [&] {
      const std::size_t line = pick(viewport.line_count());
      return editor::TextPosition{.line = line, .column = pick(viewport.lines().LineLength(line) + 1)};
    };
    for (int step = 0; step < 60; ++step) {
      std::string op;
      switch (rng() % 11) {
        case 0: {
          const auto at = random_position();
          viewport.MoveCursorTo(at.line, at.column);
          const std::string& insert = kInserts[pick(std::size(kInserts))];
          viewport.InsertText(insert);
          op = "insert '" + insert + "'";
          break;
        }
        case 1: {
          const auto from = random_position();
          auto to = from;
          to.column = std::min(viewport.lines().LineLength(from.line), from.column + 1 + pick(4));
          (void)viewport.ReplaceRange(editor::SelectionRange{.start = from, .end = to}, "");
          op = "delete on line " + std::to_string(from.line);
          break;
        }
        case 2:
          op = viewport.Undo() ? "undo" : "undo (nothing)";
          break;
        case 3:
          op = viewport.Redo() ? "redo" : "redo (nothing)";
          break;
        case 4: {
          const auto at = random_position();
          viewport.MoveCursorTo(at.line, at.column, rng() % 2 == 0);
          op = "caret/selection to line " + std::to_string(at.line);
          break;
        }
        case 5: {
          const auto at = random_position();
          viewport.AddSecondaryCaret(at.line, at.column);
          op = "secondary caret on line " + std::to_string(at.line);
          break;
        }
        case 6: {
          viewport.SetScrollLine(pick(viewport.line_count()));
          op = "scroll";
          break;
        }
        case 7: {
          std::vector<editor::Diagnostic> squiggles;
          for (std::size_t n = pick(3); n > 0; --n) {
            const auto at = random_position();
            squiggles.push_back(editor::Diagnostic{
                .range = editor::SelectionRange{.start = at, .end = {.line = at.line, .column = at.column + 3}},
                .severity = rng() % 2 == 0 ? editor::DiagnosticSeverity::Error : editor::DiagnosticSeverity::Warning,
                .message = "prop"});
          }
          (void)TestAccess::CurrentProjectState(fixture.shell)
              .diagnostics_store.ReplaceForOwnerFile("prop", viewport.path(), std::move(squiggles));
          op = "diagnostics";
          break;
        }
        case 8:
          viewport.ClearSecondaryCarets();
          op = "clear secondary carets";
          break;
        case 9: {
          const std::size_t line = pick(viewport.line_count());
          viewport.MoveCursorTo(line, 0);
          (void)TestAccess::ExecuteCommandLine(fixture.shell, "toggle-fold");
          op = "toggle fold at line " + std::to_string(line);
          break;
        }
        default: {
          const bool wrap = rng() % 2 == 0;
          (void)TestAccess::ExecuteCommandLine(fixture.shell, wrap ? "wrap on" : "wrap off");
          op = wrap ? "wrap on" : "wrap off";
          break;
        }
      }
      (void)fixture.shell.ConsumePendingRenderInvalidation();
      workspace::RenderInvalidation nothing;
      nothing.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
      fixture.ExpectFresh(nothing, "seed " + std::to_string(seed) + " step " + std::to_string(step) + " (" + op + ")");
    }
  }
}

void TestASettledFrameDerivesNoDamage() {
  FlowFixture fixture("alpha.cpp", "int alpha = 1;\nint beta = alpha + 1;\n",
                      [](const std::filesystem::path& root) { WriteFile(root / "other.cpp", "int alpha = 2;\n"); });
  const auto settles = [&](const std::string& what, int tries) {
    for (int attempt = 0; attempt < tries; ++attempt) {
      fixture.PinCaret();
      fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
      (void)fixture.shell.ConsumePendingRenderInvalidation();
      fixture.PinCaret();
      (void)fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), fixture.width, fixture.height);
      const workspace::RenderInvalidation again = fixture.shell.ConsumePendingRenderInvalidation();
      if (!again.full && again.rects.empty()) {
        return;
      }
      if (attempt + 1 == tries) {
        std::string rects;
        for (const SDL_FRect& r : again.rects) {
          rects += " " + std::to_string(static_cast<int>(r.x)) + "," + std::to_string(static_cast<int>(r.y)) + " " +
                   std::to_string(static_cast<int>(r.w)) + "x" + std::to_string(static_cast<int>(r.h));
        }
        Expect(false, what + ": preparing a settled frame again derived damage" + (again.full ? " (full)" : "") +
                          rects);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  const auto run = [&](const char* command) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command), std::string("the command runs: ") + command);
  };
  settles("the editor with the file tree", 1);
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(60.0f, 120.0f)});
  settles("the pointer resting over the file tree", 1);
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(640.0f, 200.0f)});
  settles("the pointer resting over the editor", 1);
  run("sidebar-show search");
  settles("the Search view", 1);
  run("search alpha");
  settles("the find widget over the editor", 1);
  run("sidebar-show problems");
  settles("the Problems view", 1);
  run("sidebar-show outline");
  settles("the Outline view", 1);
  run("split-right other.cpp");
  settles("two editor groups", 1);
  run("compare-files alpha.cpp other.cpp");
  settles("a compare tab", 1);
  run("settings");
  settles("the Settings card", 1);
  run("settings");
  run("about");
  settles("the Help & About card", 1);
  run("about");
  run("set-setting debug.enabled true");
  run("debug-pane-toggle");
  settles("the debug pane", 1);
  run("term");
  settles("a terminal", 100);
  run("toggle-theme");
  settles("the other theme", 1);
}

void TestTheHoverCardFollowsARepublishedDiagnosticUnderAStillPointer() {
  FlowFixture fixture("hover.cpp", "int alpha = 1;\nint beta = 2;\n");
  const auto publish = [&](std::string message) {
    (void)TestAccess::PublishDiagnostics(
        fixture.shell, "qa", fixture.file,
        {editor::Diagnostic{.range = editor::SelectionRange{.start = {.line = 0, .column = 4},
                                                            .end = {.line = 0, .column = 9}},
                            .severity = editor::DiagnosticSeverity::Warning,
                            .message = std::move(message)}});
  };
  publish("first message");
  fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const editor::EditorViewMetrics metrics = TestAccess::ActiveEditorMetrics(fixture.shell);
  const float x = metrics.text_x + TestAccess::TextCharWidth(fixture.shell) * 6.0f;
  const float y = metrics.first_line_y + metrics.line_height - 1.5f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}), "resting on the squiggle");
  Expect(TestAccess::ActiveEditorDiagnosticHoverMessage(fixture.shell) == std::optional<std::string>("first message"),
         "the card names the diagnostic under the pointer");

  publish("second message");
  TestAccess::RequestPreparedFrame(fixture.shell);
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the diagnostic republished");
  Expect(TestAccess::ActiveEditorDiagnosticHoverMessage(fixture.shell) == std::optional<std::string>("second message"),
         "the card follows the diagnostic republished under a still pointer");
}

void TestADiagnosticInAnotherFileRepaintsNoEditorPane() {
  std::string text;
  for (int i = 0; i < 200; ++i) {
    text += "int line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  FlowFixture fixture("long.cpp", text);
  (void)TestAccess::PublishDiagnostics(
      fixture.shell, "qa", fixture.root / "elsewhere.cpp",
      {editor::Diagnostic{.range = editor::SelectionRange{.start = {.line = 3, .column = 0},
                                                          .end = {.line = 3, .column = 4}},
                          .severity = editor::DiagnosticSeverity::Error,
                          .message = "elsewhere"}});
  TestAccess::RequestPreparedFrame(fixture.shell);
  const workspace::RenderInvalidation damage = FrameDamage(fixture.shell, fixture.canvas, fixture.width,
                                                           fixture.height, fixture.shell.ConsumePendingRenderInvalidation());
  const SDL_FRect editor = TestAccess::CurrentLayout(fixture.shell).editor_surface;
  Expect(!damage.full && !AnyRectIntersects(damage.rects, editor),
         "a diagnostic in a file no pane shows repaints no editor pane");
  (void)TestAccess::PublishDiagnostics(
      fixture.shell, "qa", fixture.file,
      {editor::Diagnostic{.range = editor::SelectionRange{.start = {.line = 150, .column = 0},
                                                          .end = {.line = 150, .column = 4}},
                          .severity = editor::DiagnosticSeverity::Error,
                          .message = "here"}});
  TestAccess::RequestPreparedFrame(fixture.shell);
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "a diagnostic below the viewport");
}

void TestAnImePreeditRepaintsAsItChanges() {
  FlowFixture fixture("ime.cpp", "int alpha = 1;\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search alpha"), "the find widget opens");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the find widget opened");
  const auto preedit = [&](const char* text, int start) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = text;
    event.edit.start = start;
    event.edit.length = 0;
    return DispatchEvents(fixture.shell, {event});
  };
  fixture.ExpectFresh(preedit("ka", 2), "a preedit starting");
  fixture.ExpectFresh(preedit("kat", 3), "the preedit growing");
  fixture.ExpectFresh(preedit("kat", 1), "the preedit's caret moving in");
  fixture.ExpectFresh(preedit("kat", 2), "the preedit's caret moving inside it");
  fixture.ExpectFresh(preedit("", 0), "the preedit ending");
}

void TestThePanelHeaderRepaintsWhatChangedWithNoDamageRequest() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  TestAccess::EnsureTerminalTab(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a second terminal opens");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  auto& tabs = TestAccess::CurrentProjectState(fixture.shell).terminal_tabs;
  Expect(tabs.size() >= 2 && tabs.front() != nullptr, "two terminal tabs, the first in the background");
  tabs.front()->has_unseen_bell = true;
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "a bell marked on a background terminal tab with no damage request");
}

void TestTheOutputRowsRepaintWhatChangedWithNoDamageRequest() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.rows", 60);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::CurrentProjectState(fixture.shell).panel.output.scroll_row = 7;
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "the Output rows scrolled with no damage request");
  Expect(TestAccess::OutputPanelScrollRow(fixture.shell) == 7, "the rows stay where they were scrolled");
}

void TestAnAppendToAHiddenOutputChannelRepaintsNothingInThePanel() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.shown", 5);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::AppendOutputLine(fixture.shell, "qa.hidden", "Hidden", "a line nobody shows");
  (void)fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  const workspace::RenderInvalidation prepared = fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect body = workspace::BottomPanelContentRect(TestAccess::CurrentLayout(fixture.shell));
  Expect(!prepared.full, "a hidden channel's line does not repaint the window");
  for (const SDL_FRect& rect : prepared.rects) {
    Expect(!SDL_HasRectIntersectionFloat(&rect, &body),
           "a hidden channel's line repaints nothing in the Output body (damaged " + std::to_string(rect.x) + "," +
               std::to_string(rect.y) + " " + std::to_string(rect.w) + "x" + std::to_string(rect.h) + ")");
  }
  TestAccess::AppendOutputLine(fixture.shell, "qa.shown", "Test Output", "a sixth line");
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "a line into the shown channel");
}

void TestPartialFramesStayFreshAfterFullFramesAtANewSize() {
  FlowFixture fixture("notes.txt", "one\ntwo\nthree\n");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.width = FlowFixture::kWidth - 120;
  fixture.height = FlowFixture::kHeight - 80;
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "a full frame at the smaller size");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "typing after the resize");
  fixture.width = FlowFixture::kWidth;
  fixture.height = FlowFixture::kHeight;
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "a full frame back at the first size");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "Return after the resize back");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "toggle-theme"), "the colour theme toggles");
  fixture.PinCaret();
  const workspace::RenderInvalidation themed = FrameDamage(fixture.shell, fixture.canvas, fixture.width,
                                                           fixture.height,
                                                           fixture.shell.ConsumePendingRenderInvalidation());
  const bool whole_window =
      themed.full || std::any_of(themed.rects.begin(), themed.rects.end(), [&](const SDL_FRect& rect) {
        return rect.x <= 0.0f && rect.y <= 0.0f && rect.x + rect.w >= static_cast<float>(fixture.width) &&
               rect.y + rect.h >= static_cast<float>(fixture.height);
      });
  Expect(whole_window, "a theme change repaints the whole window");
  fixture.ExpectFresh(themed, "the frame the theme change queued");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("y")}), "typing after the theme change");
}

void TestAGutterThatWidensRepaintsThePaneAboveTheFirstRow() {
  std::string text;
  for (int i = 1; i <= 99; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  text.pop_back();
  FlowFixture fixture("notes.txt", text);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}),
                      "Return adds the hundredth line and widens the gutter");
}

void TestSwitchingThePanelFromAPluginSurfaceToOutputRepaintsTheBody() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.short", 2);
  auto& state = TestAccess::CurrentProjectState(fixture.shell);
  editor::SurfaceContent chart;
  render::PluginDisplayList list;
  list.ops.push_back(render::DisplayOp{.op = render::DrawOp::Rect,
                                       .rect = {10.0f, 30.0f, 200.0f, 120.0f},
                                       .color = SDL_Color{230, 60, 90, 255}});
  list.content_width = 220.0f;
  list.content_height = 160.0f;
  list.content_hash = 0xC4A7;
  chart.body = std::move(list);
  chart.intrinsic_width = 220.0f;
  chart.intrinsic_height = 160.0f;
  state.EnsurePluginPresentation().surfaces.ReplaceForOwnerSurface("plug", "chart", std::move(chart));
  state.panel.content = workspace::PanelContentKind::PluginSurface;
  state.panel.surface_owner = "plug";
  state.panel.surface_id = "chart";
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  state.panel.content = workspace::PanelContentKind::Output;
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "the panel switched from a plugin's chart to Output");
  state.panel.content = workspace::PanelContentKind::PluginSurface;
  fixture.ExpectFresh(nothing_announced, "the panel switched from Output back to the plugin's chart");
}

void TestTheTerminalChromeRepaintsWhatChangedWithNoDamageRequest() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term-split"), "term-split should split the terminal");
  Expect(TestAccess::TerminalPaneCount(fixture.shell) == 2, "the terminal should have two panes");
  const std::size_t active = TestAccess::ActiveTerminalPaneIndex(fixture.shell);
  auto& session = TestAccess::TerminalPaneSession(fixture.shell, active);
  TerminalSessionTestAccess::Reset(session, 8, 40);
  std::string scrollback;
  for (int i = 0; i < 120; ++i) {
    scrollback += "line " + std::to_string(i) + "\r\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, scrollback);
  Expect(session.LineCount() > 100, "the active pane has scrollback, so it shows a scrollbar (the case under test)");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  auto& interaction = TestAccess::InteractionState_(fixture.shell);
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  interaction.drag_target = workspace::DragTarget::TerminalPaneDivider;
  interaction.drag_terminal_pane_boundary = 0;
  fixture.ExpectFresh(nothing_announced, "a pane divider's drag set with no damage request");
  interaction.drag_target = workspace::DragTarget::BottomPanelScrollbar;
  fixture.ExpectFresh(nothing_announced, "the active pane's thumb drag set with no damage request");
  interaction.drag_target = workspace::DragTarget::None;
  fixture.ExpectFresh(nothing_announced, "the drag ended with no damage request");
}

void TestThePanelDividerShowsAndLightsWhileDragged() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.rows", 3);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const microide::render::Theme& theme = TestAccess::PaintTheme(fixture.shell);
  const auto divider_pixel = [&]() {
    const SDL_FRect panel = TestAccess::CurrentLayout(fixture.shell).bottom_panel;
    return fixture.canvas.PixelAt(static_cast<int>(panel.x + panel.w * 0.5f), static_cast<int>(panel.y));
  };
  const auto same = [](SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  Expect(same(divider_pixel(), theme.border), "the panel's top edge is the divider line");
  auto& interaction = TestAccess::InteractionState_(fixture.shell);
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  interaction.drag_target = workspace::DragTarget::BottomPanelDivider;
  fixture.ExpectFresh(nothing_announced, "the panel divider's drag set with no damage request");
  Expect(same(divider_pixel(), theme.accent), "the dragged divider is lit");
  interaction.drag_target = workspace::DragTarget::None;
  fixture.ExpectFresh(nothing_announced, "the panel divider's drag ended with no damage request");
  Expect(same(divider_pixel(), theme.border), "and goes back to the border");
}

void TestTheNotificationToastsRepaintWhatChangedWithNoDamageRequest() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  const auto post = [&](float progress) {
    TestAccess::ShowNotificationRequest(
        fixture.shell, workspace::NotificationService::Request{.key = "qa.progress",
                                                               .message = "Indexing the project",
                                                               .progress = progress});
  };
  post(0.2f);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::ActiveNotifications(fixture.shell).size() == 1, "the progress toast shows");
  post(0.7f);
  workspace::RenderInvalidation nothing_announced;
  nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  fixture.ExpectFresh(nothing_announced, "a progress toast updated with no damage request");
}

void TestAToastAsksForAFrameNotAFullRepaint() {
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  TestAccess::Notify(fixture.shell, workspace::NotificationService::Tone::Info, "Saved notes.txt");
  const workspace::RenderInvalidation posted = fixture.shell.ConsumePendingRenderInvalidation();
  Expect(posted.frame && !posted.full && posted.rects.empty(),
         "posting a toast asks for a frame, naming no damage");
  Expect(!fixture.shell.ConsumePendingRenderInvalidation().HasAnyRedraw(),
         "the frame is owed once: taking it leaves nothing owed (or the idle loop never waits)");
  fixture.ExpectFresh(posted, "the frame a posted toast asked for");
  Expect(TestAccess::ExpireNotificationsAt(fixture.shell, ~std::uint64_t{0} / 2), "the toast expires");
  workspace::RenderInvalidation expired;
  expired.frame = true;
  fixture.ExpectFresh(expired, "the frame an expired toast asked for");
}

void TestANewLineInTheMergeResultRepaintsTheVerticalThumb() {
  FlowFixture fixture("notes.txt", "notes\n");
  std::string side;
  for (int i = 1; i <= 80; ++i) {
    side += "line " + std::to_string(i) + "\n";
  }
  WriteFile(fixture.root / "base.txt", side);
  WriteFile(fixture.root / "incoming.txt", "line 0 incoming\n" + side);
  WriteFile(fixture.root / "current.txt", "line 0 current\n" + side);
  WriteFile(fixture.root / "result.txt",
            "<<<<<<< HEAD\nline 0 current\n=======\nline 0 incoming\n>>>>>>> in\n" + side);
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto thumb_before = TestAccess::ActiveMergeScrollLayout(fixture.shell).vertical_scrollbar;
  Expect(thumb_before.has_value(), "the fixture is long enough for a vertical scrollbar");
  for (int i = 0; i < 3; ++i) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}),
                        "Return in the merge result");
  }
  const auto thumb_after = TestAccess::ActiveMergeScrollLayout(fixture.shell).vertical_scrollbar;
  Expect(thumb_after.has_value() && thumb_after->thumb.h != thumb_before->thumb.h,
         "the new rows shrink the vertical thumb (the case under test)");
  Expect(TestAccess::ActiveMerge(fixture.shell).scroll_row == 0,
         "the panes must NOT scroll, or the surface repaints for that instead");
}

void TestADeeplyIndentedTreeRowKeepsItsLabelInsideTheRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "base.txt", "base\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  std::filesystem::path deep = root;
  for (int i = 0; i < 13; ++i) {
    deep /= "d" + std::to_string(i);
    if (i >= 5) {
      WriteFile(deep / "file.txt", "deep\n");
    }
  }
  for (int i = 0; i < 30; ++i) {
    WriteFile(root / ("z" + std::to_string(i) + ".txt"), "z\n");
  }
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture opens its project");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::SetSidebarWidth(shell, 200.0f);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 38), "the git view lists the untracked files");
  TestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect sidebar = TestAccess::CurrentLayout(shell).sidebar;
  for (float y = sidebar.y + 4.0f; y < sidebar.y + sidebar.h; y += 9.0f) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, {MouseMotionEvent(sidebar.x + 10.0f, y)}));
    Expect(diff.identical(), "hovering the git view at y " + std::to_string(static_cast<int>(y)) +
                                 " paints what a full repaint paints; " + DescribePixelDiff(diff));
  }
}

void TestAConflictOutOfSightHasNoButtons() {
  FlowFixture fixture("notes.txt", "notes\n");
  std::string base;
  for (int i = 1; i <= 90; ++i) {
    base += "line " + std::to_string(i) + "\n";
  }
  const auto with = [&](const std::string& side) {
    std::string text = base;
    for (const int n : {2, 80}) {
      const std::string from = "line " + std::to_string(n) + "\n";
      text.replace(text.find(from), from.size(), "line " + std::to_string(n) + " " + side + "\n");
    }
    return text;
  };
  std::string result = base;
  for (const int n : {80, 2}) {
    const std::string from = "line " + std::to_string(n) + "\n";
    result.replace(result.find(from), from.size(),
                   "<<<<<<< HEAD\nline " + std::to_string(n) + " current\n=======\nline " + std::to_string(n) +
                       " incoming\n>>>>>>> in\n");
  }
  WriteFile(fixture.root / "base.txt", base);
  WriteFile(fixture.root / "incoming.txt", with("incoming"));
  WriteFile(fixture.root / "current.txt", with("current"));
  WriteFile(fixture.root / "result.txt", result);
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::ActiveMerge(fixture.shell).conflicts.size() == 2, "the fixture has two conflicts");

  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  std::vector<std::string> phantom;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string name = entry["name"].AsString();
    if (name.starts_with("button:merge/1/")) {
      phantom.push_back(name);
    }
  }
  Expect(phantom.empty(), "the ui names no button for the conflict out of sight: " +
                              (phantom.empty() ? std::string() : phantom.front()));
  const workspace::ScrollSurfaceLayout scroll = TestAccess::ActiveMergeScrollLayout(fixture.shell);
  Expect(scroll.vertical_scrollbar.has_value(), "a long merge has a vertical scrollbar");
  for (const char* button : {"button:merge/save", "button:merge/Mark Resolved"}) {
    const auto rect = fixture.ElementRect(button);
    Expect(rect.has_value() && scroll.vertical_scrollbar.has_value() &&
               !RectsIntersect(*rect, scroll.vertical_scrollbar->track),
           std::string(button) + " is clear of the scrollbar");
  }
  const auto first_base = fixture.ElementRect("button:merge/0/result-base");
  Expect(first_base.has_value(), "the first conflict's result buttons are on screen");
  if (first_base.has_value()) {
    const float x = first_base->x + 4.0f;
    for (float y = first_base->y - 60.0f; y < first_base->y; y += 4.0f) {
      (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)});
      const auto& hover = TestAccess::ActiveMerge(fixture.shell).hover_state;
      if (hover.has_value() && hover->conflict_index == 1) {
        Expect(false, "hovering above the first conflict targets the one out of sight (y " +
                          std::to_string(static_cast<int>(y)) + ")");
        break;
      }
    }
  }
}

void TestMovingOntoATabsCloseButtonLightsIt() {
  FlowFixture fixture("main.txt", "main\n");
  WriteFile(fixture.root / "other.txt", "other\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "other.txt");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto tab = fixture.ElementRect("tab:0/other.txt");
  const auto close = fixture.ElementRect("button:close-tab/0/other.txt");
  Expect(tab.has_value() && close.has_value(), "the active tab and its close button are named");
  if (!tab.has_value() || !close.has_value()) {
    return;
  }
  const float y = close->y + close->h / 2.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab->x + 20.0f, y)}), "onto the tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(close->x + close->w / 2.0f, y)}),
                      "onto its close button");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab->x + 20.0f, y)}),
                      "back onto the tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab->x + tab->w + 40.0f, y + 200.0f)}),
                      "off the strip");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(close->x + close->w / 2.0f, y)}),
                      "onto the close button from outside the tab");
}

void TestATabStaysInsideItsNarrowStrip() {
  FlowFixture fixture("main.txt", "main\n");
  for (int i = 0; i < 3; ++i) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  }
  TestAccess::SetWindowSize(fixture.shell, 640, 480);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  SoftwareCanvas canvas{640, 480};
  fixture.shell.Render(canvas.renderer(), 640, 480);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  std::vector<SDL_FRect> panes;
  for (const util::JsonValue& editor : ui["editors"].AsArray()) {
    const auto& r = editor["rect"].AsArray();
    panes.push_back(SDL_FRect{static_cast<float>(r[0].AsInt()), 0.0f, static_cast<float>(r[2].AsInt()), 0.0f});
  }
  Expect(panes.size() == 4, "four panes are drawn");
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string name = entry["name"].AsString();
    if (!name.starts_with("tab:")) {
      continue;
    }
    const std::size_t group = static_cast<std::size_t>(name[4] - '0');
    const auto& r = entry["rect"].AsArray();
    const float x = static_cast<float>(r[0].AsInt());
    const float right = x + static_cast<float>(r[2].AsInt());
    if (group < panes.size()) {
      Expect(x >= panes[group].x - 1.0f && right <= panes[group].x + panes[group].w + 1.0f,
             name + " [" + std::to_string(static_cast<int>(x)) + ".." + std::to_string(static_cast<int>(right)) +
                 "] stays inside its pane [" + std::to_string(static_cast<int>(panes[group].x)) + ".." +
                 std::to_string(static_cast<int>(panes[group].x + panes[group].w)) + "]");
    }
  }
}

void TestAnOverlayIsPaintedOverTheBottomPanel() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "alpha\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  for (int i = 0; i < 7; ++i) {
    TestAccess::AddTerminalTab(fixture.shell);
  }
  constexpr int kWidth = 900;
  constexpr int kHeight = 330;
  TestAccess::SetWindowSize(fixture.shell, kWidth, kHeight);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  TestAccess::OpenCommandPalette(fixture.shell);
  SoftwareCanvas canvas{kWidth, kHeight};
  fixture.shell.Render(canvas.renderer(), kWidth, kHeight);
  const SDL_FRect overlay = TestAccess::ActiveOverlayRect(fixture.shell);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& panel_json = ui["surfaces"]["bottomPanel"];
  Expect(panel_json.IsArray(), "the bottom panel is shown");
  if (!panel_json.IsArray()) {
    return;
  }
  const float panel_y = static_cast<float>(panel_json.AsArray()[1].AsInt());
  Expect(overlay.y < panel_y && overlay.y + overlay.h > panel_y + 12.0f,
         "the palette reaches down over the panel (the fixture's precondition): overlay " +
             std::to_string(static_cast<int>(overlay.y)) + ".." +
             std::to_string(static_cast<int>(overlay.y + overlay.h)) + ", panel from " +
             std::to_string(static_cast<int>(panel_y)));
  const int x = static_cast<int>(overlay.x) + 3;
  SDL_FlushRenderer(canvas.renderer());
  const SDL_Color above = canvas.PixelAt(x, static_cast<int>(panel_y) - 4);
  const SDL_Color inside = canvas.PixelAt(x, static_cast<int>(panel_y) + 6);
  Expect(above.r == inside.r && above.g == inside.g && above.b == inside.b,
         "the overlay is painted over the panel: its margin reads the same above the panel's top edge and "
         "below it");
}

void TestClickingAnUnfocusedGroupsOverflowChipRepaintsItsStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a-file-with-a-long-name.txt", "alpha\n");
  for (const char* name : {"b-file-with-a-long-name.txt", "c-file-with-a-long-name.txt",
                           "d-file-with-a-long-name.txt", "e-file-with-a-long-name.txt"}) {
    WriteFile(fixture.root / name, std::string(name) + "\n");
    TestAccess::OpenFile(fixture.shell, fixture.root / name);
  }
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search alpha"), "search should open the find widget");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "two groups, the left one's strip overflowing");
  std::optional<SDL_FRect> chip;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "button:tabs/0/more-left") {
      const auto& r = entry["rect"].AsArray();
      chip = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                       static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(chip.has_value(), "the left group's strip shows a <N chip (the fixture's precondition)");
  if (!chip) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*chip), "a click on the unfocused group's <N chip");
}

void TestATabTooltipNamesTheTabUnderThePointerInASplit() {
  FlowFixture fixture("left-file.txt", "left\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  WriteFile(fixture.root / "right-file.txt", "right\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "right-file.txt");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-other-group"), "focus-other-group should run");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "two groups, the left one focused");
  std::optional<SDL_FRect> right_tab;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "tab:1/right-file.txt") {
      const auto& r = entry["rect"].AsArray();
      right_tab = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                            static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(right_tab.has_value(), "the right group shows right-file.txt's tab");
  if (!right_tab) {
    return;
  }
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(right_tab->x + 12.0f, right_tab->y + right_tab->h / 2.0f)});
  const std::string label = TestAccess::HoveredTooltipLabel(fixture.shell);
  Expect(label.find("right-file.txt") != std::string::npos,
         "hovering the right group's tab names right-file.txt, not \"" + label + "\"");
}

void TestTheUnsavedChangesPromptRepaintsWhenWhatItShowsChanges() {
  FlowFixture fixture("left-file.txt", "left\n", 640, 480);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  WriteFile(fixture.root / "right-file.txt", "right\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "right-file.txt");
  TestAccess::ActiveEditor(fixture.shell).InsertText("x");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "two groups, the right one dirty");

  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)}),
                      "Ctrl+W on the dirty tab");
  Expect(TestAccess::DirtyPromptVisible(fixture.shell), "closing the dirty tab asks first");
  const std::string before = TestAccess::DirtyPromptMessage(fixture.shell);

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-other-group"), "focus-other-group should run");
  Expect(TestAccess::DirtyPromptMessage(fixture.shell) == before,
         "the prompt's message stays with its tab: \"" + TestAccess::DirtyPromptMessage(fixture.shell) + "\"");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(),
                      "the prompt after the focus moved under it");

  TestAccess::SetDirtyPromptSelectedAction(fixture.shell, 2);
  fixture.ExpectFresh(workspace::RenderInvalidation{.frame = true}, "the prompt's selected button moved");
}

void TestTheUnsavedChangesPromptActsOnItsTabWhereverTheFocusMoved() {
  FlowFixture fixture("left-file.txt", "left\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  WriteFile(fixture.root / "right-file.txt", "right\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "right-file.txt");
  TestAccess::ActiveEditor(fixture.shell).InsertText("x");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  Expect(TestAccess::DirtyPromptVisible(fixture.shell), "closing the dirty tab asks first");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-other-group"), "focus-other-group should run");
  const std::string message = TestAccess::DirtyPromptMessage(fixture.shell);
  Expect(message.find("right-file.txt") != std::string::npos,
         "the prompt still names the tab it closes, not \"" + message + "\"");

  TestAccess::SetDirtyPromptSelectedAction(fixture.shell, 1);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  Expect(!TestAccess::DirtyPromptVisible(fixture.shell), "the decision dismisses the prompt");
  bool right_open = false;
  bool left_open = false;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string& name = entry["name"].AsString();
    right_open = right_open || (name.starts_with("tab:") && name.ends_with("/right-file.txt"));
    left_open = left_open || (name.starts_with("tab:") && name.ends_with("/left-file.txt"));
  }
  Expect(!right_open, "Discard closes the tab the prompt was for");
  Expect(left_open, "and nothing else");
}

void TestTypingAtTheEndUnderAStickyBandRepaintsTheTypedRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string body = "int main() {\n";
  for (int i = 0; i < 60; ++i) {
    body += "    int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  FlowFixture fixture("end.cpp", body);
  editor::TextViewport& view = TestAccess::ActiveEditor(fixture.shell);
  view.MoveCursorTo(view.line_count() - 1, 0);
  view.RevealCaret();
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the caret on the last line");
  fixture.ExpectFresh(full, "a second frame");
  Expect(view.sticky_band_rows() > 0, "the function's opener pins above the scrolled body (the precondition): scroll " +
                                          std::to_string(view.scroll_line()) + ", rows " +
                                          std::to_string(view.visible_lines()) + ", lines " +
                                          std::to_string(view.line_count()));
  for (const char* text : {"x", "y", "z"}) {
    const auto redraw = DispatchEvents(fixture.shell, {TextInputEvent(text)});
    const std::size_t typed_scroll = view.scroll_line();
    fixture.ExpectFresh(redraw, std::string("typing ") + text + " on the last line, scroll " +
                                    std::to_string(typed_scroll) + ", band " + std::to_string(view.sticky_band_rows()));
    Expect(view.scroll_line() == typed_scroll,
           "the frame's sizing kept the scroll typing set (" + std::to_string(typed_scroll) + "), not " +
               std::to_string(view.scroll_line()));
    Expect(view.cursor_line() < view.scroll_line() + view.visible_lines(),
           "the caret on the last line stays in view after the frame");
  }
}

void TestTheCaretOnTheLastLineStaysInViewWhereAFoldEndsNearTheEnd() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (std::size_t tail = 1; tail < 30; ++tail) {
    for (std::size_t inner = 1; inner < 4; ++inner) {
      std::string body;
      for (int i = 0; i < 40; ++i) {
        body += "head " + std::to_string(i) + "\n";
      }
      body += "block:\n";
      for (std::size_t i = 0; i < inner; ++i) {
        body += "    inner\n";
      }
      for (std::size_t i = 0; i < tail; ++i) {
        body += "tail " + std::to_string(i) + (i + 1 < tail ? "\n" : "");
      }
      FlowFixture fixture("fold-end.py", body);
      editor::TextViewport& view = TestAccess::ActiveEditor(fixture.shell);
      view.MoveCursorTo(view.line_count() - 1, 0);
      view.RevealCaret();
      workspace::RenderInvalidation full;
      full.full = true;
      fixture.ExpectFresh(full, "the caret on the last line");
      for (const char* text : {"a", "b"}) {
        const std::string what = "typing " + std::string(text) + " with " + std::to_string(tail) + " tail and " +
                                 std::to_string(inner) + " inner lines";
        fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(text)}), what);
        Expect(view.cursor_line() < view.scroll_line() + view.visible_lines(),
               what + " keeps the caret in view: caret " + std::to_string(view.cursor_line()) + ", scroll " +
                   std::to_string(view.scroll_line()) + ", rows " + std::to_string(view.visible_lines()) +
                   ", band " + std::to_string(view.sticky_band_rows()));
      }
    }
  }
}

void TestTypingWhereTheEndLeavesAFoldRepaintsTheTypedRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::size_t rows = 0;
  {
    std::string probe;
    for (int i = 0; i < 200; ++i) {
      probe += "probe " + std::to_string(i) + "\n";
    }
    FlowFixture fixture("probe.txt", probe);
    workspace::RenderInvalidation full;
    full.full = true;
    fixture.ExpectFresh(full, "the probe");
    rows = TestAccess::ActiveEditor(fixture.shell).visible_lines();
  }
  Expect(rows > 4, "the pane shows a few rows (the precondition)");
  if (rows <= 4) {
    return;
  }
  const std::size_t total = rows + 10;
  std::string body;
  for (std::size_t i = 0; i + 1 < total - rows; ++i) {
    body += "head " + std::to_string(i) + "\n";
  }
  body += "block:\n    inner\n";
  for (std::size_t i = 0; i + 1 < rows; ++i) {
    body += "tail " + std::to_string(i) + (i + 2 < rows ? "\n" : "");
  }
  FlowFixture fixture("fold-end.py", body);
  editor::TextViewport& view = TestAccess::ActiveEditor(fixture.shell);
  view.MoveCursorTo(view.line_count() - 2, 0);
  view.RevealCaret();
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the caret at the end");
  fixture.ExpectFresh(full, "a second frame");
  for (const char* text : {"a", "b", "c"}) {
    const auto redraw = DispatchEvents(fixture.shell, {TextInputEvent(text)});
    fixture.ExpectFresh(redraw, std::string("typing ") + text + " at the end, scroll " +
                                    std::to_string(view.scroll_line()) + ", band " +
                                    std::to_string(view.sticky_band_rows()) + ", rows " +
                                    std::to_string(view.visible_lines()) + ", lines " +
                                    std::to_string(view.line_count()));
    Expect(view.cursor_line() < view.scroll_line() + view.visible_lines(),
           std::string("typing ") + text + " keeps the caret's row in the view: caret " +
               std::to_string(view.cursor_line()) + ", scroll " + std::to_string(view.scroll_line()) + ", rows " +
               std::to_string(view.visible_lines()) + ", band " + std::to_string(view.sticky_band_rows()));
  }
}

void TestMovingATabWhoseFileChangedKeepsEveryFrameFresh() {
  FlowFixture fixture("a.txt", "alpha\n");
  std::string text;
  for (int i = 1; i <= 300; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  WriteFile(fixture.root / "long.txt", text);
  WriteFile(fixture.root / "b.txt", "b\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "b.txt");
  TestAccess::OpenFile(fixture.shell, fixture.root / "long.txt");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "goto 250"), "goto 250 runs");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  WriteFile(fixture.root / "long.txt", "churn 1\n" + text);
  for (const char* command : {"tabmove 0", "tabmove 2"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command), std::string(command) + " runs");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), command);
  }
}

void TestMovingATabKeepsEveryFrameFresh() {
  FlowFixture fixture("a.txt", "alpha\n");
  for (const char* name : {"b.txt", "c.txt", "d.txt", "e.txt"}) {
    WriteFile(fixture.root / name, std::string(name) + " body\n");
    TestAccess::OpenFile(fixture.shell, fixture.root / name);
  }
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  for (const char* command : {"tabmove 3", "tabmove 0", "tabmove 4", "tabswitch 1", "tabmove 2"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command), std::string(command) + " runs");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), command);
  }
}

void TestTallMenusStayInsideTheWindow() {
  FlowFixture fixture("menu.txt", "menu\n");
  for (const int height : {720, 480}) {
    TestAccess::SetWindowSize(fixture.shell, FlowFixture::kWidth, height);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    for (const char* menu : {"menu:Edit", "menu:Debug", "menu:Go"}) {
      const auto bar_item = fixture.ElementRect(menu);
      Expect(bar_item.has_value(), std::string(menu) + " should be on the menu bar");
      if (!bar_item) {
        continue;
      }
      (void)fixture.Click(*bar_item);
      const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
      std::string last;
      SDL_FRect last_rect{};
      std::size_t rows = 0;
      for (const util::JsonValue& entry : ui["elements"].AsArray()) {
        if (entry["kind"].AsString() != "menu-item") {
          continue;
        }
        ++rows;
        const auto& r = entry["rect"].AsArray();
        const SDL_FRect rect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                             static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
        Expect(rect.y >= 0.0f && rect.y + rect.h <= static_cast<float>(height) && rect.x >= 0.0f &&
                   rect.x + rect.w <= static_cast<float>(FlowFixture::kWidth),
               std::string(menu) + " at " + std::to_string(height) + " px: " + entry["name"].AsString() +
                   " should be inside the window");
        if (entry["enabled"].IsNull() || entry["enabled"].AsBool()) {
          last = entry["name"].AsString();
          last_rect = rect;
        }
      }
      Expect(rows > 0, std::string(menu) + " should list its rows");
      if (!last.empty()) {
        (void)DispatchEvents(fixture.shell, {MouseMotionEvent(last_rect.x + last_rect.w / 2.0f,
                                                              last_rect.y + last_rect.h / 2.0f)});
        bool active = false;
        const util::JsonValue hovered = TestAccess::ControlUi(fixture.shell);
        for (const util::JsonValue& entry : hovered["elements"].AsArray()) {
          if (entry["name"].AsString() == last) {
            active = entry["active"].AsBool();
          }
        }
        Expect(active, std::string(menu) + " at " + std::to_string(height) + " px: hovering " + last +
                           " should highlight it");
      }
      (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
      (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
    }
  }
}

void TestModalsOwnThePointer() {
  FlowFixture fixture("modal.txt", "modal\n");
  const auto modal = [&]() { return TestAccess::ControlUi(fixture.shell)["modal"]; };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-toggle"), "panel-toggle should run");
  for (const bool from_command : {false, true}) {
    if (from_command) {
      Expect(TestAccess::ExecuteCommandLine(fixture.shell, "keyboard-shortcuts"),
             "keyboard-shortcuts should run");
    } else {
      TestAccess::OpenHelpAboutOverlay(fixture.shell);
    }
    Expect(modal().AsString() == "help", "Help/About should be the modal");
    (void)fixture.Click(SDL_FRect{2.0f, static_cast<float>(FlowFixture::kHeight) - 10.0f, 4.0f, 4.0f});
    Expect(modal().IsNull(), std::string("a click outside Help/About") +
                                 (from_command ? " (opened by keyboard-shortcuts)" : "") +
                                 " should close it, got " + modal().AsString());
  }
  TestAccess::OpenSettingsOverlay(fixture.shell);
  const auto segment = fixture.ElementRect("status:line-column");
  Expect(segment.has_value(), "the status bar should show the caret position");
  if (segment) {
    (void)fixture.Click(*segment);
    Expect(modal().IsNull(), "a click outside Settings should close it, got " + modal().AsString());
  }
  TestAccess::ActiveEditor(fixture.shell).InsertText("x");
  TestAccess::RequestCloseTab(fixture.shell, 0);
  Expect(modal().AsString() == "unsaved-prompt", "closing the dirty tab should ask");
  if (segment) {
    (void)fixture.Click(*segment);
    Expect(modal().AsString() == "unsaved-prompt",
           "a status-bar click under the unsaved prompt must not run its command, got " +
               modal().AsString());
  }
}

void TestHoverTextKeepsItsLines() {
  const workspace::HoverTextMeasure measure{
      .width = [](std::string_view text) { return static_cast<float>(text.size()); },
      .truncate =
          [](std::string_view text, float width) {
            return std::string(text.substr(0, static_cast<std::size_t>(width)));
          },
  };
  const auto lines = workspace::WrapHoverText("field queue_\nprovided by x\n\n\n\nType:   int", 40.0f,
                                              10, measure);
  Expect(lines == std::vector<std::string>{"field queue_", "provided by x", "", "Type: int"},
         "lines should be kept, blank runs folded, spaces collapsed");
  const auto wrapped = workspace::WrapHoverText("one two three four", 9.0f, 10, measure);
  Expect(wrapped == std::vector<std::string>{"one two", "three", "four"}, "words should wrap within a line");
  const auto capped = workspace::WrapHoverText("a\nb\nc\nd", 40.0f, 2, measure);
  Expect(capped == std::vector<std::string>{"a", "b c d"}, "the last line should carry the rest");
}

void TestDebugSessionRepaintsToolbarAndOutput() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "stop"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  const auto wait_for_stop = [&](workspace::RenderInvalidation* damage) {
    return WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); },
                     std::chrono::seconds(10), std::chrono::milliseconds(5), [&]() {
                       TestAccess::ConsumeDapCallbacks(fixture.shell);
                       if (damage != nullptr) {
                         damage->Merge(fixture.shell.ConsumePendingRenderInvalidation());
                       }
                     });
  };
  Expect(wait_for_stop(nullptr), "the mock adapter should stop on configurationDone");
  if (TestAccess::ControlUi(fixture.shell)["surfaces"]["rightPane"].IsNull()) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-toggle"), "the debug pane should open");
  }
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the first stop");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-continue"), "debug-continue should run");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(),
                      "resuming (the toolbar's Continue becomes Pause)");
  workspace::RenderInvalidation stop_damage;
  Expect(wait_for_stop(&stop_damage), "the mock adapter should stop again");
  fixture.ExpectFresh(stop_damage, "stopping again on the same line (Pause becomes Continue)");
}

void TestDebugToolbarDoesNotHoverUnderAModal() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "stop"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); },
                   std::chrono::seconds(10), std::chrono::milliseconds(5),
                   [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); }),
         "the mock adapter should stop on configurationDone");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the first stop");
  const auto restart = fixture.ElementRect("button:debug-toolbar/restart");
  Expect(restart.has_value(), "the toolbar shows Restart");
  if (!restart.has_value()) {
    return;
  }
  TestAccess::OpenPromptSurfaceForTest(fixture.shell,
                                       workspace::PromptSurfaceState::Action::AddWatchExpression,
                                       workspace::PromptSurfaceState::Kind::TextInput, {}, {});
  fixture.ExpectFresh(full, "the Add Watch prompt");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(restart->x + restart->w / 2,
                                                                      restart->y + restart->h / 2)}),
                      "the pointer over Restart under the prompt");
}

void TestTheDebugToolbarCoversNoOtherControl() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "stop"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); },
                   std::chrono::seconds(10), std::chrono::milliseconds(5),
                   [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); }),
         "the mock adapter should stop on configurationDone");
  const auto covered = [&](const char* what) {
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    std::vector<std::pair<std::string, std::array<float, 4>>> toolbar;
    std::vector<std::pair<std::string, std::array<float, 4>>> others;
    for (const util::JsonValue& element : ui["elements"].AsArray()) {
      const std::string name = element["name"].AsString();
      const auto& rect = element["rect"].AsArray();
      const std::string kind = element["kind"].AsString();
      if (rect.size() != 4 || (kind != "button" && kind != "panel-tab" && kind != "tab")) {
        continue;
      }
      const std::array<float, 4> r{static_cast<float>(rect[0].AsDouble()), static_cast<float>(rect[1].AsDouble()),
                                   static_cast<float>(rect[2].AsDouble()), static_cast<float>(rect[3].AsDouble())};
      (name.rfind("button:debug-toolbar/", 0) == 0 ? toolbar : others).emplace_back(name, r);
    }
    for (const auto& [tool, t] : toolbar) {
      for (const auto& [other, o] : others) {
        const bool overlap = t[0] < o[0] + o[2] && o[0] < t[0] + t[2] && t[1] < o[1] + o[3] && o[1] < t[1] + t[3];
        Expect(!overlap, std::string(what) + ": " + tool + " covers " + other);
      }
    }
    return toolbar.size();
  };
  Expect(covered("the first stop") > 0, "the toolbar shows at the stop");

  workspace::SetEditorBanner(TestAccess::CurrentProjectState(fixture.shell),
                             workspace::EditorBannerState::Kind::ExternalChange, fixture.file,
                             "changed on disk");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  Expect(covered("with a banner on the file") > 0, "the toolbar still shows below the banner");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "the panel should maximize");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  covered("with the panel maximized");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "the panel should restore");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);

  TestAccess::ClearEditorBannersForTesting(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-down"), "the editor should split down");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-down"), "and again, into three rows");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "and the last row in two");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const std::size_t groups = TestAccess::EditorGroupCount(fixture.shell);
  Expect(groups == 4, "the fixture builds a four-pane grid");
  for (std::size_t g = 0; g < groups; ++g) {
    TestAccess::FocusEditorGroup(fixture.shell, g);
    const std::string what = "pane " + std::to_string(g) + " of the grid focused";
    covered(what.c_str());
    const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, g);
    if (const auto stop = fixture.ElementRect("button:debug-toolbar/stop"); stop.has_value()) {
      Expect(stop->x >= pane.x && stop->y >= pane.y && stop->x + stop->w <= pane.x + pane.w &&
                 stop->y + stop->h <= pane.y + pane.h,
             what + ": the toolbar floats over the focused pane");
    }
  }

  const std::filesystem::path base = fixture.temp_dir.path() / "base.c";
  const std::filesystem::path incoming = fixture.temp_dir.path() / "incoming.c";
  const std::filesystem::path current = fixture.temp_dir.path() / "current.c";
  WriteFile(base, "int main() { return 0; }\n");
  WriteFile(incoming, "int main() { return 1; }\n");
  WriteFile(current, "int main() { return 2; }\n");
  TestAccess::FocusEditorGroup(fixture.shell, 0);
  Expect(TestAccess::OpenMergeEditor(fixture.shell, base, incoming, current, current), "the merge opens");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  Expect(covered("a merge in the focused pane") > 0, "the toolbar still shows over the merge");

  const std::filesystem::path left = fixture.temp_dir.path() / "left.c";
  const std::filesystem::path right = fixture.temp_dir.path() / "right.c";
  WriteFile(left, "int main() {\n  return 0;\n}\n");
  WriteFile(right, "int main() {\n  return 1;\n}\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "compare-files " + left.string() + " " + right.string()),
         "the compare opens");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  Expect(fixture.ElementRect("button:compare/previous-change").has_value(), "the compare shows its header buttons");
  Expect(covered("a compare in the focused pane") > 0, "the toolbar still shows over the compare");
}

void TestATabActivatedWithNoRequestRepaintsItsStrip() {
  FlowFixture fixture("first.txt", "one\n");
  WriteFile(fixture.root / "second.txt", "two\n");
  TestAccess::OpenFile(fixture.shell, fixture.root / "second.txt");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  auto& group = TestAccess::CurrentProjectState(fixture.shell).editor_groups[0];
  Expect(group.open_tabs.size() == 2 && group.active_tab_index == 1, "two tabs, the second in front");
  group.active_tab_index = 0;
  workspace::RenderInvalidation frame;
  frame.frame = true;
  fixture.ExpectFresh(frame, "the first tab brought to the front with no request");
  const SDL_FRect second = TestAccess::EditorTabRect(fixture.shell, 1);
  TestAccess::SetMousePositionForTest(fixture.shell, second.x + second.w * 0.5f, second.y + second.h * 0.5f);
  fixture.ExpectFresh(frame, "the pointer resting on the second tab");
}

void TestAHoverCardThatSaysSomethingElseRepaints() {
  FlowFixture fixture("notes.txt", "alpha beta gamma\nsecond line\n");
  const auto line1 = TestAccess::ActiveEditorLineRangeRect(fixture.shell, 0, 0);
  Expect(line1.has_value(), "line 1 is on screen");
  if (!line1.has_value()) {
    return;
  }
  const SDL_FRect anchor{line1->x + 60.0f, line1->y, 40.0f, line1->h};
  TestAccess::SetPluginHoverTargetForTest(fixture.shell, anchor, "Scheduler", "provides Scheduler           ");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::SetPluginHoverTargetForTest(fixture.shell, anchor, "Scheduler", "provides Scheduler, Task, run");
  workspace::RenderInvalidation frame;
  frame.frame = true;
  fixture.ExpectFresh(frame, "the hover card's fuller answer, the same size");
}

void TestAPromptsChangesRepaintItFromAFrame() {
  FlowFixture fixture("notes.txt", "one\n");
  TestAccess::PrepareRenamePrompt(fixture.shell, fixture.file, "notes.txt");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the rename prompt open");
  workspace::RenderInvalidation frame;
  frame.frame = true;
  auto& prompt = TestAccess::CurrentPromptSurface(fixture.shell);
  prompt.input.SetText("renamed-notes.txt");
  fixture.ExpectFresh(frame, "the prompt's input text changed with only a frame owed");
  const SDL_FRect dialog = workspace::ComputePromptSurfaceRect(TestAccess::CurrentLayout(fixture.shell).full);
  const std::vector<SDL_FRect> buttons = workspace::ComputePromptSurfaceButtonRects(dialog, prompt.button_count);
  Expect(buttons.size() == 2, "the prompt has two buttons");
  if (buttons.size() != 2) {
    return;
  }
  TestAccess::SetMousePositionForTest(fixture.shell, buttons[1].x + buttons[1].w * 0.5f,
                                      buttons[1].y + buttons[1].h * 0.5f);
  fixture.ExpectFresh(frame, "the pointer resting on the prompt's second button");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "typing into the prompt");
}

void TestEveryButtonOfTheUnsavedChangesPromptLightsUnderThePointer() {
  FlowFixture fixture("notes.txt", "one\n");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("x")});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  Expect(TestAccess::DirtyPromptVisible(fixture.shell), "closing the modified tab asks first");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the unsaved-changes prompt open");
  workspace::RenderInvalidation frame;
  frame.frame = true;
  const auto buttons =
      workspace::ComputeDirtyPromptButtonRects(workspace::ComputeDirtyPromptRect(TestAccess::CurrentLayout(fixture.shell).full));
  const char* names[] = {"Save", "Discard", "Cancel"};
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    TestAccess::SetMousePositionForTest(fixture.shell, buttons[i].x + buttons[i].w * 0.5f,
                                        buttons[i].y + buttons[i].h * 0.5f);
    fixture.ExpectFresh(frame, std::string("the pointer resting on ") + names[i]);
  }
}

void TestFoldMarksFollowAnEditFrameByFrame() {
  std::string text;
  for (int block = 0; block < 12; ++block) {
    text += "void f" + std::to_string(block) + "() {\n  int a = " + std::to_string(block) + ";\n  a += 1;\n}\n\n";
  }
  FlowFixture fixture("blocks.c", text);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "the editor splits");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "two groups");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(11, 0);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the caret in the third block");
  for (const char* typed : {"\n", "\n", "\n", "\t", "{", "\n", "}"}) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(typed)}),
                        std::string("typed ") + (typed[0] == '\n' ? "a newline" : typed));
  }
}

void TestTheCommandPaletteRepaintsWhatChangedFromAFrame() {
  FlowFixture fixture("notes.txt", "one\n");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)}),
                      "the command palette opens");
  const SDL_FRect card = TestAccess::ActiveOverlayRect(fixture.shell);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("sp")}), "the palette filtered");
  for (const float at : {0.55f, 0.65f, 0.75f}) {
    const workspace::RenderInvalidation hover =
        DispatchEvents(fixture.shell, {MouseMotionEvent(card.x + card.w * 0.5f, card.y + card.h * at)});
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, hover);
    float area = 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect overlap{};
      if (SDL_GetRectIntersectionFloat(&rect, &card, &overlap)) {
        area += overlap.w * overlap.h;
      }
    }
    Expect(!damage.full && area < card.w * card.h * 0.4f,
           "a row lit under the pointer repaints rows, not the card (" + std::to_string(area / (card.w * card.h)) +
               " of it)");
    fixture.ExpectFresh(damage, "a palette row under the pointer");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "the selection moved down");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "the palette closed");
}

void TestTheFindWidgetRepaintsItsFieldsFromAFrame() {
  FlowFixture fixture("notes.txt", "alpha beta alpha\ngamma alpha delta\nalpha\n");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)}), "the find widget opens");
  for (const char* typed : {"a", "l", "p"}) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(typed)}), std::string("typed ") + typed);
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "the next match");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_BACKSPACE)}), "a character erased");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_H, SDL_KMOD_CTRL)}), "replace shown");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("Z")}), "typed into the replace field");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_A, SDL_KMOD_CTRL)}),
                      "the replace field selected whole");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)}),
                      "the selection cleared, the caret where it was");
}

void TestTheProblemsListRepaintsItsRowsFromAFrame() {
  FlowFixture fixture("notes.txt", "alpha\nbeta\ngamma\ndelta\n");
  TestAccess::ShowProblemsSidebar(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the Problems view, empty");
  std::vector<editor::Diagnostic> diagnostics;
  for (std::size_t line = 0; line < 3; ++line) {
    diagnostics.push_back(editor::Diagnostic{
        .range = editor::SelectionRange{.start = {.line = line, .column = 0}, .end = {.line = line, .column = 3}},
        .severity = editor::DiagnosticSeverity::Warning,
        .message = "warning " + std::to_string(line)});
  }
  Expect(TestAccess::SetDiagnosticsForTest(fixture.shell, fixture.file, std::move(diagnostics)), "published");
  (void)TestAccess::RefreshProblemsSidebar(fixture.shell);
  workspace::RenderInvalidation frame;
  frame.frame = true;
  fixture.ExpectFresh(frame, "three problems landed with only a frame owed");
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  const SDL_FRect row = TestAccess::ProblemsSidebarRowRect(fixture.shell, 2);
  TestAccess::SetMousePositionForTest(fixture.shell, row.x + row.w * 0.5f, row.y + row.h * 0.5f);
  const workspace::RenderInvalidation hover =
      FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, frame);
  float area = 0.0f;
  for (const SDL_FRect& rect : hover.rects) {
    SDL_FRect overlap{};
    if (SDL_GetRectIntersectionFloat(&rect, &sidebar, &overlap)) {
      area += overlap.w * overlap.h;
    }
  }
  Expect(!hover.full && area < sidebar.w * sidebar.h * 0.2f, "a row under the pointer repaints rows, not the sidebar");
  fixture.ExpectFresh(hover, "the pointer resting on the third problem");
  TestAccess::SetFocusSidebar(fixture.shell);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "the selection stepped down");
}

void TestAStillFrameDamagesNothing() {
  FlowFixture fixture("notes.txt", "alpha beta\ngamma delta\nepsilon\n");
  const auto still = [&](const char* what) {
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    fixture.PinCaret();
    (void)fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    const workspace::RenderInvalidation again = fixture.shell.ConsumePendingRenderInvalidation();
    std::string named;
    for (const SDL_FRect& rect : again.rects) {
      named += " " + std::to_string(static_cast<int>(rect.x)) + "," + std::to_string(static_cast<int>(rect.y)) + " " +
               std::to_string(static_cast<int>(rect.w)) + "x" + std::to_string(static_cast<int>(rect.h));
    }
    Expect(!again.full && again.rects.empty(),
           std::string(what) + ": a still frame damages nothing (damaged" + (again.full ? " the window" : named) + ")");
  };
  still("an editor beside the file tree");
  TestAccess::ShowGitSidebar(fixture.shell);
  still("the git view");
  TestAccess::ShowProblemsSidebar(fixture.shell);
  still("the problems view");
  TestAccess::ShowTreeSidebar(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "the editor splits");
  still("two editor groups");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.still", 30);
  still("the Output panel");
  TestAccess::EnsureTerminalTab(fixture.shell);
  still("the terminal panel");
  TestAccess::OpenMenuBarMenuForTest(fixture.shell, workspace::WorkspaceShell::MenuId::File);
  still("the File menu open");
  WriteFile(fixture.root / "base.txt", "one\ntwo\nthree\n");
  WriteFile(fixture.root / "incoming.txt", "one\nTWO\nthree\n");
  WriteFile(fixture.root / "current.txt", "one\n2\nthree\n");
  WriteFile(fixture.root / "result.txt", "one\n<<<<<<< HEAD\n2\n=======\nTWO\n>>>>>>> in\nthree\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  still("a merge in front");
}

void TestEditorChangesRepaintTheirRowsNotThePane() {
  std::string text;
  for (int i = 1; i <= 80; ++i) {
    text += "entry_" + std::to_string(i) + " = " + std::to_string(i * 7) + "\n";
  }
  FlowFixture fixture("rows.txt", text);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect pane = TestAccess::ActiveEditorPaneRect(fixture.shell);
  const auto rows_only = [&](const workspace::RenderInvalidation& redraw, const char* what) {
    Expect(!redraw.full, std::string(what) + ": not a full repaint");
    float area = 0.0f;
    for (const SDL_FRect& rect : redraw.rects) {
      SDL_FRect overlap{};
      if (SDL_GetRectIntersectionFloat(&rect, &pane, &overlap)) {
        area += overlap.w * overlap.h;
      }
    }
    Expect(area < pane.w * pane.h * 0.2f, std::string(what) + ": repaints rows, not the pane (damaged " +
                                             std::to_string(area) + " of " + std::to_string(pane.w * pane.h) +
                                             " px)");
    fixture.ExpectFresh(redraw, what);
  };
  const auto frame_of = [&](const workspace::RenderInvalidation& redraw) {
    return FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, redraw);
  };
  const auto line5 = TestAccess::ActiveEditorLineRangeRect(fixture.shell, 4, 4);
  Expect(line5.has_value(), "line 5 is on screen");
  if (!line5.has_value()) {
    return;
  }
  const float click_x = pane.x + pane.w * 0.15f;
  const float click_y = line5->y + line5->h * 0.5f;
  rows_only(frame_of(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, click_x, click_y),
                                                    MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, click_x, click_y)})),
            "a click on line 5");
  rows_only(frame_of(DispatchEvents(fixture.shell, {TextInputEvent("x")})), "typing on line 5");
  rows_only(frame_of(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)})), "undo");
  TestAccess::SetClipboardTextReader(fixture.shell, []() { return std::optional<std::string>("pasted"); });
  rows_only(frame_of(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_V, SDL_KMOD_CTRL)})), "a paste");
  std::vector<workspace::LspClient::Diagnostic> diagnostics;
  diagnostics.push_back(workspace::LspClient::Diagnostic{
      .range = {.start = {.line = 11, .character = 0}, .end = {.line = 11, .character = 4}},
      .message = "from the server",
      .severity = 1});
  TestAccess::PublishLspDiagnostics(fixture.shell, fixture.file, std::move(diagnostics));
  rows_only(frame_of(fixture.shell.ConsumePendingRenderInvalidation()), "a diagnostics publish");
  TestAccess::PublishLspDiagnostics(fixture.shell, fixture.file, {});
  rows_only(frame_of(fixture.shell.ConsumePendingRenderInvalidation()), "the diagnostics cleared");
}

void TestAHoveredTerminalContextMenuRowRepaintsWhole() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("menu.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect body = workspace::BottomPanelContentRect(TestAccess::CurrentLayout(fixture.shell));
  const float click_x = body.x + body.w - 20.0f;
  const float click_y = body.y + body.h * 0.5f;
  SDL_Event right = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, click_x, click_y);
  right.button.button = SDL_BUTTON_RIGHT;
  SDL_Event right_up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, click_x, click_y);
  right_up.button.button = SDL_BUTTON_RIGHT;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {right, right_up}), "the terminal's context menu opens");
  const auto row = TestAccess::PopupMenuItemRect(fixture.shell, workspace::WorkspaceShell::MenuId::TerminalContext,
                                                 "Maximize Panel");
  Expect(row.has_value(), "the menu lists Maximize Panel");
  if (!row.has_value()) {
    return;
  }
  for (const float dx : {0.25f, 0.75f, 0.5f}) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(row->x + row->w * dx, row->y + row->h * 0.5f)}),
                        "the pointer over Maximize Panel");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(row->x + row->w * 0.5f, row->y - row->h)}),
                      "the pointer onto the row above");
}

void TestARepublishedCodeLensRepaintsWhatItSays() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\nint x;\n");
  auto& decorations = TestAccess::CurrentProjectState(fixture.shell).EnsurePluginPresentation().decorations;
  const auto publish = [&](const char* text) {
    editor::PluginDecorationData lenses;
    lenses.code_lenses.push_back(editor::CodeLensDecoration{.line = 0, .text = text, .command = "refs.show"});
    Expect(decorations.ReplaceForOwnerFile("lens-test", fixture.file, std::move(lenses)),
           "publishing the lens should change the store");
  };
  publish("greet (0)");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  publish("greet (1)");
  workspace::RenderInvalidation frame;
  frame.frame = true;
  fixture.ExpectFresh(frame, "the lens republished with a new count");
  const auto hint = [&](const char* text) {
    editor::PluginDecorationData data;
    data.inline_texts.push_back(
        editor::InlineTextDecoration{.line = 1, .anchor_column = 5, .text = text, .color = SDL_Color{120, 160, 200, 255}});
    Expect(decorations.ReplaceForOwnerFile("hint-test", fixture.file, std::move(data)),
           "publishing the hint should change the store");
  };
  hint(": int");
  fixture.ExpectFresh(frame, "an inlay hint appears");
  hint(": long");
  fixture.ExpectFresh(frame, "the inlay hint republished with new text");
  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "plugins.code_lens_above", "true"),
         "lenses go above their lines");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the lens moved above its line");
  publish("greet (2)");
  fixture.ExpectFresh(frame, "the lens above its line republished with a new count");
}

void TestACodeLensStaysInsideItsPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\nint x;\n");
  editor::PluginDecorationData lenses;
  lenses.code_lenses.push_back(
      editor::CodeLensDecoration{.line = 0, .text = "greet (0)", .command = "refs.show"});
  Expect(TestAccess::CurrentProjectState(fixture.shell).EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "lens-test", fixture.file, std::move(lenses)),
         "publishing the lens should change the store");
  const auto lens_rects = [&](const char* what) {
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    const auto& pane = ui["editors"][std::size_t{0}]["rect"].AsArray();
    std::size_t count = 0;
    for (const util::JsonValue& element : ui["elements"].AsArray()) {
      if (element["name"].AsString().rfind("button:lens/", 0) != 0) {
        continue;
      }
      ++count;
      const auto& r = element["rect"].AsArray();
      Expect(pane.size() == 4 && r[1].AsDouble() >= pane[1].AsDouble() - 0.5 &&
                 r[1].AsDouble() + r[3].AsDouble() <= pane[1].AsDouble() + pane[3].AsDouble() + 0.5,
             std::string(what) + ": the lens is inside its pane: lens " + util::SerializeJson(element["rect"]) +
                 " pane " + util::SerializeJson(ui["editors"][std::size_t{0}]["rect"]));
    }
    return count;
  };
  Expect(lens_rects("a full editor") == 1, "the lens is listed");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "the panel should maximize");
  lens_rects("the panel maximized");
}

void TestHelpAboutKeepsALongCommandInsideTheCard() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n", 1024, 703);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debugging on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-variables"), "the debug pane opens");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "about"), "Help / About opens");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "Help / About open");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("ensure-new")}),
                      "the filter bringing up a command id wider than the column");
}

void TestASettingsRowValueClickRepaintsTheRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const bool maximized : {false, true}) {
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  if (maximized) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-maximize"), "the panel should maximize");
  }
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "settings"), "settings should open");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the Settings overlay");
  std::vector<std::string> rows;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() == "setting") {
      rows.push_back(entry["name"].AsString().substr(std::string_view("setting:").size()));
    }
  }
  Expect(rows.size() >= 2, "Settings lists at least two rows");
  if (rows.size() < 2) {
    return;
  }
  const auto first = fixture.ElementRect("button:setting/" + rows[0] + "/value");
  const auto second = fixture.ElementRect("button:setting/" + rows[1] + "/value");
  Expect(first.has_value() && second.has_value(), "the first two rows have a value control");
  if (!first.has_value() || !second.has_value()) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(first->x + first->w / 2,
                                                                      first->y + first->h / 2)}),
                      "the pointer over the first row's value");
  fixture.ExpectFresh(fixture.Click(*second), "a click on the second row's value");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the frame after it");
  }
}

void TestExceptionFiltersStayListedAfterAStop() {
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "exception"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  const auto labels = [&]() {
    std::vector<std::string> out;
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["kind"].AsString() == "debug-row") {
        out.push_back(entry["label"].AsString());
      }
    }
    return out;
  };
  const auto lists_raised = [&]() {
    const auto rows = labels();
    return std::any_of(rows.begin(), rows.end(),
                       [](const std::string& row) { return row.starts_with("Raised Exceptions"); });
  };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-breakpoints"), "the Breakpoints view opens");
  Expect(WaitUntil([&]() { return lists_raised(); }, std::chrono::seconds(10), std::chrono::milliseconds(5),
                   [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); }),
         "the session's exception filters are listed");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-stop"), "debug-stop runs");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-breakpoints"),
         "the Breakpoints view opens again");
  Expect(lists_raised(), "after the stop the exception filters are still listed, so they can be turned off");
}

void TestStopWithNoSessionLeavesTheDebugPane() {
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-breakpoints"), "the Breakpoints view opens");
  Expect(!TestAccess::ControlUi(fixture.shell)["surfaces"]["rightPane"].IsNull(), "the debug pane is open");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "debug-stop"),
         "Stop Debugging with no session is refused");
  Expect(!TestAccess::ControlUi(fixture.shell)["surfaces"]["rightPane"].IsNull(),
         "and the debug pane stays open");
}

void TestSidebarViewTabsStayInsideANarrowSidebar() {
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  for (const float width : {46.0f, 60.0f, 80.0f, 120.0f}) {
    TestAccess::SetSidebarWidth(fixture.shell, width);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    const auto& sidebar = ui["surfaces"]["sidebar"].AsArray();
    if (sidebar.size() != 4) {
      continue;
    }
    const float right = static_cast<float>(sidebar[0].AsInt() + sidebar[2].AsInt());
    std::vector<SDL_FRect> header;
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      const std::string name = entry["name"].AsString();
      if (name.starts_with("sidebar-view:") || name == "button:sidebar/more") {
        const auto& r = entry["rect"].AsArray();
        const SDL_FRect rect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                             static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
        Expect(rect.x + rect.w <= right, name + " ends inside a " + std::to_string(static_cast<int>(width)) +
                                             " px sidebar");
        for (const SDL_FRect& other : header) {
          Expect(rect.x >= other.x + other.w || other.x >= rect.x + rect.w,
                 name + " does not overlap another header control at " +
                     std::to_string(static_cast<int>(width)) + " px");
        }
        header.push_back(rect);
      }
    }
  }
}

void TestAnAutomaticGitRefreshInsideTheIntervalIsKept() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  WriteFile(root / "b.txt", "b\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add the pacing fixture", "paced refresh fixture");

  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open");
  TestAccess::SetWindowSize(shell, 1280, 720);
  TestAccess::OnFramePresented(shell);
  TestAccess::ShowTreeSidebar(shell);
  (void)SettleGitSidebarRefresh(shell);

  const auto status_of = [&](const char* name) {
    for (const project::TreeEntry& entry : TestAccess::TreeEntries(shell)) {
      if (entry.path == (root / name).lexically_normal()) {
        return entry.git_status;
      }
    }
    return project::GitFileStatus::Clean;
  };
  const auto wait_for_modified = [&](const char* name) {
    return WaitUntil([&]() { return status_of(name) == project::GitFileStatus::Modified; },
                     kGitSidebarWaitTimeout, std::chrono::milliseconds(10),
                     [&]() { TestAccess::ConsumeGitSidebarRefresh(shell); });
  };

  WriteFile(root / "a.txt", "a changed\n");
  TestAccess::RequestAutomaticGitSidebarRefresh(shell);
  Expect(wait_for_modified("a.txt"), "the first automatic refresh should mark a.txt modified");
  WriteFile(root / "b.txt", "b changed\n");
  TestAccess::RequestAutomaticGitSidebarRefresh(shell);
  const auto settled = [&]() { return TestAccess::ControlUi(shell)["git"]["settled"].AsBool(); };
  Expect(!settled(), "a kept refresh leaves the git view unsettled");
  Expect(wait_for_modified("b.txt"),
         "a refresh asked for inside the interval should still run once it ends");
  Expect(WaitUntil(settled, kGitSidebarWaitTimeout, std::chrono::milliseconds(10),
                   [&]() { TestAccess::ConsumeGitSidebarRefresh(shell); }),
         "and once it has landed, the git view is settled");
}

void TestTheGitPanelHeadRepaintsWhatChangedWithNoDamageRequest() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add the head fixture", "git panel head fixture");
  WriteFile(root / "a.txt", "a changed\n");

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OnFramePresented(shell);
  TestAccess::ShowGitSidebar(shell);
  Expect(SettleGitSidebarRefresh(shell), "the git view's first refresh lands");
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pin = [&]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  workspace::RenderInvalidation full;
  full.full = true;
  (void)PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, full, pin);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto expect_fresh = [&](const std::string& what) {
    workspace::RenderInvalidation nothing_announced;
    nothing_announced.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, nothing_announced, pin);
    Expect(diff.count == 0, what + " should paint what a full repaint paints; " + DescribePixelDiff(diff));
  };
  auto& git = TestAccess::CurrentProjectState(shell).sidebar.git;
  git.refresh_error = "fetch failed: the remote hung up";
  expect_fresh("a refresh error's banner set with no damage request");
  git.refresh_error.clear();
  expect_fresh("a refresh error's banner cleared with no damage request");
  Expect(!git.entries.empty(), "the fixture lists a changed file");
  git.selected_index = git.entries.size();
  expect_fresh("the git selection cleared with no damage request");
  git.selected_index = 0;
  expect_fresh("the git selection set with no damage request");
  git.entries[0].status = project::GitFileStatus::Added;
  ++git.snapshot_generation;
  expect_fresh("a file's status rewritten by a refresh with no damage request");

  auto& workflow = git.commit_workflow;
  workflow.open = true;
  (void)PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, full, pin);
  (void)shell.ConsumePendingRenderInvalidation();
  workflow.status_message = "Committing 1 file…";
  expect_fresh("the draft's status line set with no damage request");
  workflow.checks.push_back(project::CommitPreCheck{.kind = project::CommitPreCheckKind::EmptySubject,
                                                    .severity = project::CommitPreCheckSeverity::Blocking,
                                                    .id = "subject",
                                                    .message = "The subject is empty."});
  expect_fresh("a pre-check added with no damage request");
  workflow.staged_summary_line = "1 file staged";
  expect_fresh("the staged summary rewritten with no damage request");
}

void TestAGitRowClickLandsOnThePaintedRowAfterTheListShrinks() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
    WriteFile(root / name, "one\n");
  }
  InitializeGitRepo(root);
  CommitAll(root, "Add the click fixture", "git click fixture");
  for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
    WriteFile(root / name, "two\n");
  }

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OnFramePresented(shell);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 3), "the git view lists the three changed files");
  const auto index_of = [&](std::string_view name) -> std::optional<std::size_t> {
    const auto& entries = TestAccess::GitSidebarEntries(shell);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].relative_path == name) {
        return i;
      }
    }
    return std::nullopt;
  };
  const SDL_FRect a_row = TestAccess::GitSidebarEntryRowRect(shell, index_of("a.txt").value_or(99));
  const SDL_FRect c_row = TestAccess::GitSidebarEntryRowRect(shell, index_of("c.txt").value_or(99));
  Expect(a_row.w > 0.0f && c_row.w > 0.0f && a_row.y < c_row.y, "a.txt and c.txt have rows, a.txt's above");
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  auto& git = TestAccess::CurrentProjectState(shell).sidebar.git;
  std::erase_if(git.entries, [](const auto& entry) { return entry.relative_path != "c.txt"; });
  Expect(git.entries.size() == 1, "the list shrank to c.txt");
  git.selected_index = git.entries.size();

  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, a_row.x + 30.0f, a_row.y + 4.0f),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, a_row.x + 30.0f, a_row.y + 4.0f)});
  Expect(TestAccess::GitSidebarSelectedIndex(shell) >= git.entries.size(),
         "a click on a row whose file has gone selects nothing (not whatever sits there now)");

  SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, c_row.x + 30.0f, c_row.y + 4.0f);
  SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, c_row.x + 30.0f, c_row.y + 4.0f);
  down.button.button = SDL_BUTTON_RIGHT;
  up.button.button = SDL_BUTTON_RIGHT;
  (void)DispatchEvents(shell, {down, up});
  const std::size_t selected = TestAccess::GitSidebarSelectedIndex(shell);
  Expect(selected < git.entries.size() && git.entries[selected].relative_path == "c.txt",
         "a click on c.txt's painted row selects c.txt, wherever the refresh moved it");
}

void TestTheGitListWheelScrollsTheLiveList() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  constexpr int kFiles = 120;
  for (int i = 0; i < kFiles; ++i) {
    WriteFile(root / ("f" + std::to_string(100 + i) + ".txt"), "one\n");
  }
  InitializeGitRepo(root);
  CommitAll(root, "Add the wheel fixture", "git wheel fixture");
  for (int i = 0; i < kFiles; ++i) {
    WriteFile(root / ("f" + std::to_string(100 + i) + ".txt"), "two\n");
  }

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OnFramePresented(shell);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, kFiles), "the git view lists every changed file");
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto repaint = [&]() {
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  repaint();
  const SDL_FRect row = TestAccess::GitSidebarEntryRowRect(shell, 0);
  Expect(row.w > 0.0f, "the first file has a row");
  TestAccess::SetSidebarScrollRow(shell, 0);
  repaint();
  const auto notch = [&]() {
    (void)DispatchEvents(shell, {MouseWheelEvent(row.x + 30.0f, row.y + 4.0f, -1.0f)});
  };

  notch();
  const int one_step = TestAccess::SidebarScrollRow(shell);
  Expect(one_step > 0, "a notch down scrolls the list");
  TestAccess::SetSidebarScrollRow(shell, 0);
  repaint();

  notch();
  notch();
  Expect(TestAccess::SidebarScrollRow(shell) == 2 * one_step,
         "two notches before a repaint scroll two steps, not one: the wheel reads the live scroll");

  TestAccess::SetSidebarScrollRow(shell, 1 << 20);
  repaint();
  const int painted_end = TestAccess::SidebarScrollRow(shell);
  Expect(painted_end > 0 && painted_end < (1 << 20), "the painted list settled at its end");
  auto& git = TestAccess::CurrentProjectState(shell).sidebar.git;
  const auto grown = git.entries;
  for (auto entry : grown) {
    entry.relative_path = "more/" + entry.relative_path;
    entry.path = (root / entry.relative_path).generic_string();
    git.entries.push_back(entry);
  }
  ++git.snapshot_generation;
  notch();
  Expect(TestAccess::SidebarScrollRow(shell) > painted_end,
         "a notch scrolls on into the rows a refresh added since the frame: the wheel clamps to the live list");
}

void TestTreeGitBadgesRepaintWhenTheGitSelectionSurvives() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  WriteFile(root / "b.txt", "b\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add the badge fixture", "tree badge repaint fixture");
  WriteFile(root / "a.txt", "a changed\n");

  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the badge fixture should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OnFramePresented(shell);
  TestAccess::ShowGitSidebar(shell);
  (void)WaitForGitSidebarEntryCount(shell, 1);
  TestAccess::ShowTreeSidebar(shell);
  TestAccess::NoteLayoutInputsChanged(shell);
  (void)SettleGitSidebarRefresh(shell);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto b_status = [&]() {
    for (const project::TreeEntry& entry : TestAccess::TreeEntries(shell)) {
      if (entry.path == (root / "b.txt").lexically_normal()) {
        return entry.git_status;
      }
    }
    return project::GitFileStatus::Clean;
  };
  Expect(b_status() == project::GitFileStatus::Clean, "b.txt should start clean");

  WriteFile(root / "b.txt", "b changed\n");
  TestAccess::RequestAutomaticGitSidebarRefresh(shell);
  workspace::RenderInvalidation redraw;
  (void)WaitUntil([&]() { return b_status() == project::GitFileStatus::Modified; },
                  kGitSidebarWaitTimeout, std::chrono::milliseconds(10), [&]() {
                    TestAccess::ConsumeGitSidebarRefresh(shell);
                    redraw.Merge(shell.ConsumePendingRenderInvalidation());
                  });
  Expect(b_status() == project::GitFileStatus::Modified, "the refresh should mark b.txt modified");
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, PinCaretOf(shell));
  Expect(diff.identical(), "the tree should repaint the badge the refresh gave b.txt; " +
                               DescribePixelDiff(diff));
}

void TestPaletteAsksForTheArgumentsACommandNeeds() {
  FlowFixture fixture("args.txt", "plain\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
                                       TextInputEvent("set-setting"), KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["modal"].IsString() && ui["modal"].AsString() == "prompt",
         "running Set Setting bare should open a prompt");
  if (ui["prompt"].IsObject()) {
    Expect(ui["prompt"]["title"].AsString() == "Set Setting",
           "the prompt should be titled by the command, got " + ui["prompt"]["title"].AsString());
    Expect(ui["prompt"]["message"].AsString() == "Enter <id> <value>",
           "the prompt should say what to type, got " + ui["prompt"]["message"].AsString());
  }
  (void)DispatchEvents(fixture.shell, {TextInputEvent("editor.tab_size 7"), KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  const auto value = TestAccess::GetSettingValue(fixture.shell, "editor.tab_size");
  Expect(value.has_value() && *value == "7", "the typed arguments should run the command");
  Expect(TestAccess::ControlUi(fixture.shell)["modal"].IsNull(), "and the prompt should close");

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_G, SDL_KMOD_CTRL)});
  ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["prompt"].IsObject() && ui["prompt"]["title"].AsString() == "Go to Line",
         "Ctrl+G should still open Go to Line");
}

void TestPaletteRowWhoseLabelStartsWithACommandRunsTheRow() {
  FlowFixture fixture("rows.txt", "plain\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
                                       TextInputEvent("compare with clipboard"),
                                       KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  const util::JsonValue actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(actions["last"].AsString() == "compare-clipboard",
         "Enter on Compare with Clipboard should run compare-clipboard, ran " +
             actions["last"].AsString() + " from " + actions["source"].AsString());
}

void TestUserDefaultReachesAProjectOpenedForTheFirstTime() {
  TemporaryDirectory temp_dir;
  ScopedAppHomes homes(temp_dir.path() / "state", temp_dir.path() / "config");
  const std::filesystem::path first = temp_dir.path() / "first";
  const std::filesystem::path second = temp_dir.path() / "second";
  WriteFile(first / "a.txt", "a\n");
  WriteFile(second / "b.txt", "b\n");

  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, first, true, false), "the first project should open");
  const std::size_t base = TestAccess::EditorTabSize(shell);
  Expect(TestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(base + 1)),
         "the tab size should take a project override");
  TestAccess::ToggleSettingScope(shell, "editor.tab_size");
  Expect(TestAccess::EditorTabSize(shell) == base + 1, "the first project keeps the value");

  Expect(TestAccess::OpenProjectTab(shell, second, true, false), "the second project should open");
  Expect(TestAccess::GetSettingValue(shell, "editor.tab_size") == std::to_string(base + 1),
         "the second project's setting resolves to the user default");
  Expect(TestAccess::EditorTabSize(shell) == base + 1,
         "and its editor should use it, got " + std::to_string(TestAccess::EditorTabSize(shell)));
}

void TestUserDefaultReachesAnOpenProjectOnSwitch() {
  TemporaryDirectory temp_dir;
  ScopedAppHomes homes(temp_dir.path() / "state", temp_dir.path() / "config");
  const std::filesystem::path first = temp_dir.path() / "first";
  const std::filesystem::path second = temp_dir.path() / "second";
  WriteFile(first / "a.txt", "a\n");
  WriteFile(second / "b.txt", "b\n");

  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, first, true, false), "the first project should open");
  Expect(TestAccess::OpenProjectTab(shell, second, true, false), "the second project should open");
  const std::size_t base = TestAccess::EditorTabSize(shell);
  Expect(TestAccess::SwitchProject(shell, 0), "switch back to the first project");
  Expect(TestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(base + 1)),
         "the tab size should take a project override");
  TestAccess::ToggleSettingScope(shell, "editor.tab_size");

  Expect(TestAccess::SwitchProject(shell, 1), "switch to the second project");
  Expect(TestAccess::EditorTabSize(shell) == base + 1,
         "the open project's editor should follow the new user default, got " +
             std::to_string(TestAccess::EditorTabSize(shell)));
}

void TestHoverCardDoesNotSwallowClicksOnThePalette() {
  FlowFixture fixture("hover.txt", "alpha beta\n");
  Expect(TestAccess::PublishDiagnostics(
             fixture.shell, "diagnostics", fixture.file,
             {editor::Diagnostic{
                 .range = editor::SelectionRange{.start = editor::TextPosition{.line = 0, .column = 1},
                                                 .end = editor::TextPosition{.line = 0, .column = 5}},
                 .severity = editor::DiagnosticSeverity::Warning,
                 .message = "Unexpected token near alpha",
             }}),
         "the fixture should publish a diagnostic");
  const auto metrics = TestAccess::ActiveEditorMetrics(fixture.shell);
  const float x = metrics.text_x + TestAccess::TextCharWidth(fixture.shell) * 2.0f;
  const float y = metrics.first_line_y + metrics.line_height - 1.5f;
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)});
  Expect(TestAccess::ActiveEditorHoverPopupRect(fixture.shell).has_value(),
         "hovering the diagnostic should open its card");

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
                                       TextInputEvent("toggle status bar")});
  Expect(!TestAccess::ActiveEditorHoverPopupRect(fixture.shell).has_value(),
         "the palette should cover the hover card");
  std::optional<SDL_FRect> row;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString().starts_with("overlay-row:Toggle Status Bar")) {
      row = fixture.ElementRect(entry["name"].AsString());
    }
  }
  Expect(row.has_value(), "the palette should list Toggle Status Bar");
  if (row) {
    (void)fixture.Click(*row);
    Expect(TestAccess::ControlUi(fixture.shell)["actions"]["last"].AsString() == "status-bar-toggle",
           "a click on the row should run it, ran " +
               TestAccess::ControlUi(fixture.shell)["actions"]["last"].AsString());
  }
}

void TestCtrlShiftTReopensClosedEditorsNewestFirst() {
  FlowFixture fixture("first.txt", "one\ntwo\nthree\n");
  const std::filesystem::path second = fixture.root / "second.txt";
  WriteFile(second, "alpha\nbeta\ngamma\n");
  Expect(TestAccess::OpenFileInNewTab(fixture.shell, second), "second.txt should open");
  TestAccess::ActiveEditor(fixture.shell).JumpCursorTo(1, 2);
  const auto active_path = [&]() -> std::filesystem::path {
    const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
    const std::size_t active = TestAccess::ActiveTabIndex(fixture.shell);
    return active < paths.size() ? paths[active] : std::filesystem::path{};
  };
  const SDL_Keymod ctrl_shift = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  Expect(active_path().empty(), "both tabs should be closed");

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_T, ctrl_shift)});
  Expect(active_path() == fixture.file,
         "Ctrl+Shift+T should reopen the last closed editor (first.txt)");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_T, ctrl_shift)});
  Expect(active_path() == second,
         "a second Ctrl+Shift+T should reopen second.txt");
  const editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "the caret should be where it was when second.txt closed, got " +
             std::to_string(viewport.cursor_line()) + ":" + std::to_string(viewport.cursor_column()));
  const std::int64_t rejected_before = TestAccess::ControlUi(fixture.shell)["actions"]["dispatched"].AsInt();
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_T, ctrl_shift)});
  const util::JsonValue actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(actions["dispatched"].AsInt() > rejected_before && actions["rejected"].AsBool(),
         "with nothing left to reopen, Ctrl+Shift+T should say so");
}

void TestDebugPaneModeTabHoverRepaints() {
  FlowFixture fixture("hover.txt", "plain\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled on"),
         "the debugger should enable");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-toggle"), "the debug pane should open");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  for (const char* tab : {"debug-tab:callstack", "debug-tab:watch"}) {
    const std::optional<SDL_FRect> rect = fixture.ElementRect(tab);
    Expect(rect.has_value(), std::string("the debug pane should show ") + tab);
    if (!rect.has_value()) {
      return;
    }
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(rect->x + rect->w / 2.0f,
                                                                        rect->y + rect->h / 2.0f)}),
                        std::string("the pointer moving onto ") + tab);
  }
}

void TestGitCommitButtonHoverRepaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add the hover fixture", "git hover fixture");
  WriteFile(root / "a.txt", "a changed\n");
  Expect(RunGitCommand(root, {"add", "a.txt"}) == 0, "staging a.txt should succeed");

  WorkspaceShell shell;
  SoftwareCanvas canvas{FlowFixture::kWidth, FlowFixture::kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the hover fixture should open");
  TestAccess::SetWindowSize(shell, FlowFixture::kWidth, FlowFixture::kHeight);
  TestAccess::ShowGitSidebar(shell);
  (void)WaitForGitSidebarEntryCount(shell, 1);
  TestAccess::NoteLayoutInputsChanged(shell);
  (void)SettleGitSidebarRefresh(shell);
  shell.Render(canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  std::optional<SDL_FRect> commit;
  {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == "button:git/commit") {
        const auto& r = entry["rect"].AsArray();
        commit = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                           static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
      }
    }
  }
  Expect(commit.has_value(), "the git view should show its Commit button");
  if (!commit.has_value()) {
    return;
  }
  {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    std::string commit_label;
    std::string sync_label;
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == "button:git/commit") commit_label = entry["label"].AsString();
      if (entry["name"].AsString() == "button:git/sync") sync_label = entry["label"].AsString();
    }
    Expect(commit_label == "Commit" && sync_label.starts_with("Sync"),
           "the git buttons should carry their labels, got '" + commit_label + "' and '" + sync_label + "'");
  }
  const auto redraw =
      DispatchEvents(shell, {MouseMotionEvent(commit->x + commit->w / 2.0f, commit->y + commit->h / 2.0f)});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, FlowFixture::kWidth, FlowFixture::kHeight, redraw,
                                                PinCaretOf(shell));
  Expect(diff.identical(), "the pointer moving onto Commit should light it up on screen; " +
                               DescribePixelDiff(diff));
}

void TestCtrlFSeedsTheWordUnderTheCaret() {
  FlowFixture fixture("seed.txt", "cat catalog  cat\n");
  const auto query = [&]() {
    return TestAccess::ControlQuery(fixture.shell, "ui")["overlay"]["query"].AsString();
  };
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 6);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  Expect(query() == "catalog", "Ctrl+F on catalog should seed it, got '" + query() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 12);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  Expect(query() == "catalog", "a caret in whitespace should keep the last query, got '" + query() + "'");
}

void TestCtrlHInTheFindWidgetOpensReplace() {
  FlowFixture fixture("replace.txt", "id=12 id=345\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL), TextInputEvent("id")});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_H, SDL_KMOD_CTRL)});
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["overlay"]["mode"].AsString() == "buffer-replace",
         "Ctrl+H in the find widget should show the replace row, mode is " + ui["overlay"]["mode"].AsString());
  Expect(ui["overlay"]["query"].AsString() == "id", "the query should survive, got '" +
                                                        ui["overlay"]["query"].AsString() + "'");
  bool replace_active = false;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "field:find/replace") {
      replace_active = entry["active"].AsBool();
    }
  }
  Expect(replace_active, "the replace field should take the keyboard");
}

void TestShiftAltRightExpandsTheSelection() {
  FlowFixture fixture("expand.cpp", "x = f(alpha);\n");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 8);
  const SDL_Keymod shift_alt = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT);
  const auto selected = [&]() { return TestAccess::ActiveEditor(fixture.shell).SelectedText(); };
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHT, shift_alt)});
  Expect(selected() == "alpha", "Shift+Alt+Right should select the word, got '" + selected() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHT, shift_alt)});
  Expect(selected() == "(alpha)", "again should take the parentheses, got '" + selected() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_LEFT, shift_alt)});
  Expect(selected() == "alpha", "Shift+Alt+Left should shrink back to the word, got '" + selected() + "'");
}

void TestTreeF2RenamesAndDeleteDeletes() {
  FlowFixture fixture("keep.txt", "keep\n");
  WriteFile(fixture.root / "victim.txt", "x\n");
  (void)TestAccess::ExecuteTreeRefresh(fixture.shell);
  Expect(TestAccess::SelectTreePath(fixture.shell, fixture.root / "victim.txt"),
         "the tree should list victim.txt");
  TestAccess::SetFocusSidebar(fixture.shell);
  const auto prompt = [&]() { return TestAccess::ControlUi(fixture.shell)["prompt"]; };
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F2, SDL_KMOD_NONE)});
  Expect(prompt()["input"].AsString() == "victim.txt",
         "F2 should open the rename prompt on the entry, got '" + prompt()["input"].AsString() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  TestAccess::SetFocusSidebar(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DELETE, SDL_KMOD_NONE)});
  Expect(prompt()["kind"].AsString() == "confirm",
         "Delete should ask before deleting, prompt is " + prompt()["kind"].AsString());
  Expect(std::filesystem::exists(fixture.root / "victim.txt"), "nothing is deleted before the answer");
}

void TestQuickOpenPrefixesAndLineSuffix() {
  FlowFixture fixture("first.txt", "one\n");
  std::string lines;
  for (int i = 1; i <= 30; ++i) {
    lines += "line " + std::to_string(i) + "\n";
  }
  WriteFile(fixture.root / "second.txt", lines);
  TestAccess::ForceFileIndexRefreshAndDrain(fixture.shell);
  const SDL_Keymod ctrl = SDL_KMOD_CTRL;
  const auto ui = [&]() { return TestAccess::ControlUi(fixture.shell); };

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl), TextInputEvent("second.txt:12")});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
  const std::size_t active = TestAccess::ActiveTabIndex(fixture.shell);
  Expect(active < paths.size() && paths[active].filename() == "second.txt",
         "second.txt:12 should open second.txt");
  Expect(TestAccess::ActiveEditor(fixture.shell).cursor_line() == 11,
         "at line 12, got " + std::to_string(TestAccess::ActiveEditor(fixture.shell).cursor_line() + 1));

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl), TextInputEvent(">sort")});
  Expect(ui()["overlay"]["mode"].AsString() == "command-palette" && ui()["overlay"]["query"].AsString() == "sort",
         "> should switch to the command palette with 'sort', got " + ui()["overlay"]["mode"].AsString() +
             " '" + ui()["overlay"]["query"].AsString() + "'");
  Expect(ui()["overlay"]["count"].AsInt() > 0, "and list the sort commands");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl), TextInputEvent(":3")});
  Expect(ui()["prompt"]["input"].AsString() == "3", "':' should open Go to Line with '3', got '" +
                                                        ui()["prompt"]["input"].AsString() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  Expect(TestAccess::ActiveEditor(fixture.shell).cursor_line() == 2, "and Enter should go to line 3");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, ctrl)});
  Expect(ui()["overlay"]["query"].AsString().empty(), "the next Ctrl+P should open on an empty query");
}

void TestTypingAfterCtrlEndRepaintsTheLastLine() {
  std::string text = "alpha beta gamma\n";
  for (int i = 0; i < 80; ++i) {
    text += "pad " + std::to_string(i) + "\n";
  }
  FlowFixture fixture("tail.txt", text);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_CTRL)}), "Ctrl+End");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("t")}), "typing on the last line");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("a")}), "typing again");
}

void TestDoubleClickOnEmptyTabStripOpensUntitled() {
  FlowFixture fixture("tabs.txt", "x\n");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto& strip = ui["surfaces"]["tabStrip"].AsArray();
  Expect(strip.size() == 4, "the ui query should give the tab strip");
  if (strip.size() != 4) {
    return;
  }
  const float x = static_cast<float>(strip[0].AsDouble() + strip[2].AsDouble() - 120.0);
  const float y = static_cast<float>(strip[1].AsDouble() + strip[3].AsDouble() / 2.0);
  const std::size_t before = TestAccess::GroupTabPaths(fixture.shell, 0).size();
  SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
  SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
  down.button.clicks = 2;
  up.button.clicks = 2;
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y), down, up});
  Expect(TestAccess::GroupTabPaths(fixture.shell, 0).size() == before + 1,
         "a double-click on the strip's empty space should open an untitled tab");
}

void TestShiftWheelScrollsTheEditorSideways() {
  std::string line;
  for (int i = 0; i < 120; ++i) {
    line += "w" + std::to_string(i) + " ";
  }
  FlowFixture fixture("wide.txt", line + "\nshort\n");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto& pane = ui["editors"].AsArray()[0];
  const float x = static_cast<float>(pane["textX"].AsDouble() + 200.0);
  const float y = static_cast<float>(pane["rows"].AsArray()[0]["y"].AsDouble() + 5.0);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_LSHIFT, SDL_KMOD_LSHIFT), MouseMotionEvent(x, y),
                                       MouseWheelEvent(x, y, -3.0f), KeyUpEvent(SDLK_LSHIFT)});
  Expect(TestAccess::ActiveEditor(fixture.shell).horizontal_scroll() > 0,
         "Shift+wheel down should scroll the line to the right, horizontal scroll is " +
             std::to_string(TestAccess::ActiveEditor(fixture.shell).horizontal_scroll()));
}

void TestCtrlShiftFSeedsTheSelection() {
  FlowFixture fixture("seed.txt", "find quokka here\n");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 5);
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 11, true);
  (void)DispatchEvents(fixture.shell,
                       {KeyDownEvent(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  std::string query;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "field:search/query") {
      query = entry["label"].AsString();
    }
  }
  Expect(query == "quokka", "Ctrl+Shift+F should seed the search with the selection, got '" + query + "'");
}

void TestCtrlKChordSurvivesReleasingCtrl() {
  FlowFixture fixture("chord.txt", "x\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_BACKSLASH, SDL_KMOD_CTRL)});
  Expect(TestAccess::EditorGroupCount(fixture.shell) == 2, "Ctrl+\\ should split");
  const std::size_t before = TestAccess::FocusedGroupIndex(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_K, SDL_KMOD_CTRL),
                                       KeyDownEvent(SDLK_LCTRL, SDL_KMOD_LCTRL),
                                       KeyDownEvent(SDLK_LEFT, SDL_KMOD_CTRL)});
  Expect(before == 1 && TestAccess::FocusedGroupIndex(fixture.shell) == 0,
         "Ctrl+K, Ctrl (again), Ctrl+Left should focus the left group, focused " +
             std::to_string(TestAccess::FocusedGroupIndex(fixture.shell)));
}

void TestColumnSelectionNeverSplitsACharacter() {
  FlowFixture fixture("utf8.txt", "a\xE2\x80\x94" "b\nc\xC3\xA9" "d\n");
  const SDL_Keymod column = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT);
  for (const SDL_Keycode key : {SDLK_RIGHT, SDLK_LEFT}) {
    for (std::size_t start = 0; start <= 4; ++start) {
      TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, start);
      (void)DispatchEvents(fixture.shell, {KeyDownEvent(key, column), KeyDownEvent(SDLK_DOWN, column)});
      const std::string selected = TestAccess::ActiveEditor(fixture.shell).SelectedText();
      for (std::size_t i = 0; i < selected.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(selected[i]);
        std::string what = "a column selection from column ";
        what += std::to_string(start);
        what += " split a character";
        Expect(!(c >= 0x80 && c < 0xC0 && (i == 0 || static_cast<unsigned char>(selected[i - 1]) < 0x80)), what);
      }
      (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
    }
  }
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  const std::string query = TestAccess::ControlUi(fixture.shell)["overlay"]["query"].AsString();
  Expect(query.empty() || static_cast<unsigned char>(query[0]) < 0x80 || static_cast<unsigned char>(query[0]) >= 0xC0,
         "the find query should not start inside a character");
}

void TestDeleteInTheBreakpointsPaneRemovesIt() {
  FlowFixture fixture("bp.cpp", "int a;\nint b;\nint c;\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled on"), "debugger on");
  TestAccess::ToggleBreakpointForTest(fixture.shell, fixture.file, 1);
  TestAccess::FocusDebugPaneBreakpoints(fixture.shell);
  const auto breakpoint_count = [&]() {
    std::size_t count = 0;
    const util::JsonValue files = TestAccess::ControlQuery(fixture.shell, "breakpoints");
    for (const util::JsonValue& file : files.AsArray()) {
      count += file["breakpoints"].AsArray().size();
    }
    return count;
  };
  Expect(breakpoint_count() == 1, "the fixture should have one breakpoint");
  for (int i = 0; i < 3 && breakpoint_count() == 1; ++i) {
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DELETE, SDL_KMOD_NONE)});
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN, SDL_KMOD_NONE)});
  }
  Expect(breakpoint_count() == 0, "Delete on the breakpoint row should remove it");
}

void TestBreakpointCommandsRepaintTheGutterAndThePane() {
  FlowFixture fixture("bp.cpp", "int a;\nint b;\nint c;\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled on"), "debugger on");
  TestAccess::FocusDebugPaneBreakpoints(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the debug pane open");
  const std::string target = fixture.file.string() + " 2";
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "breakpoint-set " + target), "breakpoint-set should run");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "breakpoint-set");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "breakpoint-remove " + target), "breakpoint-remove should run");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "breakpoint-remove");
}

void TestALayoutChangeWithNoDamageOfItsOwnRepaints() {
  FlowFixture fixture("layout.txt", "x\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "layout", 5);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the output panel shown");
  Expect(TestAccess::BottomPanelVisible(fixture.shell), "the panel should be up");
  TestAccess::HideBottomPanel(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN, SDL_KMOD_NONE)}),
                      "the panel gone with only the editor damaged");
}

void TestAutosaveOnFocusChangeCoversEveryWayTheEditorLosesFocus() {
  FlowFixture fixture("focus.txt", "start\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.autosave on_focus_change"),
         "the setting should apply");
  const std::filesystem::path other = fixture.root / "other.txt";
  WriteFile(other, "other\n");
  const auto frame = [&] {
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    TestAccess::SettleFileWrites(fixture.shell);
  };
  frame();
  (void)DispatchEvents(fixture.shell, {TextInputEvent("A")});
  frame();
  Expect(ReadFile(fixture.file) == "start\n", "typing alone should not write the file");
  Expect(TestAccess::OpenFileInNewTab(fixture.shell, other), "other.txt should open");
  frame();
  Expect(ReadFile(fixture.file).starts_with("A"), "opening another file should auto-save focus.txt");

  (void)DispatchEvents(fixture.shell, {TextInputEvent("B")});
  frame();
  TestAccess::SetFocusSidebar(fixture.shell);
  frame();
  Expect(ReadFile(other).starts_with("B"), "moving the keyboard to the sidebar should auto-save other.txt");
}

void TestCtrlShiftSSavesANamedFileAs() {
  FlowFixture fixture("orig.txt", "orig\n");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("X")});
  (void)DispatchEvents(fixture.shell,
                       {KeyDownEvent(SDLK_S, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  const util::JsonValue prompt = TestAccess::ControlUi(fixture.shell)["prompt"];
  Expect(prompt["title"].AsString() == "Save As" && prompt["input"].AsString() == "orig.txt",
         "Ctrl+Shift+S should ask where, starting from orig.txt, got '" + prompt["input"].AsString() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_A, SDL_KMOD_CTRL), TextInputEvent("copy.txt"),
                                       KeyDownEvent(SDLK_RETURN, SDL_KMOD_NONE)});
  Expect(ReadFile(fixture.root / "copy.txt") == "Xorig\n", "the buffer should be written to copy.txt");
  Expect(ReadFile(fixture.file) == "orig\n", "orig.txt should be left as it was on disk");
  const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
  const std::size_t active = TestAccess::ActiveTabIndex(fixture.shell);
  Expect(active < paths.size() && paths[active].filename() == "copy.txt", "the tab should now be copy.txt");
}

void TestDecorationsFollowTheBufferGoingDirtyAndClean() {
  FlowFixture fixture("deco.txt", "first line\nsecond\nthird line\nfourth\n");
  editor::PluginDecorationData data;
  data.inline_texts.push_back(editor::InlineTextDecoration{
      .line = 0, .text = "note on the first line", .color = SDL_Color{224, 169, 109, 255}});
  TestAccess::PublishPluginDecorationsForTest(fixture.shell, "qa", fixture.file, std::move(data));
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the annotation shown");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(3, 0);
  fixture.ExpectFresh(full, "the caret on line four");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("y")}),
                      "typing on line four (the buffer goes dirty)");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}),
                      "undo back to the saved text");
}

void TestCommandRefusalsSayWhatIsWrong() {
  FlowFixture fixture("refuse.txt", "text\n");
  const std::filesystem::path other = fixture.root / "other.txt";
  WriteFile(other, "other\n");
  const auto said = [&](const std::string& line) {
    (void)TestAccess::ExecuteCommandLine(fixture.shell, line);
    return TestAccess::CommandFeedbackText(fixture.shell);
  };
  const auto expect_said = [&](const std::string& line, std::string_view want) {
    const std::string feedback = said(line);
    Expect(feedback.find(want) != std::string::npos,
           line + " should say \"" + std::string(want) + "\", said \"" + feedback + "\"");
  };
  for (int i = 0; i < 8 && TestAccess::ExecuteCommandLine(fixture.shell, "term-close"); ++i) {
  }
  expect_said("term-relaunch", "No terminal to relaunch");

  TestAccess::OpenTerminalViaService(fixture.shell);
  expect_said("term-pane-left", "No terminal pane to the left");
  expect_said("term-pane-right", "No terminal pane to the right");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-toggle"), "panel-toggle should hide the panel");
  Expect(!TestAccess::ControlUi(fixture.shell)["panel"]["visible"].AsBool(), "the panel should be hidden");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "terminal-find"),
         "terminal-find should run with a terminal behind a hidden panel, said \"" +
             TestAccess::CommandFeedbackText(fixture.shell) + "\"");
  const util::JsonValue panel = TestAccess::ControlUi(fixture.shell)["panel"];
  Expect(panel["visible"].AsBool() && panel["content"].AsString() == "terminal",
         "terminal-find should bring the terminal up, panel " + util::SerializeJson(panel));

  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + other.string()),
         "compare-files should open a diff");
  expect_said("find-previous", "find-previous is unavailable");
  expect_said("split-right", "cannot be split");

  TestAccess::ExecuteCloseAllTabs(fixture.shell);
  expect_said("tests-discover", "No active file");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "project-close"), "project-close should run");
  expect_said("compare-clipboard", "No file or buffer selected to compare");
  expect_said("remote-remove-mirror", "usage: remote-remove-mirror <root>");
  expect_said("remote-apply-deletes", "No active project");
  expect_said("tests-discover", "No active project");
}

void TestTheDiffsOwnVerbsAreCommands() {
  FlowFixture fixture("left.txt", "alpha\n  beta\ngamma\n");
  const std::filesystem::path right = fixture.root / "right.txt";
  WriteFile(right, "alpha\nbeta\ngamma changed\n");
  std::string clipboard;
  TestAccess::SetClipboardTextWriter(fixture.shell, [&](std::string_view text) {
    clipboard = std::string(text);
    return true;
  });
  const auto run = [&](const std::string& line) { return TestAccess::ExecuteCommandLine(fixture.shell, line); };
  const auto feedback = [&]() { return TestAccess::CommandFeedbackText(fixture.shell); };

  Expect(!run("diff-copy-path") && feedback().find("Not a diff") != std::string::npos,
         "outside a diff the verbs refuse, said \"" + feedback() + "\"");
  Expect(run("compare-files " + fixture.file.string() + " " + right.string()), "compare-files should open a diff");

  const bool ignoring = TestAccess::ActiveCompare(fixture.shell).build_options.ignore_whitespace;
  Expect(run("diff-ignore-whitespace") &&
             TestAccess::ActiveCompare(fixture.shell).build_options.ignore_whitespace != ignoring,
         "diff-ignore-whitespace should toggle the diff's option");
  const bool showing = TestAccess::ActiveCompare(fixture.shell).show_whitespace;
  Expect(run("diff-show-whitespace") && TestAccess::ActiveCompare(fixture.shell).show_whitespace != showing,
         "diff-show-whitespace should toggle the diff's option");

  Expect(run("diff-copy-path") && clipboard.ends_with("right.txt"),
         "diff-copy-path should copy the diff's path, copied [" + clipboard + "]");
  Expect(run("diff-copy-patch") && clipboard.find("@@") != std::string::npos &&
             clipboard.find("+gamma changed") != std::string::npos,
         "diff-copy-patch should copy a unified diff, copied [" + clipboard + "]");

  TestAccess::ActiveCompare(fixture.shell).selected_row = 0;
  clipboard.clear();
  Expect(!run("diff-copy-hunk") && clipboard.empty() && feedback().find("No change") != std::string::npos,
         "diff-copy-hunk on an unchanged row should refuse, said \"" + feedback() + "\"");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHTBRACKET, SDL_KMOD_ALT)});
  Expect(run("diff-copy-hunk") && clipboard.find("@@") != std::string::npos,
         "diff-copy-hunk on a change should copy it, copied [" + clipboard + "], said \"" + feedback() + "\"");

  Expect(!run("diff-next-file") && feedback().find("not part of a review") != std::string::npos,
         "diff-next-file outside a review should say so, said \"" + feedback() + "\"");
}

void TestTheMergeEditorsColumnsPaintInsideThemselves() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  const std::string common = "#include <scheduler_with_a_long_name.hpp>\n#include <algorithm>\nint a;\nint b;\n";
  FlowFixture fixture("base.txt", common + "two\nthree\n", 1154, 900);
  const std::filesystem::path incoming = fixture.root / "incoming.txt";
  const std::filesystem::path current = fixture.root / "current.txt";
  WriteFile(incoming, common + "two from incoming\nthree\n");
  WriteFile(current, common + "two from current\nthree\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "merge " + fixture.file.string() + " " +
                                                           incoming.string() + " " + current.string()),
         "merge should open the merge editor");
  TestAccess::SetFocusEditor(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the merge editor");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.font_size 32"), "a large font");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.wrap word"), "word wrap");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the merge editor at a large font");
  for (const int width : {800}) {
    fixture.width = width;
    TestAccess::SetWindowSize(fixture.shell, width, 900);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the merge editor resized");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(";")}), "a first edit");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_HOME)}), "the caret to the line's start");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("int ")}),
                      "typing in the result column, whose first line runs past its edge");
}

void TestTheMergeEditorCopiesAConflictSide() {
  FlowFixture fixture("base.txt", "one\ntwo\nthree\n");
  const std::filesystem::path incoming = fixture.root / "incoming.txt";
  const std::filesystem::path current = fixture.root / "current.txt";
  WriteFile(incoming, "one\ntwo from incoming\nthree\n");
  WriteFile(current, "one\ntwo from current\nthree\n");
  std::string clipboard;
  TestAccess::SetClipboardTextWriter(fixture.shell, [&](std::string_view text) {
    clipboard = std::string(text);
    return true;
  });
  const auto run = [&](const std::string& line) { return TestAccess::ExecuteCommandLine(fixture.shell, line); };
  const auto feedback = [&]() { return TestAccess::CommandFeedbackText(fixture.shell); };

  Expect(!run("merge-copy-remote") && feedback().find("Not a merge") != std::string::npos,
         "outside a merge it refuses, said \"" + feedback() + "\"");
  Expect(run("merge " + fixture.file.string() + " " + incoming.string() + " " + current.string()),
         "merge should open the merge editor, said \"" + feedback() + "\"");
  Expect(run("merge-copy-remote") && clipboard.find("two from incoming") != std::string::npos &&
             clipboard.find("current") == std::string::npos,
         "merge-copy-remote should copy the incoming side, copied [" + clipboard + "]");
  Expect(run("merge-copy-local") && clipboard.find("two from current") != std::string::npos,
         "merge-copy-local should copy the current side, copied [" + clipboard + "]");
}

void TestColorschemeListSaysWhichSchemesThereAre() {
  FlowFixture fixture("theme.txt", "x\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "colorscheme list"), "colorscheme list should run");
  const std::string feedback = TestAccess::CommandFeedbackText(fixture.shell);
  Expect(feedback.find("default") != std::string::npos && feedback.find("light") != std::string::npos,
         "colorscheme list should name the schemes, said '" + feedback + "'");
}

void TestTypingOnTheDiffsWorkingSideNeverRunsReviewLetters() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "diff.txt";
  WriteFile(file, "alpha\nbeta\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add the diff fixture", "diff fixture");
  WriteFile(file, "alpha\nbeta changed\n");
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the diff fixture should open");
  TestAccess::SetWindowSize(shell, FlowFixture::kWidth, FlowFixture::kHeight);
  Expect(TestAccess::OpenWorkingTreeComparison(shell, file, "HEAD", "HEAD"), "the diff should open");
  auto& compare = TestAccess::ActiveCompare(shell);
  Expect(compare.right_editable, "the working side should be editable");
  Expect(compare.review_header.summary_line.starts_with("Working tree review"),
         "a working-tree diff should open as one, header '" + compare.review_header.summary_line + "'");
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(1, 12);
  TestAccess::SetFocusEditor(shell);
  for (const char letter : std::string("adjkoc")) {
    const std::string typed(1, letter);
    (void)DispatchEvents(shell, {KeyDownEvent(static_cast<SDL_Keycode>(letter), SDL_KMOD_NONE),
                                 TextInputEvent(typed.c_str())});
  }
  Expect(TestAccess::ControlUi(shell)["prompt"].IsNull(), "typing must not open a review prompt");
  const std::string text = TestAccess::ControlQuery(shell, "buffer")["text"].AsString();
  Expect(text.find("beta changedadjkoc") != std::string::npos,
         "every typed letter should land in the working side, got '" + text + "'");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_S, SDL_KMOD_CTRL)});
  for (int i = 0; i < 100 && ReadFile(file).find("adjkoc") == std::string::npos; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)DispatchEvents(shell, {});
  }
  Expect(ReadFile(file).find("beta changedadjkoc") != std::string::npos,
         "Ctrl+S in the diff should write the working side, disk has '" + ReadFile(file) + "'");
}

void TestCompareHunkStepRepaintsTheSelectionMarkers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "diff.txt";
  std::string text;
  for (int i = 1; i <= 60; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  WriteFile(file, text);
  InitializeGitRepo(root);
  CommitAll(root, "Add the diff fixture", "diff fixture");
  std::string changed = text;
  changed.replace(changed.find("line 5\n"), 7, "line five\n");
  changed.replace(changed.find("line 40\n"), 8, "line forty\n");
  WriteFile(file, changed);
  WorkspaceShell shell;
  SoftwareCanvas canvas{FlowFixture::kWidth, FlowFixture::kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the diff fixture should open");
  TestAccess::SetWindowSize(shell, FlowFixture::kWidth, FlowFixture::kHeight);
  Expect(TestAccess::OpenWorkingTreeComparison(shell, file, "HEAD", "HEAD"), "the diff should open");
  TestAccess::SetFocusEditor(shell);
  shell.Render(canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  for (int step = 0; step < 3; ++step) {
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, FlowFixture::kWidth, FlowFixture::kHeight,
        DispatchEvents(shell, {KeyDownEvent(SDLK_RIGHTBRACKET, SDL_KMOD_ALT)}), PinCaretOf(shell));
    Expect(diff.identical(), "Alt+] should repaint the old and new change's markers; " + DescribePixelDiff(diff));
  }
}

void TestCompareHeaderRepaintsWhenAnEditChangesIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "diff.txt";
  std::string text;
  for (int i = 1; i <= 30; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  WriteFile(file, text);
  InitializeGitRepo(root);
  CommitAll(root, "Add the diff fixture", "diff fixture");
  std::string changed = text;
  changed.replace(changed.find("line 5\n"), 7, "line five\n");
  WriteFile(file, changed);
  WorkspaceShell shell;
  SoftwareCanvas canvas{FlowFixture::kWidth, FlowFixture::kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the diff fixture should open");
  TestAccess::SetWindowSize(shell, FlowFixture::kWidth, FlowFixture::kHeight);
  Expect(TestAccess::OpenWorkingTreeComparison(shell, file, "HEAD", "HEAD"), "the diff should open");
  TestAccess::SetFocusEditor(shell);
  auto& compare = TestAccess::ActiveCompare(shell);
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(19, 7);
  shell.Render(canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::string before = compare.review_header.summary_line;
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, FlowFixture::kWidth, FlowFixture::kHeight,
                                                DispatchEvents(shell, {TextInputEvent("X")}), PinCaretOf(shell));
  Expect(TestAccess::ActiveCompare(shell).review_header.summary_line != before,
         "the edit should add a hunk to the header");
  Expect(diff.identical(), "the header should repaint with its new hunk count; " + DescribePixelDiff(diff));
}

void TestAltLeftRightNavigateBackAndForward() {
  std::string many_lines;
  for (int i = 0; i < 60; ++i) {
    many_lines += "line " + std::to_string(i) + "\n";
  }
  FlowFixture fixture("long.txt", many_lines);
  const std::filesystem::path other = fixture.root / "other.txt";
  WriteFile(other, "alpha\nbeta\ngamma\ndelta\n");
  const auto frame = [&] {
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  };
  const auto key = [&](SDL_Keycode code) {
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(code, SDL_KMOD_ALT)});
    frame();
  };
  const auto where = [&]() -> std::string {
    const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
    const std::size_t active = TestAccess::ActiveTabIndex(fixture.shell);
    if (active >= paths.size()) {
      return "no tab";
    }
    return paths[active].filename().string() + ":" +
           std::to_string(TestAccess::ActiveEditor(fixture.shell).cursor_line());
  };
  frame();
  TestAccess::ActiveEditor(fixture.shell).JumpCursorTo(40, 0);
  frame();
  Expect(TestAccess::OpenFileInNewTab(fixture.shell, other), "other.txt should open");
  TestAccess::ActiveEditor(fixture.shell).JumpCursorTo(2, 0);
  frame();

  key(SDLK_LEFT);
  Expect(where() == "long.txt:40", "Alt+Left should go back to long.txt:40, got " + where());
  key(SDLK_LEFT);
  Expect(where() == "long.txt:0", "Alt+Left again should go back to long.txt:0, got " + where());
  key(SDLK_LEFT);
  Expect(where() == "long.txt:0", "with nothing behind, Alt+Left should stay put, got " + where());
  key(SDLK_RIGHT);
  Expect(where() == "long.txt:40", "Alt+Right should go forward to long.txt:40, got " + where());

  const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (paths[i] == other) {
      TestAccess::CloseTab(fixture.shell, i);
      break;
    }
  }
  frame();
  key(SDLK_RIGHT);
  Expect(where() == "other.txt:2", "Alt+Right should reopen other.txt at line 2, got " + where());

  key(SDLK_LEFT);
  Expect(where() == "long.txt:40", "back from other.txt should land on long.txt:40, got " + where());
  TestAccess::ActiveEditor(fixture.shell).JumpCursorTo(10, 0);
  frame();
  key(SDLK_RIGHT);
  Expect(where() == "long.txt:10",
         "after a new jump there should be nothing to go forward to, got " + where());
  key(SDLK_LEFT);
  Expect(where() == "long.txt:40", "back from the new place should land on long.txt:40, got " + where());
}

void TestF1AndCtrlNumberFocusGroups() {
  FlowFixture fixture("groups.txt", "plain\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F1, SDL_KMOD_NONE)});
  Expect(TestAccess::ControlUi(fixture.shell)["overlay"]["mode"].AsString() == "command-palette",
         "F1 should open the command palette");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_2, SDL_KMOD_CTRL)});
  Expect(TestAccess::EditorGroupCount(fixture.shell) == 2 &&
             TestAccess::FocusedGroupIndex(fixture.shell) == 1,
         "Ctrl+2 from one group should split right and focus the new group");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_1, SDL_KMOD_CTRL)});
  Expect(TestAccess::FocusedGroupIndex(fixture.shell) == 0, "Ctrl+1 should focus the first group");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_2, SDL_KMOD_CTRL)});
  Expect(TestAccess::EditorGroupCount(fixture.shell) == 2 &&
             TestAccess::FocusedGroupIndex(fixture.shell) == 1,
         "Ctrl+2 with two groups should focus the second, not split again");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_5, SDL_KMOD_CTRL)});
  const util::JsonValue actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(TestAccess::EditorGroupCount(fixture.shell) == 2 && actions["rejected"].AsBool(),
         "Ctrl+5 with two groups should be refused, not split");
}

void TestOutputTabRepaintsWhenItsChannelGetsALabel() {
  FlowFixture fixture("notes.txt", "notes\n");
  TestAccess::SetOutputChannelTabs(fixture.shell, {"qa.late"}, "qa.late");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::AppendOutputLine(fixture.shell, "qa.late", "Late Results", "a result");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)}),
                      "the frame after the channel's first line");
}

void TestTypingOffAWordRepaintsItsOccurrences() {
  FlowFixture fixture("words.txt", "greet one\nfiller\ngreet\n");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(2, 5);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(" ")}), "typing a space after the word");
}

void TestTypingRepaintsOnlyTheLinesWhoseOccurrencesChanged() {
  std::string text = "greet one\n";
  for (int i = 0; i < 30; ++i) {
    text += i == 20 ? "greet\n" : "filler\n";
  }
  FlowFixture fixture("words.txt", text);
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 5);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
  const auto prepared_damage_height = [&](const workspace::RenderInvalidation& redraw) {
    (void)fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    workspace::RenderInvalidation all = redraw;
    all.Merge(fixture.shell.ConsumePendingRenderInvalidation());
    float top = 1e9f;
    float bottom = -1e9f;
    for (const SDL_FRect& rect : all.rects) {
      const float y0 = std::max(rect.y, pane.y);
      const float y1 = std::min(rect.y + rect.h, pane.y + pane.h);
      if (y1 <= y0 || rect.x + rect.w <= pane.x || rect.x >= pane.x + pane.w) {
        continue;
      }
      top = std::min(top, y0);
      bottom = std::max(bottom, y1);
    }
    return std::pair<bool, float>{all.full, bottom - top};
  };
  const auto [full_off, span_off] = prepared_damage_height(DispatchEvents(fixture.shell, {TextInputEvent(" ")}));
  Expect(!full_off && span_off < pane.h * 0.9f && span_off > pane.h * 0.3f,
         "typing off the word repaints its occurrence lines, not the pane: span " + std::to_string(span_off) +
             " of " + std::to_string(pane.h));
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto [full_on, span_on] = prepared_damage_height(DispatchEvents(fixture.shell, {TextInputEvent("x")}));
  Expect(!full_on && span_on < pane.h * 0.2f,
         "typing with nothing highlighted repaints the edited line, span " + std::to_string(span_on));
}

void TestTypingPastTheViewsEdgeRepaintsTheBarNotThePane() {
  std::string text = std::string(400, 'w') + "\nshort\nshort\n";
  FlowFixture fixture("wide.txt", text);
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(0, 400);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
  const workspace::RenderInvalidation redraw = DispatchEvents(fixture.shell, {TextInputEvent("x")});
  float covered = 0.0f;
  for (const SDL_FRect& rect : redraw.rects) {
    const float y0 = std::max(rect.y, pane.y);
    const float y1 = std::min(rect.y + rect.h, pane.y + pane.h);
    covered += std::max(0.0f, y1 - y0);
  }
  Expect(!redraw.full && covered < pane.h * 0.5f,
         "a keystroke past the edge repaints its row and the bar, " + std::to_string(covered) + " of " +
             std::to_string(pane.h));
  fixture.ExpectFresh(redraw, "typing past the view's edge");
}

void TestHoverUnderAStickyBandResolvesThePointersLine() {
  std::string body = "int outer() {\n";
  for (int i = 0; i < 80; ++i) {
    body += "  int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  body += "}\n";
  FlowFixture fixture("sticky.cpp", body);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_PAGEDOWN), KeyDownEvent(SDLK_PAGEDOWN)});
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"][std::size_t{0}];
  Expect(pane["stickyRows"].AsInt() >= 1, "the fixture should show a sticky band");
  const util::JsonValue& rows = pane["rows"];
  Expect(rows.AsArray().size() > 4, "the pane should show rows");
  const std::int64_t line = rows[std::size_t{3}]["line"].AsInt();
  const float x = static_cast<float>(pane["textX"].AsDouble() + 6.5 * pane["charWidth"].AsDouble());
  const float y = static_cast<float>(pane["firstLineY"].AsDouble() + 3.5 * pane["lineHeight"].AsDouble());
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)});
  Expect(TestAccess::HoverCacheLineForTesting(fixture.shell) == static_cast<std::size_t>(line),
         "the hover should ask about line " + std::to_string(line) + ", the one under the pointer, not " +
             std::to_string(TestAccess::HoverCacheLineForTesting(fixture.shell)));
}

void TestTypingUnderAStickyBandRepaintsTheTypedRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string body = "namespace outer {\nstruct Inner {\n";
  for (int i = 0; i < 80; ++i) {
    body += "  int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  body += "};\n}\n";
  FlowFixture fixture("sticky.cpp", body);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_PAGEDOWN), KeyDownEvent(SDLK_PAGEDOWN)});
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "scrolling under the headers");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"][std::size_t{0}];
  Expect(pane["stickyRows"].AsInt() >= 2, "the fixture should pin two headers");
  const std::int64_t line = pane["rows"][std::size_t{4}]["line"].AsInt();
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(static_cast<std::size_t>(line - 1), 2);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the caret on a row under the band");
  const auto ime = TestAccess::ActiveTextInputArea(fixture.shell);
  const double row_y = pane["rows"][std::size_t{4}]["y"].AsDouble();
  Expect(ime.has_value() && std::fabs((ime->y + 1.0) - row_y) < 0.5,
         "the text-input area should sit on the caret's row (" + std::to_string(row_y) + "), not at " +
             (ime.has_value() ? std::to_string(ime->y + 1.0) : std::string("nothing")));
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("Q")}),
                      "typing on line " + std::to_string(line) + " under the sticky band");
}

void TestACommitFieldUnderAModalPromptTakesNoClickAndPaintsNoCaret() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "main.cpp", "int main() { return 0; }\n");
  InitializeGitRepo(root);
  CommitAll(root, "seed commit", "seed body");
  WriteFile(root / "main.cpp", "int main() { return 1; }\n");
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the repo should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1), "the repo should show its modified file");
  Expect(TestAccess::OpenCommitWorkflow(shell), "the commit draft should open");
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::CurrentTextInputSurface(shell) == workspace::TextInputSurface::CommitSubject,
         "the subject should hold the keyboard");
  const SDL_FRect body = TestAccess::CommitWorkflow(shell).body_field_rect;
  Expect(body.w > 0.0f, "the body field should be laid out");

  TestAccess::OpenPromptSurfaceForTest(shell, workspace::PromptSurfaceState::Action::CreateFile,
                                       workspace::PromptSurfaceState::Kind::TextInput, root, "name");
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::CurrentTextInputSurface(shell) == workspace::TextInputSurface::PromptInput,
         "the prompt should hold the keyboard");
  const float x = body.x + 20.0f;
  const float y = body.y + body.h * 0.5f;
  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  Expect(TestAccess::CommitWorkflow(shell).focus_field == workspace::CommitWorkflowFocusField::Subject,
         "a click on the body behind the modal prompt should not focus the body");

  TestAccess::CommitWorkflow(shell).focus_field = workspace::CommitWorkflowFocusField::Body;
  TestAccess::SetFocusSidebar(shell);
  const Uint64 now = SDL_GetTicks();
  TestAccess::SetCaretBlinkEpochMs(shell, now);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::CurrentTextInputSurface(shell) == workspace::TextInputSurface::PromptInput,
         "the prompt should still hold the keyboard");
  TestAccess::SetCaretBlinkEpochMs(shell, now - TestAccess::CaretBlinkIntervalMs(shell) - 50);
  const std::optional<SDL_FRect> blink = TestAccess::CaretDirtyRect(shell);
  Expect(blink.has_value(), "the prompt's caret should blink");
  if (!blink.has_value()) {
    return;
  }
  const auto diff = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, workspace::RenderInvalidation{.full = false, .rects = {*blink}});
  Expect(diff.identical(), "a blink under the prompt should leave no caret in the commit body; " +
                               DescribePixelDiff(diff));
}

void TestRightClicksUnderAStickyBandActOnTheClickedLine() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string body = "namespace outer {\nstruct Inner {\n";
  for (int i = 0; i < 80; ++i) {
    body += "  int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  body += "};\n}\n";
  FlowFixture fixture("sticky.cpp", body);
  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "debug.enabled", "true"),
         "the gutter's breakpoint menu needs the debugger on");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_PAGEDOWN), KeyDownEvent(SDLK_PAGEDOWN)});
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "scrolling under the headers");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"][std::size_t{0}];
  Expect(pane["stickyRows"].AsInt() >= 2, "the fixture should pin two headers");
  const util::JsonValue& row = pane["rows"][std::size_t{4}];
  const std::int64_t line = row["line"].AsInt();
  const float y = static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() * 0.5);
  const auto right_click = [&](float x) {
    SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    down.button.button = SDL_BUTTON_RIGHT;
    SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    up.button.button = SDL_BUTTON_RIGHT;
    (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y), down, up});
  };
  right_click(static_cast<float>(pane["textX"].AsDouble() + 3.5 * pane["charWidth"].AsDouble()));
  Expect(static_cast<std::int64_t>(TestAccess::ActiveEditor(fixture.shell).cursor_line()) + 1 == line,
         "a right-click on line " + std::to_string(line) + " should put the caret there, not on line " +
             std::to_string(TestAccess::ActiveEditor(fixture.shell).cursor_line() + 1));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
  TestAccess::BreakpointStore(fixture.shell).Set(TestAccess::ActiveEditor(fixture.shell).path(),
                                                 static_cast<std::size_t>(line - 1));
  right_click(static_cast<float>(pane["gutterX"].AsDouble() + 6.0));
  Expect(TestAccess::BreakpointContextMenuOpen(fixture.shell), "a gutter right-click should open the breakpoint menu");
  Expect(static_cast<std::int64_t>(TestAccess::TreeContextMenuLine(fixture.shell)) + 1 == line,
         "the gutter's menu should be for line " + std::to_string(line) + ", not " +
             std::to_string(TestAccess::TreeContextMenuLine(fixture.shell) + 1));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});

  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "plugins.code_lens_above", "true"),
         "lenses should draw above their lines");
  editor::PluginDecorationData lenses;
  lenses.code_lenses.push_back(editor::CodeLensDecoration{
      .line = static_cast<std::uint32_t>(line - 1), .text = "2 refs", .command = "refs.show"});
  Expect(TestAccess::CurrentProjectState(fixture.shell).EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "lens-test", fixture.file, std::move(lenses)),
         "publishing the lens should change the store");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the lens painted");
  const util::JsonValue lens_ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& lens_pane = lens_ui["editors"][std::size_t{0}];
  std::optional<double> lens_row_y;
  std::optional<double> previous_row_y;
  for (const util::JsonValue& r : lens_pane["rows"].AsArray()) {
    if (r["line"].AsInt() == line) {
      lens_row_y = r["y"].AsDouble();
      break;
    }
    previous_row_y = r["y"].AsDouble();
  }
  Expect(lens_row_y.has_value() && previous_row_y.has_value(), "the lensed line should still be on screen");
  if (lens_row_y.has_value() && previous_row_y.has_value()) {
    const double lh = lens_pane["lineHeight"].AsDouble();
    Expect(std::fabs(*lens_row_y - *previous_row_y - 2.0 * lh) < 0.5,
           "the ui should report the lensed line a lens strip below the line above it");
    const auto lens = TestAccess::AboveLensAtPosition(
        fixture.shell, static_cast<float>(lens_pane["textX"].AsDouble() + 8.0),
        static_cast<float>(*lens_row_y - 0.5 * lh));
    Expect(lens.has_value() && lens->line == static_cast<std::size_t>(line - 1),
           "a click on the lens above line " + std::to_string(line) + " should resolve that lens");
  }
}

void TestTypingUnderACodeLensStripRepaintsTheTypedLine() {
  FlowFixture fixture("lensed.cpp", "int greet();\nint second();\nint third();\n");
  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "plugins.code_lens_above", "true"),
         "lenses should draw above their lines");
  editor::PluginDecorationData lenses;
  lenses.code_lenses.push_back(editor::CodeLensDecoration{.line = 0, .text = "greet (2)", .command = "refs.show"});
  lenses.code_lenses.push_back(editor::CodeLensDecoration{.line = 2, .text = "third (1)", .command = "refs.show"});
  Expect(TestAccess::CurrentProjectState(fixture.shell).EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "lens-test", fixture.file, std::move(lenses)),
         "publishing the lenses should change the store");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the lenses painted");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("//")}), "typing on the first lensed line");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN), KeyDownEvent(SDLK_DOWN)});
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the caret on line 3");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("//")}), "typing under the second lens");
}

void TestPartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  constexpr int kHeight = 400;
  std::string body;
  for (int i = 0; i < 30; ++i) {
    body += "line " + std::to_string(i) + " with enough text to reach the divider of a split pane\n";
  }
  for (const float kScale : {1.25f, 1.75f}) {
  for (const int width : {960, 963}) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    WriteFile(root / "split.txt", body);
    WorkspaceShell shell;
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the project should open");
    TestAccess::SetWindowSize(shell, width, kHeight);
    TestAccess::OpenSingleEditorTab(shell, root / "split.txt");
    TestAccess::SetFocusEditor(shell);
    (void)DispatchEvents(shell, {KeyDownEvent(SDLK_BACKSLASH, SDL_KMOD_CTRL)});
    Expect(TestAccess::ExecuteCommandLine(shell, "toggle-theme"), "the theme should toggle");
    Expect(TestAccess::ExecuteCommandLine(shell, "search line"), "the find widget should open");
    const int device_w = static_cast<int>(std::lround(width * kScale));
    const int device_h = static_cast<int>(std::lround(kHeight * kScale));
    SoftwareCanvas canvas{device_w, device_h};
    Expect(SDL_SetRenderLogicalPresentation(canvas.renderer(), width, kHeight,
                                            SDL_LOGICAL_PRESENTATION_STRETCH),
           "the canvas should take a 1.75x presentation");
    shell.SetWindowPresentationState(WorkspaceShell::WindowPresentationState{
        .logical_width = width, .logical_height = kHeight, .scale_x = kScale, .scale_y = kScale});
    const auto pin = [&] { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
    pin();
    shell.Render(canvas.renderer(), width, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    const auto& editors = ui["editors"].AsArray();
    Expect(editors.size() == 2, "the split should show two panes");
    if (editors.size() != 2) {
      continue;
    }
    const auto& left = editors[0]["rect"].AsArray();
    const float divider_x = static_cast<float>(left[0].AsInt() + left[2].AsInt());
    const workspace::FindWidgetLayout fw = TestAccess::FindWidgetControls(shell, false);
    (void)DispatchEvents(shell, {MouseMotionEvent(fw.close_button.x + fw.close_button.w / 2.0f,
                                                  fw.close_button.y + fw.close_button.h / 2.0f)});
    pin();
    shell.Render(canvas.renderer(), width, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
    const render::TextClipPadding padding =
        app::PadClipForPresentationScale(shell.PartialRedrawClipPadding(), kScale, kScale);
    const float sidebar_edge_x = static_cast<float>(left[0].AsInt());
    for (const float edge_x : {divider_x, sidebar_edge_x}) {
    for (float dx = -12.0f; dx <= 12.0f; dx += 1.5f) {
      for (const float w : {3.0f, 40.0f, 300.0f}) {
        const SDL_FRect damage{edge_x + dx - w, 60.0f, w, 250.0f};
        pin();
        const workspace::WorkspaceShell::FrameToken token =
            shell.PrepareFrameOnce(canvas.renderer(), width, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        const SDL_Rect clip = render::AlignClipToDevicePixels(
            canvas.renderer(), InflateClipRect(damage, padding, width, kHeight));
        SDL_SetRenderClipRect(canvas.renderer(), &clip);
        shell.RenderClip(token, canvas.renderer(), width, kHeight, app::DeviceClipHint(clip, kScale, kScale));
        SDL_SetRenderClipRect(canvas.renderer(), nullptr);
        SDL_FlushRenderer(canvas.renderer());
        SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
        pin();
        shell.Render(canvas.renderer(), width, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        SDL_FlushRenderer(canvas.renderer());
        SDL_Surface* reference = SDL_ConvertSurface(canvas.surface(), retained->format);
        int solid = 0;
        int first_x = -1;
        int first_y = -1;
        for (int y = 0; y < retained->h && reference != nullptr; ++y) {
          const auto* a = static_cast<const std::uint8_t*>(retained->pixels) + y * retained->pitch;
          const auto* b = static_cast<const std::uint8_t*>(reference->pixels) + y * reference->pitch;
          for (int x = 0; x < retained->w * 4; ++x) {
            if (std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x])) > 8) {
              if (solid++ == 0) {
                first_x = x / 4;
                first_y = y;
              }
              x += 3 - (x % 4);
            }
          }
        }
        SDL_DestroySurface(reference);
        const render::PixelDiffResult diff = DiffSurfaces(retained, canvas.surface());
        if (const char* dump = std::getenv("MICROIDE_TEST_DUMP_DIFF"); dump != nullptr && solid != 0) {
          SDL_SaveBMP(retained, (std::string(dump) + "/frac-retained.bmp").c_str());
          SDL_SaveBMP(canvas.surface(), (std::string(dump) + "/frac-reference.bmp").c_str());
        }
        SDL_DestroySurface(retained);
        Expect(solid == 0, "width " + std::to_string(width) + ", damage x " +
                               std::to_string(damage.x) + " w " + std::to_string(w) +
                               " (divider at " + std::to_string(divider_x) +
                               "): a partial frame should paint what a full one does; " +
                               std::to_string(solid) + " pixels differ by colour, first at " +
                               std::to_string(first_x) + "," + std::to_string(first_y) +
                               " (" + DescribePixelDiff(diff) + " in all)");
        if (solid != 0) {
          return;
        }
      }
    }
  }
  }
  }
}

void TestAPromptCaretBlinkAtScaleThreeLeavesNoColumn() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  constexpr int kWidth = 213;
  constexpr int kHeight = 160;
  constexpr float kScale = 3.0f;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::string body;
  for (int i = 0; i < 40; ++i) {
    body += "line " + std::to_string(i) + "\n";
  }
  WriteFile(root / "notes.txt", body);
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the project should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OpenSingleEditorTab(shell, root / "notes.txt");
  TestAccess::SetFocusEditor(shell);
  SoftwareCanvas canvas{static_cast<int>(kWidth * kScale), static_cast<int>(kHeight * kScale)};
  Expect(SDL_SetRenderLogicalPresentation(canvas.renderer(), kWidth, kHeight, SDL_LOGICAL_PRESENTATION_STRETCH),
         "the canvas should take a 3x presentation");
  shell.SetWindowPresentationState(WorkspaceShell::WindowPresentationState{
      .logical_width = kWidth, .logical_height = kHeight, .scale_x = kScale, .scale_y = kScale});
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_G, SDL_KMOD_CTRL)});
  Expect(TestAccess::CurrentTextInputSurface(shell) == workspace::TextInputSurface::PromptInput,
         "Ctrl+G should open the Go to Line prompt");
  const Uint64 interval = TestAccess::CaretBlinkIntervalMs(shell);
  const auto phase = [&](bool lit) {
    TestAccess::SetCaretBlinkEpochMs(shell, lit ? SDL_GetTicks() : SDL_GetTicks() - interval - 50);
  };
  const render::TextClipPadding padding =
      app::PadClipForPresentationScale(shell.PartialRedrawClipPadding(), kScale, kScale);
  app::DirtyRegionAnalysis analysis;
  for (int digits = 0; digits <= 24; ++digits) {
    if (digits > 0) {
      (void)DispatchEvents(shell, {TextInputEvent(digits % 3 == 0 ? "7" : digits % 3 == 1 ? "1" : "4")});
    }
    for (const bool to_lit : {false, true}) {
      phase(!to_lit);
      shell.Render(canvas.renderer(), kWidth, kHeight);
      (void)shell.ConsumePendingRenderInvalidation();
      phase(to_lit);
      const workspace::RenderInvalidation wake = TestAccess::HandleScheduledWake(shell).redraw;
      Expect(!wake.full && !wake.rects.empty(), "the prompt's blink names its caret's rect");
      const workspace::WorkspaceShell::FrameToken token = shell.PrepareFrameOnce(canvas.renderer(), kWidth, kHeight);
      const workspace::RenderInvalidation prepared = shell.ConsumePendingRenderInvalidation();
      Expect(!prepared.full, "a blink does not repaint the window");
      std::vector<SDL_FRect> rects(wake.rects.begin(), wake.rects.end());
      rects.insert(rects.end(), prepared.rects.begin(), prepared.rects.end());
      app::AnalyzeDirtyRegions(rects, padding, kWidth, kHeight, SDL_FPoint{kScale, kScale}, analysis);
      for (const SDL_Rect& merged : analysis.merged_clip_rects) {
        const SDL_Rect clip = render::AlignClipToDevicePixels(canvas.renderer(), merged);
        SDL_SetRenderClipRect(canvas.renderer(), &clip);
        shell.RenderClip(token, canvas.renderer(), kWidth, kHeight, app::DeviceClipHint(clip, kScale, kScale));
      }
      SDL_SetRenderClipRect(canvas.renderer(), nullptr);
      SDL_FlushRenderer(canvas.renderer());
      SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
      phase(to_lit);
      shell.Render(canvas.renderer(), kWidth, kHeight);
      (void)shell.ConsumePendingRenderInvalidation();
      SDL_FlushRenderer(canvas.renderer());
      const render::PixelDiffResult diff = DiffSurfaces(retained, canvas.surface());
      SDL_DestroySurface(retained);
      Expect(diff.identical(), std::string("a prompt caret blinking ") + (to_lit ? "on" : "off") + " after " +
                                   std::to_string(digits) + " digits should paint what a full frame paints; " +
                                   DescribePixelDiff(diff) + " (device pixels)");
      if (!diff.identical()) {
        return;
      }
    }
  }
}

void TestMergeToolbarStaysInsideANarrowPane() {
  FlowFixture fixture("notes.txt", "notes\n");
  const std::filesystem::path base = fixture.root / "base.txt";
  const std::filesystem::path incoming = fixture.root / "incoming.txt";
  const std::filesystem::path current = fixture.root / "current.txt";
  const std::filesystem::path output = fixture.root / "result.txt";
  WriteFile(base, "alpha\none\ntwo\n");
  WriteFile(incoming, "incoming\none\ntwo\n");
  WriteFile(current, "current\none\ntwo\n");
  WriteFile(output, "alpha\none\ntwo\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, base, incoming, current, output),
         "the merge editor should open");
  constexpr int kNarrowWidth = 520;
  TestAccess::SetWindowSize(fixture.shell, kNarrowWidth, FlowFixture::kHeight);
  fixture.shell.Render(fixture.canvas.renderer(), kNarrowWidth, FlowFixture::kHeight);
  const workspace::WorkspaceLayout layout = TestAccess::CurrentLayout(fixture.shell);
  Expect(layout.editor_surface.w < 400.0f, "the fixture must squeeze the editor pane");
  std::size_t seen = 0;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string& name = entry["name"].AsString();
    if (!name.starts_with("button:merge/") || name.find('/', 13) != std::string::npos) {
      continue;
    }
    ++seen;
    const auto& r = entry["rect"].AsArray();
    Expect(static_cast<float>(r[0].AsInt()) >= layout.editor_surface.x,
           name + " must not start left of the editor pane");
  }
  Expect(seen > 0, "the narrow pane still shows the toolbar buttons that fit");
}

void TestMergeToolbarStaysInsideAShortPane() {
  FlowFixture fixture("notes.txt", "notes\n");
  const std::filesystem::path base = fixture.root / "base.txt";
  const std::filesystem::path incoming = fixture.root / "incoming.txt";
  const std::filesystem::path current = fixture.root / "current.txt";
  const std::filesystem::path output = fixture.root / "result.txt";
  WriteFile(base, "alpha\none\ntwo\n");
  WriteFile(incoming, "incoming\none\ntwo\n");
  WriteFile(current, "current\none\ntwo\n");
  WriteFile(output, "alpha\none\ntwo\n");
  Expect(TestAccess::SplitEditorGroup(fixture.shell, workspace::EditorSplitOrientation::Horizontal),
         "the editor should split into stacked groups");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, base, incoming, current, output),
         "the merge editor should open");
  TestAccess::OpenTerminalViaService(fixture.shell);
  bool squeezed = false;
  for (const float panel : {500.0f, 540.0f, 580.0f, 620.0f, 660.0f}) {
    TestAccess::SetBottomPanelHeight(fixture.shell, panel);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    const SDL_FRect pane = TestAccess::ActiveTabSurface(fixture.shell);
    if (pane.h <= 0.0f || pane.h >= 56.0f) {
      continue;
    }
    squeezed = true;
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      const std::string& name = entry["name"].AsString();
      if (!name.starts_with("button:merge/") || name.find('/', 13) != std::string::npos) {
        continue;
      }
      const auto& r = entry["rect"].AsArray();
      Expect(static_cast<float>(r[1].AsInt() + r[3].AsInt()) <= pane.y + pane.h,
             name + " must not run past the bottom of a " + std::to_string(pane.h) +
                 " px pane");
    }
  }
  Expect(squeezed, "the fixture must squeeze the editor pane below the toolbar's two rows");
}

void TestMergeToolbarHoverRepaintsTheButton() {
  FlowFixture fixture("notes.txt", "notes\n");
  const std::filesystem::path base = fixture.root / "base.txt";
  const std::filesystem::path incoming = fixture.root / "incoming.txt";
  const std::filesystem::path current = fixture.root / "current.txt";
  const std::filesystem::path output = fixture.root / "result.txt";
  WriteFile(base, "alpha\none\ntwo\n");
  WriteFile(incoming, "incoming\none\ntwo\n");
  WriteFile(current, "current\none\ntwo\n");
  WriteFile(output, "alpha\none\ntwo\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, base, incoming, current, output),
         "the merge editor should open");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto away = fixture.ElementRect("button:merge/save");
  Expect(away.has_value(), "the ui query should name the merge Save button");
  for (const std::string_view name : {"button:merge/save", "button:merge/open", "button:merge/prev",
                                      "button:merge/Toggle Base", "button:merge/Mark Resolved"}) {
    const auto rect = fixture.ElementRect(name);
    Expect(rect.has_value(), "the ui query should name " + std::string(name));
    if (!rect.has_value()) {
      continue;
    }
    const float x = rect->x + rect->w / 2.0f;
    const float y = rect->y + rect->h / 2.0f;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}),
                        "hovering " + std::string(name));
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y + 120.0f)}),
                        "leaving " + std::string(name));
  }
}

void TestLeavingAPanelTabMidSlideLeavesNoHover() {
  FlowFixture fixture("notes.txt", "notes\n");
  for (int i = 0; i < 4; ++i) {
    TestAccess::OpenTerminalViaService(fixture.shell);
  }
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "four shell tabs");
  std::vector<SDL_FRect> tabs;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() == "panel-tab" &&
        entry["name"].AsString().rfind("panel-tab:bash", 0) == 0) {
      const auto& r = entry["rect"].AsArray();
      tabs.push_back(SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                               static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())});
    }
  }
  Expect(tabs.size() >= 4, "the panel should show the four shell tabs");
  if (tabs.size() < 4) {
    return;
  }
  const float y = tabs[0].y + tabs[0].h / 2.0f;
  const float start = tabs[0].x + 12.0f;
  const float end = tabs[3].x + tabs[3].w * 0.4f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(start, y)}), "resting on the first tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, start, y)}),
      "pressing the first tab");
  const auto held_motion = [](float x, float motion_y) {
    SDL_Event event = MouseMotionEvent(x, motion_y);
    event.motion.state = SDL_BUTTON_LMASK;
    return event;
  };
  for (float x = start; x < end; x += 24.0f) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {held_motion(x, y)}), "dragging along the strip");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {held_motion(end, y)}), "dragging onto the last tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, end, y)}),
      "dropping it there");
  const auto file_menu = fixture.ElementRect("menu:File");
  Expect(file_menu.has_value(), "the ui query should name the File menu");
  if (!file_menu.has_value()) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(file_menu->x + 4.0f, file_menu->y + 4.0f)}),
                      "the pointer leaving for the menu bar while the tabs slide");
}

void TestADragAcrossPanelTabsLeavesNoHoveredCloseButton() {
  FlowFixture fixture("notes.txt", "notes\n");
  TestAccess::OpenTerminalViaService(fixture.shell);
  TestAccess::OpenTerminalViaService(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  std::vector<SDL_FRect> tabs;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() == "panel-tab" &&
        entry["name"].AsString().rfind("panel-tab:bash", 0) == 0) {
      const auto& r = entry["rect"].AsArray();
      tabs.push_back(SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                               static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())});
    }
  }
  Expect(tabs.size() >= 2, "the panel should show the two shell tabs");
  if (tabs.size() < 2) {
    return;
  }
  const float y = tabs[0].y + tabs[0].h / 2.0f;
  const float start = tabs[0].x + 12.0f;
  const float over_close = tabs[0].x + tabs[0].w - 12.0f;
  const float end = tabs[1].x + tabs[1].w * 0.8f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(start, y)}),
                      "resting on the first tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, start, y)}),
      "pressing the first tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(over_close, y)}),
                      "dragging over its close button");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(end, y)}),
                      "dragging onto the next tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, end, y)}),
      "releasing on the next tab");

  const auto view_menu = fixture.ElementRect("menu:View");
  Expect(view_menu.has_value(), "the ui query should name the View menu");
  if (!view_menu.has_value()) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*view_menu), "opening the View menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(over_close, y)}),
                      "the pointer over a tab's close button under the open menu");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, over_close, y)}),
      "the press that closes the menu");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, over_close, y)}),
      "its release");
}

void TestSearchResultsCaughtUpAtFramePrepRepaintTheirCount() {
  FlowFixture fixture("notes.txt", "alpha\nalpha beta\n");
  TestAccess::ShowSearchSidebar(fixture.shell, "alpha", false);
  WaitForProjectSearch(fixture.shell);
  TestAccess::SetFocusEditor(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::ProjectSearchResults(fixture.shell).size() == 2, "the search finds both alphas");

  editor::TextViewport& editor = TestAccess::ActiveEditor(fixture.shell);
  editor.MoveCursorTo(0, 0);
  editor.InsertText("alpha ");
  const SDL_FRect editor_surface = TestAccess::CurrentLayout(fixture.shell).editor_surface;
  TestAccess::CatchUpSearchResults(fixture.shell);
  TestAccess::SettleLiveSearchHits(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = false, .rects = {editor_surface}},
                      "a frame that shows an edit and the search results caught up with it");
  Expect(TestAccess::ProjectSearchResults(fixture.shell).size() == 3,
         "the results followed the edit (a third alpha)");
}

void TestAKeyInAMenuUnderARestingPointerRepaintsTheRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("menu.txt", "text\n");
  const auto go = fixture.ElementRect("menu:Go");
  Expect(go.has_value(), "the Go menu should be on screen");
  if (!go) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*go), "opening the Go menu");
  const auto row = fixture.ElementRect("menu-item:Go/Find in Project…");
  Expect(row.has_value(), "the ui query should name the Go menu's Find in Project row");
  if (!row) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(60.0f, 700.0f)}),
                      "the pointer leaving the open menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseWheelEvent(595.0f, 400.0f, -5.0f)}),
                      "a wheel over the editor under the open menu");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseMotionEvent(row->x + row->w / 2.0f, row->y + row->h / 2.0f)}),
      "the pointer resting on a menu row");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_UP)}), "Up in the open menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(60.0f, 700.0f)}),
                      "the pointer leaving the menu after the key");
}

void TestLeavingTheCommitSubjectFieldRepaintsItsCaret() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "notes.txt", "base\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(root / "notes.txt", "changed\n");
  RequireGitCommandSuccess(root, {"add", "notes.txt"}, "git add notes.txt");
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1), "the git view should list the change");
  Expect(TestAccess::ExecuteCommandLine(shell, "git-commit"),
         "the commit form opens: " + TestAccess::CommandFeedbackText(shell));
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pin = PinCaretOf(shell);
  pin();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto field_rect = [&]() -> std::optional<SDL_FRect> {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == "field:git/subject") {
        const auto& r = entry["rect"].AsArray();
        return SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
      }
    }
    return std::nullopt;
  };
  const auto subject = field_rect();
  Expect(subject.has_value(), "the ui query should name the commit Subject field");
  if (!subject) {
    return;
  }
  const auto expect_fresh = [&](const workspace::RenderInvalidation& redraw, const std::string& what) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin);
    Expect(diff.identical(), what + " should paint what a full repaint paints; " + DescribePixelDiff(diff));
  };
  const float x = subject->x + subject->w / 2.0f;
  const float y = subject->y + subject->h / 2.0f;
  expect_fresh(DispatchEvents(shell, {MouseMotionEvent(x, y),
                                      MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                      MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
               "clicking into the Subject field");
  expect_fresh(DispatchEvents(shell, {TextInputEvent("alpha")}), "typing a subject");
  expect_fresh(DispatchEvents(shell, {KeyDownEvent(SDLK_F6)}), "F6 out of the field");
  TestAccess::SetFocusEditor(shell);
  expect_fresh(workspace::RenderInvalidation{.full = true, .rects = {}}, "the editor focused");
  expect_fresh(DispatchEvents(shell, {KeyDownEvent(SDLK_F6)}), "F6 into the sidebar");
  expect_fresh(DispatchEvents(shell, {MouseMotionEvent(x, y)}), "the pointer over the field");
  const float below = subject->y + 240.0f;
  const workspace::RenderInvalidation click =
      DispatchEvents(shell, {MouseMotionEvent(x, below), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, below),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, below)});
  TestAccess::SettleCompareLoads(shell);
  expect_fresh(click, "a click lower in the sidebar, and the diff it opened once loaded");
}

void TestACommitBodySelectionStaysInsideItsField() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "notes.txt", "base\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(root / "notes.txt", "changed\n");
  RequireGitCommandSuccess(root, {"add", "notes.txt"}, "git add notes.txt");
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1), "the git view should list the change");
  Expect(TestAccess::ExecuteCommandLine(shell, "git-commit"), "the commit form opens");
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pin = PinCaretOf(shell);
  pin();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  std::optional<SDL_FRect> body;
  const util::JsonValue ui = TestAccess::ControlUi(shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "field:git/body") {
      const auto& r = entry["rect"].AsArray();
      body = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                       static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(body.has_value(), "the ui query should name the commit Body field");
  if (!body) {
    return;
  }
  const auto expect_fresh = [&](const workspace::RenderInvalidation& redraw, const std::string& what) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin);
    Expect(diff.identical(), what + " should paint what a full repaint paints; " + DescribePixelDiff(diff));
  };
  const float x = body->x + 10.0f;
  const float y = body->y + 8.0f;
  expect_fresh(DispatchEvents(shell, {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                      MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
               "clicking into the Body field");
  expect_fresh(DispatchEvents(shell, {TextInputEvent("a body line far wider than the field it is typed into, "
                                                     "and wider again, and again, until it runs well past")}),
               "typing a line wider than the field");
  expect_fresh(DispatchEvents(shell, {KeyDownEvent(SDLK_A, SDL_KMOD_CTRL)}), "selecting the whole body");
}

void TestMovingOntoAPanelTabsCloseButtonRepaintsIt() {
  FlowFixture fixture("notes.txt", "notes\n");
  TestAccess::OpenTerminalViaService(fixture.shell);
  TestAccess::OpenTerminalViaService(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  std::vector<std::pair<SDL_FRect, SDL_FRect>> tabs;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto rect_of = [](const util::JsonValue& entry) {
    const auto& r = entry["rect"].AsArray();
    return SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                     static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
  };
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() != "panel-tab") {
      continue;
    }
    const std::string close = "button:close-panel-tab/" + entry["label"].AsString();
    for (const util::JsonValue& other : ui["elements"].AsArray()) {
      if (other["name"].AsString() == close) {
        tabs.emplace_back(rect_of(entry), rect_of(other));
      }
    }
  }
  Expect(tabs.size() >= 2, "the panel should show tabs with close buttons");
  if (tabs.size() < 2) {
    return;
  }
  for (const auto& [tab, close] : {tabs[0], tabs[1]}) {
    const float y = tab.y + tab.h / 2.0f;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab.x + 10.0f, y)}),
                        "the pointer on a panel tab's label");
    fixture.ExpectFresh(
        DispatchEvents(fixture.shell, {MouseMotionEvent(close.x + close.w / 2.0f, close.y + close.h / 2.0f)}),
        "the pointer onto that tab's close button");
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab.x + 10.0f, y)}),
                        "back onto its label");
  }
}


void TestASearchRowThatDoesNotFitTheSidebarIsLeftOut() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n", 213, 160);
  TestAccess::ShowSearchSidebar(fixture.shell, "[]", false);
  WaitForProjectSearch(fixture.shell);
  TestAccess::SetFocusEditor(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "status-bar-toggle"), "status-bar-toggle should run");
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Error,
                               "fatal: No configured push destination");
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Info, "git remote is configured");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the search view in a short sidebar, with toasts");
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  const float bottom = sidebar.y + sidebar.h;
  Expect(sidebar.y + workspace::project_search_panel::kReplaceTop +
                 workspace::project_search_panel::kFieldHeight >
             bottom,
         "the sidebar should be too short for the replace row");
  const auto all = fixture.ElementRect("button:search/replace-all");
  Expect(!all.has_value() || all->w <= 0.0f || all->h <= 0.0f, "Replace All should not be laid out");
  const auto replace = fixture.ElementRect("field:search/replace");
  Expect(!replace.has_value() || replace->w <= 0.0f || replace->h <= 0.0f,
         "the replace field should not be laid out");
  const auto query = fixture.ElementRect("field:search/query");
  Expect(query.has_value() && query->h > 0.0f && query->y + query->h <= bottom,
         "the query field, which fits, should be");
  const float x = sidebar.x + sidebar.w - workspace::project_search_panel::kInset - 3.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, bottom)}),
                      "the pointer just below the sidebar, where Replace All was");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(150.0f, 60.0f)}), "the pointer away");
}

void TestAPaneNarrowedToItsMinimumPaintsNothingOutsideIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string content;
  for (int line = 1; line <= 60; ++line) {
    content += "line " + std::to_string(line) + " of a file taller than the pane\n";
  }
  FlowFixture fixture("a.txt", content, 213, 160);
  if (TestAccess::CurrentLayout(fixture.shell).bottom_panel.h > 0.0f) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "panel-toggle"), "panel-toggle should run");
  }
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "two panes beside the sidebar");
  const auto divider = fixture.ElementRect("divider:editor/0");
  Expect(divider.has_value(), "the ui query should name divider:editor/0");
  if (!divider) {
    return;
  }
  const float y = divider->y + divider->h / 2.0f;
  const float x0 = divider->x + divider->w / 2.0f;
  const float x1 = 20.0f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x0, y),
                                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y)}),
                      "pressing on the divider");
  for (int step = 1; step <= 12; ++step) {
    SDL_Event motion = MouseMotionEvent(x0 + (x1 - x0) * static_cast<float>(step) / 12.0f, y);
    motion.motion.state = SDL_BUTTON_LMASK;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {motion}),
                        "dragging the divider left, step " + std::to_string(step));
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x1, y)}),
                      "releasing it");
}

}

void TestTheRemoteSegmentOpensTheRemoteMenu() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("remote.txt", "text\n");
  workspace::StatusBarSegmentValue remote;
  remote.text = "dev@box";
  remote.tooltip = "Remote menu";
  remote.menu = workspace::MenuId::Remote;
  remote.visible = true;
  TestAccess::SetStatusBarSegment(fixture.shell, workspace::StatusBarSegmentId::Remote, std::move(remote));
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  bool reported = false;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString() == "status:remote") {
      reported = entry["enabled"].AsBool() && entry["menu"].AsString() == "Remote";
    }
  }
  Expect(reported, "the ui query should report the remote segment as opening the Remote menu");

  const auto segment = fixture.ElementRect("status:remote");
  Expect(segment.has_value(), "the status bar should show the remote segment");
  if (!segment) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*segment), "opening the Remote menu from the status bar");
  Expect(TestAccess::MenuBarOpen(fixture.shell) &&
             TestAccess::ActiveMenuId(fixture.shell) == workspace::MenuId::Remote,
         "a click on the remote segment should open the Remote menu");
  const auto open_row = fixture.ElementRect("menu-item:Remote/Open Folder on Host…");
  const auto status_row = fixture.ElementRect("menu-item:Remote/Show Status");
  Expect(open_row.has_value() && status_row.has_value(), "the popup should list the remote verbs");
  if (open_row) {
    Expect(open_row->y + open_row->h <= segment->y,
           "the popup should open above the status bar, there being no room below");
  }
  const util::JsonValue open_ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : open_ui["elements"].AsArray()) {
    const std::string& name = entry["name"].AsString();
    if (!name.starts_with("menu-item:Remote/")) {
      continue;
    }
    if (name == "menu-item:Remote/Remove Mirror…") {
      Expect(entry["enabled"].AsBool() == TestAccess::IsActionEnabled(fixture.shell, workspace::ActionId::RemoteRemoveMirror),
             "Remove Mirror… is drawn as available exactly when it is");
      continue;
    }
    const bool always = name == "menu-item:Remote/Open Folder on Host…";
    Expect(entry["enabled"].AsBool() == always,
           name + (always ? " should be available with no host" : " should be dimmed with no host connected"));
  }
  using workspace::MenuId;
  Expect(TestAccess::PaintedMenuItemLabel(fixture.shell, MenuId::Remote, "Show Status") ==
             "Show Status (connect to a host first)",
         "a dimmed connection verb says what it waits for: '" +
             TestAccess::PaintedMenuItemLabel(fixture.shell, MenuId::Remote, "Show Status") + "'");
  Expect(TestAccess::PaintedMenuItemLabel(fixture.shell, MenuId::Remote, "Forward a Port…") ==
             "Forward a Port… (connect to a host first)",
         "Forward a Port… too");
  Expect(TestAccess::PaintedMenuItemLabel(fixture.shell, MenuId::Remote, "Stop Forwarding a Port…") ==
             "Stop Forwarding a Port… (no port is forwarded)",
         "Stop Forwarding says there is nothing forwarded");
  Expect(TestAccess::PaintedMenuItemLabel(fixture.shell, MenuId::Remote, "Open Folder on Host…") ==
             "Open Folder on Host…",
         "an available row paints its label alone");

  SendKeyDown(fixture.shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  Expect(!TestAccess::MenuBarOpen(fixture.shell), "Escape should close the Remote menu");

  (void)fixture.Click(*segment);
  const auto row = fixture.ElementRect("menu-item:Remote/Open Folder on Host…");
  Expect(row.has_value(), "the Remote menu should open again");
  if (row) {
    (void)fixture.Click(*row);
    Expect(!TestAccess::MenuBarOpen(fixture.shell), "running a row should close the menu");
    Expect(!TestAccess::ControlUi(fixture.shell)["modal"].IsNull(),
           "Open Folder on Host… should open the host prompt");
  }
}

void TestClosingTheLastTabWhenZoomedInRepaintsTheStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("README.md", "# taskflow\n\nA tiny, dependency-free showcase.\n", 214, 160);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  TestAccess::SetFocusEditor(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "one tab in a 214x160-unit window");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)}),
                      "Ctrl+W on the only tab, zoomed in");
}

void TestTypingInAnEditorWithNoRowsRepaintsTheStatusBar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("README.md", "# taskflow\n", 214, 160);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  TestAccess::SetFocusEditor(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "an editor with no rows under the panel");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "typing into it");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_NONE)}),
                      "moving its caret");
}

void TestTabInANarrowDirtyPromptRepaintsEveryButton() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("many-lines.txt", "one\ntwo\n", 316, 216);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "one tab in a 316x216-unit window");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "an edit (the tab goes dirty)");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)}),
                      "Ctrl+W on the dirty tab opens the prompt");
  Expect(!TestAccess::ControlUi(fixture.shell)["modal"].IsNull(), "the dirty prompt should be open");
  for (int step = 1; step <= 3; ++step) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_TAB, SDL_KMOD_NONE)}),
                        "Tab in the dirty prompt, " + std::to_string(step));
  }
}

void TestABreadcrumbLabelChangeRepaintsTheBand() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file in its tab");
  TestAccess::ActiveEditor(fixture.shell).SetPath(fixture.root / "renamed.txt");
  workspace::RenderInvalidation editor_only;
  editor_only.Add(SDL_FRect{400.0f, 300.0f, 20.0f, 20.0f});
  fixture.ExpectFresh(editor_only, "the document renamed, the editor alone damaged");
}

void TestAContextMenuClosesOverTheRowsItPainted() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file in its tab");
  TestAccess::OpenTreeContextMenuForPath(fixture.shell, workspace::TreeContextTargetKind::File, fixture.file);
  fixture.ExpectFresh(full, "the file's context menu, the long list");
  TestAccess::SetTreeContextMenuTargetForTest(fixture.shell, workspace::TreeContextTargetKind::Directory);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)}),
                      "Escape closes the menu whose list got shorter");
}

void TestAPointerOnAToastHoversNoMenuRowBeneathIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 640, 240);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file in its tab");
  TestAccess::OpenTreeContextMenuForPath(fixture.shell, workspace::TreeContextTargetKind::File, fixture.file);
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Warning,
                               "Debugging is disabled (enable it in Settings, Debugger) and this toast wraps "
                               "onto a few lines so that it reaches up over the menu");
  fixture.ExpectFresh(full, "the file's context menu, a toast over its lower right");

  const SDL_FRect toast = TestAccess::NotificationToastRect(fixture.shell, 0);
  std::optional<SDL_FPoint> point;
  for (const auto& item : TestAccess::TreeContextMenuVisibleItems(fixture.shell)) {
    if (item.separator) {
      continue;
    }
    const float left = std::max(item.rect.x, toast.x) + 2.0f;
    const float right = std::min(item.rect.x + item.rect.w, toast.x + toast.w) - 2.0f;
    const float top = std::max(item.rect.y, toast.y) + 2.0f;
    const float bottom = std::min(item.rect.y + item.rect.h, toast.y + toast.h) - 2.0f;
    if (left < right && top < bottom) {
      point = SDL_FPoint{left, (top + bottom) * 0.5f};
      break;
    }
  }
  Expect(point.has_value(), "the toast covers a menu row (the case under test)");
  if (!point.has_value()) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(point->x, point->y)}),
                      "the pointer moved onto the toast where it covers a menu row");
}

void TestAPointerMovingOntoAToastUnlightsTheMenuRowItLeft() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 840, 600);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file in its tab");
  const auto git = fixture.ElementRect("menu:Git");
  Expect(git.has_value(), "the menu bar shows Git");
  if (!git) {
    return;
  }
  (void)fixture.Click(*git);
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Warning,
                               "No completions available");
  fixture.ExpectFresh(full, "the Git menu open, a toast at the bottom right");
  const SDL_FRect toast = TestAccess::NotificationToastRect(fixture.shell, 0);

  std::optional<SDL_FPoint> dimmed_row;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (!entry["name"].AsString().starts_with("menu-item:Git/") || entry["enabled"].AsBool()) {
      continue;
    }
    const auto rect = fixture.ElementRect(entry["name"].AsString());
    if (rect && !workspace::Contains(toast, rect->x + 8.0f, rect->y + rect->h * 0.5f)) {
      dimmed_row = SDL_FPoint{rect->x + 8.0f, rect->y + rect->h * 0.5f};
      break;
    }
  }
  Expect(dimmed_row.has_value(), "a dimmed Git row the toast leaves uncovered (the case under test)");
  if (!dimmed_row) {
    return;
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(dimmed_row->x, dimmed_row->y)}),
                      "the pointer lit a dimmed menu row");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(toast.x + toast.w - 4.0f,
                                                                      toast.y + toast.h * 0.5f)}),
                      "the pointer left the row for the toast");
}

void TestAPointerMovingOntoAToastUnlightsTheTabItLeft() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 840, 600);
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Warning,
                               "No completions available");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the file in its tab, a toast at the bottom right");
  const auto tab = fixture.ElementRect("tab:0/notes.txt");
  Expect(tab.has_value(), "the strip shows notes.txt");
  if (!tab) {
    return;
  }
  const SDL_FRect toast = TestAccess::NotificationToastRect(fixture.shell, 0);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab->x + 6.0f, tab->y + tab->h * 0.5f)}),
                      "the pointer lit the tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(toast.x + toast.w - 4.0f,
                                                                      toast.y + toast.h * 0.5f)}),
                      "the pointer left the tab for the toast");
}

void TestScrollingTheProjectStripRepaintsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\n", 360, 300);
  for (const char* name : {"second-project-with-a-long-name", "third-project-with-a-long-name"}) {
    const std::filesystem::path other = fixture.temp_dir.path() / name;
    WriteFile(other / "a.txt", "a\n");
    Expect(TestAccess::OpenProjectTab(fixture.shell, other, false, false), std::string("opens ") + name);
  }
  Expect(TestAccess::ProjectCount(fixture.shell) == 3, "three projects");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "three projects in a narrow strip");
  TestAccess::SetProjectStripScrollForTest(fixture.shell, 0);
  workspace::RenderInvalidation arrow_only;
  arrow_only.Add(SDL_FRect{2.0f, 28.0f, 10.0f, 10.0f});
  fixture.ExpectFresh(arrow_only, "the strip scrolled back to its first project");
}


namespace {

workspace::RenderInvalidation NothingAnnounced() {
  workspace::RenderInvalidation none;
  none.rects.push_back(SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f});
  return none;
}

std::string OneBlockFile(int body_lines, int lines_above = 0) {
  std::string text;
  for (int i = 0; i < lines_above; ++i) {
    text += "int above" + std::to_string(i) + ";\n";
  }
  text += "void run() {\n";
  for (int i = 0; i < body_lines; ++i) {
    text += "    int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  text += "}\n";
  return text;
}

void TestTheStickyBandFollowsTheCaretOffItsLine() {
  FlowFixture fixture("sticky.cpp", OneBlockFile(120));
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(0, 0);
  viewport.SetScrollLine(54);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(viewport.cursor_line() == 0 && viewport.scroll_line() == 54,
         "the caret stays on the block's opener, scrolled out of sight");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "goto 66"), "goto should run");
  Expect(viewport.scroll_line() == 54, "the target row is in view: nothing scrolls");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(),
                      "Go to Line moved the caret off the sticky band's line");
  std::vector<editor::Diagnostic> squiggles;
  squiggles.push_back(editor::Diagnostic{
      .range = editor::SelectionRange{.start = {.line = 0, .column = 0}, .end = {.line = 0, .column = 4}},
      .severity = editor::DiagnosticSeverity::Error,
      .message = "sticky"});
  (void)TestAccess::CurrentProjectState(fixture.shell)
      .diagnostics_store.ReplaceForOwnerFile("sticky", viewport.path(), std::move(squiggles));
  fixture.ExpectFresh(NothingAnnounced(), "a diagnostic on the sticky band's line");
}

void TestRepublishedTextStylesAndGutterMarksRepaint() {
  FlowFixture fixture("sticky.cpp", OneBlockFile(120));
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(60, 0);
  viewport.SetScrollLine(54);
  auto& decorations = TestAccess::CurrentProjectState(fixture.shell).EnsurePluginPresentation().decorations;
  const auto publish = [&](SDL_Color token, editor::GutterIconShape shape, SDL_Color mark) {
    editor::PluginDecorationData data;
    for (const std::uint32_t line : {0u, 60u}) {
      data.text_styles.push_back(editor::TextStyleDecoration{
          .line = line, .start_column = 0, .end_column = 4, .foreground = token});
    }
    data.gutter_marks.push_back(editor::GutterMarkDecoration{.line = 62, .shape = shape, .color = mark});
    Expect(decorations.ReplaceForOwnerFile("tokens", viewport.path(), std::move(data)),
           "publishing should change the store");
  };
  publish(SDL_Color{200, 80, 80, 255}, editor::GutterIconShape::Dot, SDL_Color{80, 200, 80, 255});
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  publish(SDL_Color{80, 80, 220, 255}, editor::GutterIconShape::Dot, SDL_Color{80, 200, 80, 255});
  fixture.ExpectFresh(NothingAnnounced(), "tokens recoloured at the same columns, in a row and in the band");
  publish(SDL_Color{80, 80, 220, 255}, editor::GutterIconShape::Diamond, SDL_Color{220, 160, 40, 255});
  fixture.ExpectFresh(NothingAnnounced(), "a gutter mark that changed shape and colour");
}

void TestSplitPanesKeepTheirOwnStickyLines() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.cpp", OneBlockFile(120));
  WriteFile(fixture.root / "b.cpp", OneBlockFile(120, 3));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right b.cpp"), "split-right should open b.cpp beside a.cpp");
  editor::TextViewport* right = &TestAccess::ActiveEditor(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-left"), "focus-group-left should run");
  editor::TextViewport* left = &TestAccess::ActiveEditor(fixture.shell);
  Expect(left != right, "two panes show two viewports");
  const auto render = [&] {
    for (int pass = 0; pass < 2; ++pass) {
      fixture.PinCaret();
      fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
      (void)fixture.shell.ConsumePendingRenderInvalidation();
    }
    SDL_FlushRenderer(fixture.canvas.renderer());
    return SDL_DuplicateSurface(fixture.canvas.surface());
  };
  SDL_DestroySurface(render());
  left->MoveCursorTo(50, 0);
  left->SetScrollLine(40);
  right->SetScrollLine(40);
  SDL_Surface* before = render();
  right->MoveCursorTo(0, 0);
  right->InsertText("int pre;\n");
  right->SetScrollLine(41);
  SDL_Surface* after = render();
  Expect(before != nullptr && after != nullptr, "the two frames should be readable");
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
  std::size_t differing = 0;
  for (int y = static_cast<int>(pane.y) + 2; y < static_cast<int>(pane.y + pane.h) - 2; ++y) {
    const auto* row_before = static_cast<const std::uint8_t*>(before->pixels) + y * before->pitch;
    const auto* row_after = static_cast<const std::uint8_t*>(after->pixels) + y * after->pitch;
    for (int x = static_cast<int>(pane.x) + 2; x < static_cast<int>(pane.x + pane.w) - 2; ++x) {
      differing += std::memcmp(row_before + x * 4, row_after + x * 4, 4) != 0 ? 1 : 0;
    }
  }
  SDL_DestroySurface(before);
  SDL_DestroySurface(after);
  Expect(differing == 0, "the left pane changed when only the right pane's document did: " +
                             std::to_string(differing) + " pixels (its sticky band drew the right pane's lines)");
}

void TestTypingBesideAPluginPresentationRepaintsTheEditedLine() {
  FlowFixture fixture("typing.cpp", OneBlockFile(120));
  auto& state = TestAccess::CurrentProjectState(fixture.shell);
  editor::SurfaceContent chart;
  render::PluginDisplayList list;
  list.content_width = 10.0f;
  list.content_height = 10.0f;
  chart.body = std::move(list);
  state.EnsurePluginPresentation().surfaces.ReplaceForOwnerSurface("plug", "chart", std::move(chart));
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(10, 4);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("a")}), "the first keystroke");
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
  const workspace::RenderInvalidation damage =
      FrameDamage(fixture.shell, fixture.canvas, FlowFixture::kWidth, FlowFixture::kHeight,
                  DispatchEvents(fixture.shell, {TextInputEvent("b")}));
  float covered = 0.0f;
  for (const SDL_FRect& rect : damage.rects) {
    SDL_FRect inside{};
    if (SDL_GetRectIntersectionFloat(&rect, &pane, &inside)) {
      covered += inside.w * inside.h;
    }
  }
  Expect(!damage.full && covered < pane.w * pane.h * 0.2f,
         "a keystroke beside a plugin presentation repaints the edited line, not the pane: " +
             std::to_string(covered) + " of " + std::to_string(pane.w * pane.h) + " px");
}

void TestAnInsetGapThatOpensOrClosesRepaintsTheRowsItMoves() {
  FlowFixture fixture("lensed.cpp", "int greet();\nint second();\nint third();\nint fourth();\n");
  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "plugins.code_lens_above", "true"),
         "lenses should draw above their lines");
  auto& state = TestAccess::CurrentProjectState(fixture.shell);
  editor::SurfaceContent chart;
  render::PluginDisplayList list;
  list.content_width = 10.0f;
  list.content_height = 10.0f;
  chart.body = std::move(list);
  state.EnsurePluginPresentation().surfaces.ReplaceForOwnerSurface("plug", "chart", std::move(chart));
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the file with no lens");
  editor::PluginDecorationData lenses;
  lenses.code_lenses.push_back(editor::CodeLensDecoration{.line = 1, .text = "second (2)", .command = "refs.show"});
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lens-test", fixture.file, std::move(lenses)),
         "publishing the lens should change the store");
  fixture.ExpectFresh(NothingAnnounced(), "a lens strip opened above the second line");
  editor::PluginDecorationData two;
  two.code_lenses.push_back(editor::CodeLensDecoration{.line = 1, .text = "second (2)", .command = "refs.show"});
  two.code_lenses.push_back(editor::CodeLensDecoration{.line = 3, .text = "fourth (1)", .command = "refs.show"});
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lens-test", fixture.file, std::move(two)),
         "publishing a second lens should change the store");
  fixture.ExpectFresh(NothingAnnounced(), "a second strip opened above the fourth line");
  Expect(state.EnsurePluginPresentation().decorations.ClearOwner("lens-test"), "clearing the lenses should change the store");
  fixture.ExpectFresh(NothingAnnounced(), "both strips closed");
}

void TestACaretBlinkRepaintsTheCaretsRowsOnly() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string text = OneBlockFile(40);
  text += std::string(600, 'w') + "\n";
  FlowFixture fixture("blink.cpp", text);
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  const Uint64 interval = TestAccess::CaretBlinkIntervalMs(fixture.shell);
  const std::function<void()> lit = [&] { TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks()); };
  const std::function<void()> dark = [&] {
    TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks() - interval - 50);
  };
  const auto blink = [&](bool to_lit, const std::string& what, float max_rows) {
    (to_lit ? dark : lit)();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    (to_lit ? lit : dark)();
    const workspace::RenderInvalidation wake = TestAccess::HandleScheduledWake(fixture.shell).redraw;
    Expect(wake.frame && !wake.full && wake.rects.empty(), what + ": the blink's wake asks for a frame");
    const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, FlowFixture::kWidth, FlowFixture::kHeight, wake);
    float covered = 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect inside{};
      if (SDL_GetRectIntersectionFloat(&rect, &pane, &inside)) {
        covered += inside.w * inside.h;
      }
    }
    const float line_height = TestAccess::TextLineHeight(fixture.shell) * 1.25f;
    Expect(!damage.full && covered > 0.0f && covered <= pane.w * line_height * max_rows,
           what + ": the blink repaints its caret rows, " + std::to_string(covered) + " px of the pane's " +
               std::to_string(pane.w * pane.h));
    (to_lit ? dark : lit)();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    workspace::RenderInvalidation frame;
    frame.frame = true;
    const auto diff = PaintAndDiffWithFullRepaint(fixture.shell, fixture.canvas, FlowFixture::kWidth,
                                                  FlowFixture::kHeight, frame, to_lit ? lit : dark);
    Expect(diff.identical(), what + ": the blink's frame should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  viewport.MoveCursorTo(5, 0);
  blink(true, "column 0, dark to lit", 1.5f);
  blink(false, "column 0, lit to dark", 1.5f);
  viewport.MoveCursorTo(7, 9);
  viewport.AddSecondaryCaret(12, 4);
  blink(true, "two carets, dark to lit", 3.0f);
  blink(false, "two carets, lit to dark", 3.0f);
  viewport.ClearSecondaryCarets();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "wrap on"), "wrap on should run");
  viewport.MoveCursorTo(42, 300);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  blink(true, "a wrapped line, dark to lit", 1.5f);
  blink(false, "a wrapped line, lit to dark", 1.5f);
}

void TestACaretBlinkDamagesNothingInTheOtherPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.cpp", OneBlockFile(60));
  WriteFile(fixture.root / "b.cpp", OneBlockFile(60, 2));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right b.cpp"), "split-right should open b.cpp beside a.cpp");
  const Uint64 interval = TestAccess::CaretBlinkIntervalMs(fixture.shell);
  for (int pass = 0; pass < 2; ++pass) {
    TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks() - interval - 50);
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  }
  TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks());
  const workspace::RenderInvalidation damage =
      FrameDamage(fixture.shell, fixture.canvas, FlowFixture::kWidth, FlowFixture::kHeight,
                  TestAccess::HandleScheduledWake(fixture.shell).redraw);
  const SDL_FRect other = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);
  const SDL_FRect focused = TestAccess::GroupEditorSurfaceRect(fixture.shell, 1);
  Expect(!damage.full, "a blink in a split does not repaint the window");
  bool in_focused = false;
  for (const SDL_FRect& rect : damage.rects) {
    const SDL_FRect inner{other.x, other.y, other.w - 2.0f, other.h};
    Expect(!SDL_HasRectIntersectionFloat(&rect, &inner),
           "a blink damages nothing in the pane without the keyboard (damaged " + std::to_string(rect.x) + "," +
               std::to_string(rect.y) + " " + std::to_string(rect.w) + "x" + std::to_string(rect.h) + ")");
    in_focused = in_focused || SDL_HasRectIntersectionFloat(&rect, &focused);
  }
  Expect(in_focused, "the blink repaints the caret's row in the focused pane");
}

void TestAnIndentWidthChangeRepaintsTheIndentGuides() {
  FlowFixture fixture("tabs.txt", "\tone\n\t\ttwo\nthree\n\t\t\tfour\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  viewport.SetTabSize(viewport.tab_size() == 8 ? 2 : 8);
  fixture.ExpectFresh(NothingAnnounced(), "the tab size changed");
  viewport.SetIndentWidth(viewport.indent_width() == 8 ? 2 : 8);
  fixture.ExpectFresh(NothingAnnounced(), "the indent width changed");
}

void TestAWrappedLinesRowsKeyOnEveryByteOfIt() {
  std::string line(3000, 'a');
  for (std::size_t i = 9; i < line.size(); i += 10) {
    line[i] = ' ';
  }
  FlowFixture fixture("wrapped.txt", "first\n" + line + "\nlast\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "wrap on"), "wrap on should run");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  editor::EditorViewMetrics metrics;
  metrics.line_height = 18.0f;
  metrics.first_line_y = 100.0f;
  metrics.visible_rows = viewport.visible_lines();
  std::vector<std::uint64_t> keys;
  std::vector<workspace::DerivedDamage::Unit> units;
  const auto row_keys = [&] {
    units.clear();
    workspace::AppendEditorRowUnits(
        workspace::EditorRowKeyInputs{
            .viewport = &viewport, .pane_rect = SDL_FRect{0.0f, 90.0f, 900.0f, 500.0f}, .metrics = metrics, .pane_key = 1},
        keys, units);
    return keys;
  };
  const std::vector<std::uint64_t> before = row_keys();
  Expect(before.size() > 6, "the wrapped line fills several rows");
  for (std::size_t row = 2; row < before.size(); ++row) {
    Expect(before[row] != before[row - 1], "two rows of one line key apart (row " + std::to_string(row) + ")");
  }
  for (const std::size_t column : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{1501},
                                   line.size() - 9, line.size() - 2, line.size() - 1}) {
    viewport.MoveCursorTo(0, 0);
    const std::vector<std::uint64_t> was = row_keys();
    (void)viewport.ReplaceRange(editor::SelectionRange{.start = {.line = 1, .column = column},
                                                       .end = {.line = 1, .column = column + 1}},
                                "Z");
    viewport.MoveCursorTo(0, 0);
    const std::vector<std::uint64_t> now = row_keys();
    Expect(now.size() == was.size() && now[0] == was[0], "the line above keeps its key");
    for (std::size_t row = 1; row + 1 < now.size(); ++row) {
      Expect(now[row] != was[row], "a byte changed at column " + std::to_string(column) + " moves row " +
                                       std::to_string(row) + " of its line");
    }
  }
}

}


namespace {

SDL_Surface* FullFrame(FlowFixture& fixture) {
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  SDL_FlushRenderer(fixture.canvas.renderer());
  return SDL_DuplicateSurface(fixture.canvas.surface());
}

void TestACompareWorkingSideRepaintsItsDiagnosticsAndDecorations() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string left;
  std::string right;
  for (int i = 1; i <= 40; ++i) {
    left += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    right += (i % 7 == 0 ? "long value" : "int value") + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  FlowFixture fixture("left.cpp", left);
  const std::filesystem::path right_path = fixture.root / "right.cpp";
  WriteFile(right_path, right);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + right_path.string()),
         "compare-files should open a diff");
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(0, 0);
  TestAccess::SetFocusEditor(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  Expect(compare.right_editable && !compare.right_viewport.dirty(), "the working side is editable and clean");
  auto& state = TestAccess::CurrentProjectState(fixture.shell);
  const auto diagnostics = [&](std::initializer_list<std::size_t> lines, editor::DiagnosticSeverity severity) {
    std::vector<editor::Diagnostic> squiggles;
    for (const std::size_t line : lines) {
      squiggles.push_back(editor::Diagnostic{
          .range = editor::SelectionRange{.start = {.line = line, .column = 0}, .end = {.line = line, .column = 6}},
          .severity = severity,
          .message = "diff"});
    }
    (void)state.diagnostics_store.ReplaceForOwnerFile("diff-test", compare.right_viewport.path(), std::move(squiggles));
  };
  const auto decorations = [&](SDL_Color colour, editor::GutterIconShape shape) {
    editor::PluginDecorationData data;
    data.text_styles.push_back(
        editor::TextStyleDecoration{.line = 5, .start_column = 0, .end_column = 3, .foreground = colour});
    data.gutter_marks.push_back(editor::GutterMarkDecoration{.line = 11, .shape = shape, .color = colour});
    Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("diff-test", compare.right_viewport.path(),
                                                                          std::move(data)),
           "publishing the decorations should change the store");
  };
  SDL_Surface* plain = FullFrame(fixture);
  diagnostics({3, 9}, editor::DiagnosticSeverity::Error);
  SDL_Surface* with_diagnostics = FullFrame(fixture);
  Expect(DiffSurfaces(plain, with_diagnostics).count > 0, "the working side draws its diagnostics");
  decorations(SDL_Color{230, 60, 90, 255}, editor::GutterIconShape::Dot);
  SDL_Surface* with_decorations = FullFrame(fixture);
  Expect(DiffSurfaces(with_diagnostics, with_decorations).count > 0, "the working side draws its decorations");
  SDL_DestroySurface(plain);
  SDL_DestroySurface(with_diagnostics);
  SDL_DestroySurface(with_decorations);

  diagnostics({3, 15}, editor::DiagnosticSeverity::Error);
  fixture.ExpectFresh(NothingAnnounced(), "a diagnostic moved to another line of the working side");
  diagnostics({3, 15}, editor::DiagnosticSeverity::Warning);
  fixture.ExpectFresh(NothingAnnounced(), "the diagnostics republished at another severity");
  decorations(SDL_Color{60, 200, 120, 255}, editor::GutterIconShape::Dot);
  fixture.ExpectFresh(NothingAnnounced(), "a text style and a gutter mark republished in another colour");
  decorations(SDL_Color{60, 200, 120, 255}, editor::GutterIconShape::Diamond);
  fixture.ExpectFresh(NothingAnnounced(), "the gutter mark republished in another shape");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("x")});
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(compare.right_viewport.dirty(), "the keystroke dirtied the working side");
  fixture.ExpectFresh(NothingAnnounced(), "the first keystroke hid the diagnostics and decorations");
}

void TestAnUnchangedCompareRowReadsTheLeftTokensUntilTheRightWalkHasIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string left;
  std::string right;
  for (int i = 1; i <= 14; ++i) {
    left += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";  // note\n";
    right += (i == 7 ? "long value" : "int value") + std::to_string(i) + " = " + std::to_string(i) + ";  // note\n";
  }
  FlowFixture fixture("left.cpp", left);
  const std::filesystem::path right_path = fixture.root / "right.cpp";
  WriteFile(right_path, right);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + right_path.string()),
         "compare-files should open a diff");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  const auto surface = TestAccess::ActiveCompareSurfaceLayout(fixture.shell);
  const workspace::CompareRowGeometry geometry{.surface = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0),
                                               .rows_y = surface.rows_y,
                                               .line_height = surface.line_height,
                                               .content_width = 600.0f,
                                               .visible_rows = surface.visible_rows};
  const auto keys = [&]() {
    std::vector<workspace::DerivedDamage::Unit> units;
    workspace::AppendCompareRowUnits(compare, geometry, workspace::CompareRowKeyInputs{}, units);
    return units;
  };
  const auto rekeyed = [](const std::vector<workspace::DerivedDamage::Unit>& a,
                          const std::vector<workspace::DerivedDamage::Unit>& b) {
    std::size_t count = a.size() == b.size() ? 0 : a.size() + b.size();
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      count += a[i].fingerprint != b[i].fingerprint ? 1 : 0;
    }
    return count;
  };
  const std::size_t rows = compare.model.rows.size();
  std::vector<bool> aliased(rows);
  std::size_t aliased_count = 0;
  for (std::size_t row = 0; row < rows; ++row) {
    aliased[row] = workspace::CompareRowRightTokensAliasLeft(compare, row) &&
                   !workspace::CompareRowLeftTokens(compare, compare.model.rows[row]).empty();
    aliased_count += aliased[row] ? 1 : 0;
  }
  Expect(aliased_count >= 10, "the unchanged rows read the left window's tokens (" + std::to_string(aliased_count) +
                                  " of " + std::to_string(rows) + ")");
  const std::vector<workspace::DerivedDamage::Unit> caught_up = keys();
  const editor::SyntaxState initial = compare.right_token_window.StateBefore(0);
  const std::size_t right_lines = compare.right_line_rows.size();
  const auto right_line = [&](std::size_t line) -> std::string_view {
    return compare.model.rows[compare.right_line_rows[line]].right_text;
  };
  const auto declines_aliased = [&](std::size_t line) {
    return !workspace::CompareRowRightTokensAliasLeft(compare, compare.right_line_rows[line]);
  };

  compare.right_token_window.Reset(right_lines, initial);
  compare.right_token_window.EnsureWindow(0, right_lines, compare.path,
                                          workspace::SurfaceTokenWindow::WalkBudget{1}, right_line, declines_aliased);
  Expect(compare.right_token_window.frontier() == 1, "the right walk is one row in");
  std::size_t still_aliased = 0;
  for (std::size_t row = 0; row < rows; ++row) {
    still_aliased += aliased[row] && workspace::CompareRowRightTokensAliasLeft(compare, row) ? 1 : 0;
  }
  Expect(still_aliased == aliased_count,
         "behind the left walk, an unchanged row still reads the left window's tokens: " +
             std::to_string(still_aliased) + " of " + std::to_string(aliased_count));
  const std::size_t behind = rekeyed(caught_up, keys());
  Expect(behind <= 1, "and keeps its key: " + std::to_string(behind) + " rows rekeyed (the changed row may)");

  compare.right_token_window.Reset(right_lines, initial);
  compare.right_token_window.EnsureWindow(
      right_lines, right_lines, compare.path, workspace::SurfaceTokenWindow::WalkBudget{right_lines},
      [&](std::size_t line) -> std::string_view { return line == 0 ? std::string_view("/* opened") : right_line(line); },
      declines_aliased);
  Expect(compare.right_token_window.frontier() == right_lines, "the right walk passed every line");
  std::size_t flipped = 0;
  for (std::size_t row = 0; row < rows; ++row) {
    if (aliased[row] && !workspace::CompareRowRightTokensAliasLeft(compare, row)) {
      Expect(workspace::CompareRowRightOwnTokens(compare, compare.model.rows[row]).empty() &&
                 !workspace::CompareRowLeftTokens(compare, compare.model.rows[row]).empty(),
             "its own tokens are none yet, the left's are what they were");
      ++flipped;
    }
  }
  Expect(flipped >= 10, "resumed from another state, those rows read the right window (" + std::to_string(flipped) + ")");
  const std::size_t moved = rekeyed(caught_up, keys());
  Expect(moved >= flipped, "every row whose working side reads another window is rekeyed: " + std::to_string(moved) +
                               " of " + std::to_string(flipped));
}

void TestAMergeResultRepaintsTheFarBracketOfItsPair() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "notes\n");
  std::string side = "int a = 1;\nint b = 2;\nint c = 3;\nvoid run() {\n";
  for (int i = 1; i <= 8; ++i) {
    side += "  int line" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  side += "}\nint d = 4;\n";
  WriteFile(fixture.root / "base.txt", side);
  WriteFile(fixture.root / "incoming.txt", "int head = 1;\n" + side);
  WriteFile(fixture.root / "current.txt", "int head = 2;\n" + side);
  WriteFile(fixture.root / "result.txt", "<<<<<<< HEAD\nint head = 2;\n=======\nint head = 1;\n>>>>>>> in\n" + side);
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  auto& merge = TestAccess::ActiveMerge(fixture.shell);
  editor::TextViewport& result = merge.result_viewport;
  std::size_t opener = 0;
  while (opener < result.line_count() && result.lines().LineView(opener) != "void run() {") {
    ++opener;
  }
  Expect(opener < result.line_count(), "the result holds the block's opener");
  result.MoveCursorTo(opener + 3, 2);
  SDL_Surface* away = FullFrame(fixture);
  result.MoveCursorTo(opener, 11);
  SDL_Surface* on_bracket = FullFrame(fixture);
  const render::PixelDiffResult lit = DiffSurfaces(away, on_bracket);
  SDL_DestroySurface(away);
  SDL_DestroySurface(on_bracket);
  Expect(lit.box_height > 100, "the caret on the opening brace lights its pair, rows apart (changed box " +
                                   std::to_string(lit.box_height) + " px high)");
  result.MoveCursorTo(opener + 3, 2);
  fixture.ExpectFresh(NothingAnnounced(), "the caret left the bracket");
  result.MoveCursorTo(opener, 11);
  fixture.ExpectFresh(NothingAnnounced(), "the caret on the opening brace");
  result.MoveCursorTo(opener + 9, 1);
  fixture.ExpectFresh(NothingAnnounced(), "the caret on the closing brace");
}

}


namespace {

struct PaneDamage {
  bool full = false;
  float share = 0.0f;
};
PaneDamage DamageInPane(FlowFixture& fixture, const workspace::RenderInvalidation& redraw, std::size_t group = 0) {
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, group);
  const workspace::RenderInvalidation damage =
      FrameDamage(fixture.shell, fixture.canvas, FlowFixture::kWidth, FlowFixture::kHeight, redraw);
  PaneDamage out{.full = damage.full};
  for (const SDL_FRect& rect : damage.rects) {
    SDL_FRect inside{};
    if (SDL_GetRectIntersectionFloat(&rect, &pane, &inside)) {
      out.share += inside.w * inside.h / (pane.w * pane.h);
    }
  }
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  return out;
}

void OpenTwoFileCompare(FlowFixture& fixture, int lines) {
  std::string right;
  for (int i = 1; i <= lines; ++i) {
    right += (i % 7 == 0 ? "long value" : "int value") + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  const std::filesystem::path right_path = fixture.root / "right.cpp";
  WriteFile(right_path, right);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + right_path.string()),
         "compare-files should open a diff");
  TestAccess::SetFocusEditor(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
}

std::string CompareLeftText(int lines) {
  std::string left;
  for (int i = 1; i <= lines; ++i) {
    left += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  return left;
}

void TestACompareTabsActionsRepaintTheirRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.cpp", CompareLeftText(60));
  OpenTwoFileCompare(fixture, 60);
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  const SDL_FRect pane = TestAccess::GroupEditorSurfaceRect(fixture.shell, 0);

  compare.right_view_active = false;
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the diff with its working side inactive");
  const std::size_t selected = compare.selected_row;
  workspace::RenderInvalidation step = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)});
  Expect(compare.selected_row != selected, "Down moves the diff's selection");
  Expect(step.frame && !step.full && step.rects.empty(), "a selection step asks for a frame and names no rect");
  PaneDamage damage = DamageInPane(fixture, step);
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.15f,
         "a selection step repaints the two rows it moved between: " + std::to_string(damage.share) + " of the pane");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "another selection step");

  const auto click = [&](float y) {
    const float x = pane.x + pane.w * 0.75f;
    return DispatchEvents(fixture.shell, {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                          MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  };
  fixture.ExpectFresh(click(pane.y + 200.0f), "a click into the working side");
  Expect(compare.right_view_active, "the click activated the working side");
  damage = DamageInPane(fixture, click(pane.y + 290.0f));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.25f,
         "a click in the active working side repaints the rows its caret left and took: " +
             std::to_string(damage.share) + " of the pane");
  fixture.ExpectFresh(click(pane.y + 326.0f), "a third click there");

  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "the first keystroke");
  workspace::RenderInvalidation typed = DispatchEvents(fixture.shell, {TextInputEvent("y")});
  Expect(!typed.full, "a keystroke in the working side asks for no full repaint");
  damage = DamageInPane(fixture, typed);
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.12f,
         "a keystroke in the working side repaints its row: " + std::to_string(damage.share) + " of the pane");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("z")}), "a third keystroke");
  const auto edit_repaints = [&](const std::string& what, float most, const workspace::RenderInvalidation& redraw) {
    Expect(!redraw.full, what + " asks for no full repaint");
    const PaneDamage edit = DamageInPane(fixture, redraw);
    Expect(!edit.full && edit.share > 0.0f && edit.share < most,
           what + " in the working side repaints what it changed: " + std::to_string(edit.share) + " of the pane");
  };
  edit_repaints("undo", 0.4f, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}));
  edit_repaints("redo", 0.4f, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Y, SDL_KMOD_CTRL)}));
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}), "undo, against a full repaint");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Y, SDL_KMOD_CTRL)}), "redo, against a full repaint");
  SDL_SetClipboardText("pasted");
  edit_repaints("paste", 0.12f, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_V, SDL_KMOD_CTRL)}));
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_V, SDL_KMOD_CTRL)}), "paste, against a full repaint");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "Return in the working side");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "diff-show-whitespace"), "diff-show-whitespace should run");
  workspace::RenderInvalidation toggled = fixture.shell.ConsumePendingRenderInvalidation();
  Expect(toggled.frame && !toggled.full && toggled.rects.empty(), "show-whitespace asks for a frame and names no rect");
  fixture.ExpectFresh(toggled, "whitespace shown");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "diff-ignore-whitespace"), "diff-ignore-whitespace should run");
  toggled = fixture.shell.ConsumePendingRenderInvalidation();
  Expect(!toggled.full && toggled.rects.empty(), "ignore-whitespace names no rect");
  fixture.ExpectFresh(toggled, "whitespace ignored");
}

void TestANewChangeInADiffRepaintsItsRowsNotTheChangesBelow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.cpp", CompareLeftText(60));
  OpenTwoFileCompare(fixture, 60);
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  compare.right_view_active = true;
  compare.right_viewport.MoveCursorTo(8, 4);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the caret on a line both sides agree on");
  PaneDamage damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {TextInputEvent("q")}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.4f,
         "a keystroke that makes a new change repaints that change, not the rows below it: " +
             std::to_string(damage.share) + " of the pane");
  damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {TextInputEvent("r")}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.12f,
         "the next keystroke there repaints its row: " + std::to_string(damage.share) + " of the pane");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN), KeyDownEvent(SDLK_DOWN)}), "two rows down");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("s")}), "a second new change");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}), "the change undone");
}

void TestScrollingADiffLeavesThePaneBesideItAlone() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.cpp", CompareLeftText(120));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right left.cpp"), "split-right should open a second pane");
  OpenTwoFileCompare(fixture, 120);
  auto& compare = TestAccess::ActiveCompare(fixture.shell);
  const std::size_t diff_group = TestAccess::CurrentProjectState(fixture.shell).clamped_focused_group_index();
  const std::size_t other_group = diff_group == 0 ? 1 : 0;
  const SDL_FRect other = TestAccess::GroupEditorSurfaceRect(fixture.shell, other_group);
  const SDL_FRect diff = TestAccess::GroupEditorSurfaceRect(fixture.shell, diff_group);
  for (const int row : {5, 20, 8}) {
    compare.scroll_row = row;
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, FlowFixture::kWidth, FlowFixture::kHeight, NothingAnnounced());
    Expect(!damage.full, "a diff's scroll does not repaint the window");
    bool in_diff = false;
    for (const SDL_FRect& rect : damage.rects) {
      const SDL_FRect inner{other.x + 2.0f, other.y, other.w - 4.0f, other.h};
      Expect(!SDL_HasRectIntersectionFloat(&rect, &inner),
             "a diff's scroll repaints nothing in the pane beside it (damaged " + std::to_string(rect.x) + "," +
                 std::to_string(rect.y) + " " + std::to_string(rect.w) + "x" + std::to_string(rect.h) + ")");
      in_diff = in_diff || SDL_HasRectIntersectionFloat(&rect, &diff);
    }
    Expect(in_diff, "the scroll repaints the diff");
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    compare.scroll_row = row + 1;
    fixture.ExpectFresh(NothingAnnounced(), "the diff scrolled one more row");
  }
}

void TestAMergeTabsActionsRepaintWhatTheyChange() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "notes\n");
  std::string side;
  for (int i = 1; i <= 6; ++i) {
    side += "int line" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(fixture.root / "base.txt", side);
  WriteFile(fixture.root / "incoming.txt", "int head = 1;\n" + side + "int tail = 9;\n");
  WriteFile(fixture.root / "current.txt", "int head = 2;\n" + side + "int tail = 8;\n");
  WriteFile(fixture.root / "result.txt", "<<<<<<< HEAD\nint head = 2;\n=======\nint head = 1;\n>>>>>>> in\n" + side +
                                             "<<<<<<< HEAD\nint tail = 8;\n=======\nint tail = 9;\n>>>>>>> in\n");
  Expect(TestAccess::OpenMergeEditor(fixture.shell, fixture.root / "base.txt", fixture.root / "incoming.txt",
                                     fixture.root / "current.txt", fixture.root / "result.txt"),
         "the merge editor opens");
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the merge editor");
  auto& merge = TestAccess::ActiveMerge(fixture.shell);
  Expect(merge.conflicts.size() == 2, "the result holds two conflicts, both in view");
  const auto frame_of = [&](const std::string& what) {
    const workspace::RenderInvalidation redraw = fixture.shell.ConsumePendingRenderInvalidation();
    Expect(redraw.frame && !redraw.full && redraw.rects.empty(), what + " asks for a frame and names no rect");
    return redraw;
  };
  TestAccess::MoveMergeSelection(fixture.shell, 1);
  fixture.ExpectFresh(frame_of("the next conflict"), "the next conflict selected");
  TestAccess::MoveMergeSelection(fixture.shell, -1);
  fixture.ExpectFresh(frame_of("the previous conflict"), "the previous conflict selected");
  TestAccess::MarkMergeResolved(fixture.shell);
  fixture.ExpectFresh(frame_of("Mark Resolved with conflicts left"), "Mark Resolved said how many are left");
  TestAccess::ApplyMergeChoice(fixture.shell, compare::MergeChoice::Incoming);
  fixture.ExpectFresh(frame_of("accepting a side"), "a side accepted");
  TestAccess::ToggleMergeBasePane(fixture.shell);
  fixture.ExpectFresh(frame_of("the base pane"), "the base pane toggled");
  TestAccess::ToggleMergeBasePane(fixture.shell);
  fixture.ExpectFresh(frame_of("the base pane"), "the base pane toggled back");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "the first keystroke in the result");
  PaneDamage damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {TextInputEvent("y")}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.1f,
         "a keystroke in the result repaints its row: " + std::to_string(damage.share) + " of the pane");
  damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.35f,
         "undo in the result repaints its row and the header: " + std::to_string(damage.share) + " of the pane");
  damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Y, SDL_KMOD_CTRL)}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.35f,
         "redo in the result repaints its row and the header: " + std::to_string(damage.share) + " of the pane");
  SDL_SetClipboardText("pasted");
  damage = DamageInPane(fixture, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_V, SDL_KMOD_CTRL)}));
  Expect(!damage.full && damage.share > 0.0f && damage.share < 0.1f,
         "a paste in the result repaints its row: " + std::to_string(damage.share) + " of the pane");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)}), "undo, against a full repaint");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Y, SDL_KMOD_CTRL)}), "redo, against a full repaint");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_V, SDL_KMOD_CTRL)}), "paste, against a full repaint");
}

}


namespace {

struct TreeFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};

  explicit TreeFixture(int files_per_folder = 6) {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    WriteFile(root / "notes.txt", "notes\n");
    for (const char* folder : {"alpha", "beta", "gamma"}) {
      for (int i = 0; i < files_per_folder; ++i) {
        WriteFile(root / folder / ("file" + std::to_string(i) + ".cpp"), "int x;\n");
      }
    }
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the tree fixture should open its project");
    TestAccess::SetWindowSize(shell, kWidth, kHeight);
    TestAccess::OpenSingleEditorTab(shell, root / "notes.txt");
    TestAccess::ShowTreeSidebar(shell);
    TestAccess::SetFocusEditor(shell);
    TestAccess::NoteLayoutInputsChanged(shell);
    Paint();
  }

  project::DirectoryTree& tree() { return TestAccess::CurrentProjectState(shell).directory_tree; }

  void Paint() {
    TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  void ExpectFresh(const workspace::RenderInvalidation& redraw, const std::string& what) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, [this]() {
      TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
    });
    Expect(diff.identical(), what + " should paint what a full repaint paints; " + DescribePixelDiff(diff));
  }

  float SidebarShare(const workspace::RenderInvalidation& redraw, bool* full = nullptr) {
    const SDL_FRect sidebar = TestAccess::CurrentLayout(shell).sidebar;
    TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
    const workspace::RenderInvalidation damage = FrameDamage(shell, canvas, kWidth, kHeight, redraw);
    if (full != nullptr) {
      *full = damage.full;
    }
    float share = 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect inside{};
      if (SDL_GetRectIntersectionFloat(&rect, &sidebar, &inside)) {
        share += inside.w * inside.h / (sidebar.w * sidebar.h);
      }
    }
    Paint();
    return share;
  }
};

void TestTheFileTreesRowsRepaintWhatChangedWithNoDamageRequest() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TreeFixture fixture;
  project::DirectoryTree& tree = fixture.tree();
  Expect(tree.entries().size() >= 4, "the tree shows the project's folders and file");

  tree.SetSelectedIndex(1);
  fixture.ExpectFresh(NothingAnnounced(), "the tree's selection moved");
  tree.SetSelectedIndex(2);
  bool full = false;
  const float moved = fixture.SidebarShare(NothingAnnounced(), &full);
  Expect(!full && moved > 0.0f && moved < 0.15f,
         "a selection that moves repaints the row it left and the row it took: " + std::to_string(moved) +
             " of the sidebar");

  Expect(tree.SelectPath(fixture.root / "alpha"), "the first folder is in the tree");
  fixture.Paint();
  const std::size_t closed_count = tree.entries().size();
  tree.ExpandSelection();
  Expect(tree.entries().size() >= closed_count + 6, "the opened folder lists its files");
  fixture.ExpectFresh(NothingAnnounced(), "a folder opened");
  tree.CollapseSelection();
  fixture.ExpectFresh(NothingAnnounced(), "the folder closed");
  tree.ExpandSelection();
  fixture.Paint();

  auto statuses = std::make_shared<project::GitTreeStatusMap>();
  (*statuses)["alpha/file2.cpp"] = project::GitFileStatus::Modified;
  (*statuses)["alpha"] = project::GitFileStatus::Modified;
  Expect(tree.ApplyGitStatuses(statuses), "the statuses change a badge");
  full = false;
  const float badged = fixture.SidebarShare(NothingAnnounced(), &full);
  Expect(!full && badged > 0.0f && badged < 0.15f,
         "a git badge repaints its file's row and its folder's: " + std::to_string(badged) + " of the sidebar");
  auto more = std::make_shared<project::GitTreeStatusMap>(*statuses);
  (*more)["alpha/file4.cpp"] = project::GitFileStatus::Untracked;
  Expect(tree.ApplyGitStatuses(more), "a second badge");
  fixture.ExpectFresh(NothingAnnounced(), "a second git badge");
  Expect(tree.ApplyGitStatuses(nullptr), "the badges clear");
  fixture.ExpectFresh(NothingAnnounced(), "the git badges cleared");

  tree.CollapseAll();
  fixture.ExpectFresh(NothingAnnounced(), "every folder closed (the Collapse button dims)");

  const std::optional<SDL_FRect> row = TestAccess::TreeSidebarRowRect(fixture.shell, 2);
  Expect(row.has_value(), "the third row is on screen");
  if (row.has_value()) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(row->x + row->w / 2.0f, row->y + row->h / 2.0f)}),
                        "the pointer onto a tree row");
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(900.0f, 400.0f)}),
                        "the pointer off the tree");
  }

  TestAccess::ShowSearchSidebar(fixture.shell, "", false);
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched to Search");
  TestAccess::ShowTreeSidebar(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched back to the tree");
}

void TestTheFileTreeFollowsItsScrollAndARefresh() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TreeFixture fixture(30);
  project::DirectoryTree& tree = fixture.tree();
  Expect(tree.SelectPath(fixture.root / "alpha"), "the first folder is in the tree");
  tree.ExpandSelection();
  fixture.Paint();
  Expect(tree.entries().size() > 30, "the opened folder overflows the sidebar");
  auto& sidebar = TestAccess::CurrentProjectState(fixture.shell).sidebar;
  sidebar.scroll_row = 5;
  fixture.ExpectFresh(NothingAnnounced(), "the tree scrolled five rows");
  sidebar.scroll_row = 6;
  fixture.ExpectFresh(NothingAnnounced(), "the tree scrolled one more row");
  sidebar.scroll_row = 0;
  fixture.ExpectFresh(NothingAnnounced(), "the tree scrolled back to its top");
  WriteFile(fixture.root / "alpha" / "added.cpp", "int y;\n");
  tree.Refresh();
  fixture.ExpectFresh(NothingAnnounced(), "a file appeared and the tree refreshed");
}

void TestAGitWakeWithProblemsInFrontRepaintsNothingInTheSidebar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TreeFixture fixture(0);
  InitializeGitRepo(fixture.root);
  CommitAll(fixture.root, "base", "base");
  WriteFile(fixture.root / "notes.txt", "changed\n");
  TestAccess::ShowGitSidebar(fixture.shell);
  Expect(WaitForGitSidebarEntryCount(fixture.shell, 1), "the git view lists the change");
  TestAccess::ShowProblemsSidebar(fixture.shell);
  fixture.Paint();
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::ConsumeGitSidebarRefresh(fixture.shell);
  const workspace::RenderInvalidation asked = fixture.shell.ConsumePendingRenderInvalidation();
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  bool touches = asked.full;
  for (const SDL_FRect& rect : asked.rects) {
    SDL_FRect inside{};
    touches = touches || SDL_GetRectIntersectionFloat(&rect, &sidebar, &inside);
  }
  Expect(!touches, "a git wake with Problems in front asks for none of the sidebar");
}

void TestTheGitViewsSelectionAndScrollRepaintTheirRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TreeFixture fixture(0);
  for (int i = 0; i < 60; ++i) {
    WriteFile(fixture.root / ("f" + std::to_string(i) + ".txt"), "base\n");
  }
  InitializeGitRepo(fixture.root);
  CommitAll(fixture.root, "base", "base");
  for (int i = 0; i < 60; ++i) {
    WriteFile(fixture.root / ("f" + std::to_string(i) + ".txt"), "changed\n");
  }
  TestAccess::ShowGitSidebar(fixture.shell);
  Expect(WaitForGitSidebarEntryCount(fixture.shell, 60), "the git view lists the sixty changed files");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.Paint();
  auto& sidebar = TestAccess::CurrentProjectState(fixture.shell).sidebar;

  sidebar.git.selected_index = 3;
  fixture.ExpectFresh(NothingAnnounced(), "the git view's selection moved");
  sidebar.git.selected_index = 4;
  bool full = false;
  const float moved = fixture.SidebarShare(NothingAnnounced(), &full);
  Expect(!full && moved > 0.0f && moved < 0.15f,
         "a selection step repaints the row it left and the row it took: " + std::to_string(moved) +
             " of the sidebar");

  sidebar.scroll_row = 1;
  fixture.ExpectFresh(NothingAnnounced(), "the git view scrolled a row");
  sidebar.scroll_row = 5;
  fixture.ExpectFresh(NothingAnnounced(), "the git view scrolled four more");
  sidebar.scroll_row = 0;
  fixture.ExpectFresh(NothingAnnounced(), "the git view scrolled back to its top");

  TestAccess::SetFocusSidebar(fixture.shell);
  fixture.ExpectFresh(workspace::RenderInvalidation{.full = true}, "the sidebar focused");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN), KeyDownEvent(SDLK_DOWN)}),
                      "Down twice in the git list");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)}), "End in the git list");
  WriteFile(fixture.root / "added.txt", "new\n");
  RequireGitCommandSuccess(fixture.root, {"add", "f0.txt"}, "git add f0.txt");
  TestAccess::RefreshGitSidebar(fixture.shell);
  Expect(WaitForGitSidebarEntryCount(fixture.shell, 61), "the refresh lists the new file, and f0.txt staged");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "a refresh landed with one more file");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "git-commit"), "the commit form opens");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the commit form opened");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("subject")}), "a subject typed");

  TestAccess::ShowTreeSidebar(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched to the tree");
  TestAccess::ShowGitSidebar(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched back to Source Control");
}

}

namespace {

void TestTheSidebarsModeRowRepaintsWhatChangedWithNoDamageRequest() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TreeFixture fixture;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SidebarModeRow) >= 3,
         "the mode row lays its tabs out as units");
  TestAccess::ShowSearchSidebar(fixture.shell, "", false);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched to Search");
  TestAccess::ShowTreeSidebar(fixture.shell);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched back to the tree");
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(sidebar.x + 30.0f, sidebar.y + 12.0f)}),
                      "the pointer onto a mode tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(900.0f, 400.0f)}), "the pointer off the mode row");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "sidebar-show problems"), "the Problems view should show");
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the sidebar switched to a view behind the overflow");
  TestAccess::ShowTreeSidebar(fixture.shell);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "and back to the tree");
}

void TestADerivedSidebarViewCommitsItsSurfacesUnits() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  using workspace::SidebarMode;
  for (const SidebarMode mode : {SidebarMode::Tree, SidebarMode::Search, SidebarMode::Problems, SidebarMode::Git,
                                 SidebarMode::Tests, SidebarMode::Plugin, SidebarMode::Outline}) {
    Expect(workspace::SidebarViewSurface(mode) != workspace::DerivedSurface::Count,
           "every sidebar view names the surface it derives its damage on");
    TreeFixture fixture;
    switch (mode) {
      case SidebarMode::Tree:
        TestAccess::ShowTreeSidebar(fixture.shell);
        break;
      case SidebarMode::Git:
        TestAccess::ShowGitSidebar(fixture.shell);
        break;
      case SidebarMode::Search:
        TestAccess::ShowSearchSidebar(fixture.shell, "", false);
        break;
      case SidebarMode::Problems:
        TestAccess::ShowProblemsSidebar(fixture.shell);
        break;
      case SidebarMode::Tests:
        TestAccess::ShowTestsSidebar(fixture.shell);
        break;
      case SidebarMode::Outline:
        Expect(TestAccess::ExecuteCommandLine(fixture.shell, "sidebar-show outline"), "the outline view shows");
        break;
      case SidebarMode::Plugin:
        Expect(workspace::SidebarViewSurface(mode) == workspace::SidebarViewSurface(SidebarMode::Outline),
               "a plugin item view is the shared list's");
        continue;
      default:
        Expect(false, "a sidebar view this test cannot show: teach it how");
        continue;
    }
    fixture.Paint();
    Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::SidebarViewSurface(mode)) > 0,
           "a sidebar view said to be derived lays units out on its surface");
    Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SidebarModeRow) > 0,
           "and its mode row does");
  }

  TreeFixture fixture;
  TestAccess::SetFocusSidebar(fixture.shell);
  fixture.Paint();
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  const workspace::RenderInvalidation step = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)});
  Expect(!step.full && step.frame, "a tree key asks for a frame");
  for (const SDL_FRect& rect : step.rects) {
    Expect(!(rect.w >= sidebar.w - 1.0f && rect.h >= sidebar.h - 1.0f),
           "a tree key names no rect the size of the sidebar");
  }
  bool full = false;
  const float moved = fixture.SidebarShare(step, &full);
  Expect(!full && moved > 0.0f && moved < 0.15f,
         "a tree key repaints the rows its selection moved between: " + std::to_string(moved) + " of the sidebar");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "another tree key");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHT)}), "a tree key opening a folder");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_LEFT)}), "a tree key closing it");

  TestAccess::ShowSearchSidebar(fixture.shell, "", false);
  fixture.Paint();
  const workspace::RenderInvalidation in_search = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)});
  fixture.ExpectFresh(in_search, "a key in the Search view");
}

}

namespace {

struct MenuRowProbe {
  std::string name;
  SDL_FRect rect{};
  bool enabled = false;
};

std::vector<MenuRowProbe> OpenMenuRows(FlowFixture& fixture, std::string_view menu) {
  std::vector<MenuRowProbe> rows;
  const std::string prefix = "menu-item:" + std::string(menu) + "/";
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string name = entry["name"].AsString();
    if (!name.starts_with(prefix)) {
      continue;
    }
    const auto& r = entry["rect"].AsArray();
    rows.push_back(MenuRowProbe{.name = name,
                                .rect = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                                                  static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())},
                                .enabled = entry["enabled"].AsBool()});
  }
  return rows;
}

void TestAnOpenMenusRowsRepaintWhatChanged() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 840, 600);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) == 0,
         "no menu open: the popup lays out no units");
  const auto file = fixture.ElementRect("menu:File");
  Expect(file.has_value(), "the menu bar shows File");
  if (!file) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*file), "the File menu opened");
  std::vector<MenuRowProbe> rows = OpenMenuRows(fixture, "File");
  Expect(rows.size() >= 4, "the File menu lists its rows");
  if (rows.size() < 4) {
    return;
  }
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) > rows.size(),
         "the open popup lays out its card and a unit per row");

  const SDL_FRect popup = SDL_FRect{rows.front().rect.x, rows.front().rect.y, rows.front().rect.w,
                                    rows.back().rect.y + rows.back().rect.h - rows.front().rect.y};
  const auto onto = [&](const MenuRowProbe& row) {
    return DispatchEvents(fixture.shell, {MouseMotionEvent(row.rect.x + 12.0f, row.rect.y + row.rect.h * 0.5f)});
  };
  fixture.ExpectFresh(onto(rows[0]), "the pointer onto the first row");
  fixture.ExpectFresh(onto(rows[1]), "the pointer onto the second row");
  const auto popup_share = [&](const workspace::RenderInvalidation& redraw) {
    fixture.PinCaret();
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, redraw);
    float covered = damage.full ? popup.w * popup.h : 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect inside{};
      if (SDL_GetRectIntersectionFloat(&rect, &popup, &inside)) {
        covered += inside.w * inside.h;
      }
    }
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    return covered / (popup.w * popup.h);
  };
  const float step = popup_share(onto(rows[2]));
  Expect(step > 0.0f && step < 0.5f,
         "a pointer step repaints the rows it moved between, not the popup: " + std::to_string(step) + " of it");
  const workspace::RenderInvalidation within =
      DispatchEvents(fixture.shell, {MouseMotionEvent(rows[2].rect.x + 40.0f, rows[2].rect.y + rows[2].rect.h * 0.5f)});
  Expect(!within.full && !within.frame && within.rects.empty(),
         "the pointer moving within its menu row asks for no frame (full=" + std::to_string(within.full) +
             " frame=" + std::to_string(within.frame) + " rects=" + std::to_string(within.rects.size()) + ")");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(700.0f, 500.0f)}), "the pointer off the menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "Down moves the active row");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "and again");
  const float key = popup_share(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}));
  Expect(key > 0.0f && key < 0.5f,
         "a key in the menu repaints the rows it moved between, not the popup: " + std::to_string(key) + " of it");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)}), "End takes the last row");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_HOME)}), "Home takes the first");
  {
    const workspace::RenderInvalidation swallowed = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F9)});
    fixture.PinCaret();
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, swallowed);
    Expect(!damage.full && damage.rects.empty(),
           "a key the open menu swallows repaints nothing (full=" + std::to_string(damage.full) + " rects=" +
               std::to_string(damage.rects.size()) + ")");
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  }

  const auto revert_row = [&rows]() {
    return std::find_if(rows.begin(), rows.end(), [](const MenuRowProbe& row) {
      return row.name.find("Revert File") != std::string::npos;
    });
  };
  Expect(revert_row() != rows.end() && !revert_row()->enabled, "Revert File is dimmed while the file is clean");
  TestAccess::ActiveEditor(fixture.shell).InsertText("x");
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "an edit under the open menu enabled Revert File");
  rows = OpenMenuRows(fixture, "File");
  Expect(revert_row() != rows.end() && revert_row()->enabled, "Revert File is enabled now");
  TestAccess::ActiveEditor(fixture.shell).SetDirty(false);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const float dimmed = popup_share(NothingAnnounced());
  Expect(dimmed > 0.0f && dimmed < 0.5f,
         "the row that changed repaints, not the popup: " + std::to_string(dimmed) + " of it");

  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the menu");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) == 0,
         "closed again: no units");

  const auto edit = fixture.ElementRect("menu:Edit");
  Expect(edit.has_value(), "the menu bar shows Edit");
  if (!edit) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*edit), "the Edit menu opened");
  fixture.ExpectFresh(onto(rows[3]), "the pointer at rest where File's fourth row will open");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_LEFT, SDL_KMOD_ALT)}),
                      "Alt+Left opening File under the still pointer");
  Expect(!OpenMenuRows(fixture, "File").empty(), "Alt+Left opened the File menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(700.0f, 500.0f)}),
                      "the pointer left the row it alone had lit");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the File menu");
}

void TestAnOpenMenusRowsRepaintAtFractionalScales() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const std::string scale : {"1.25", "1.6", "2.25"}) {
    FlowFixture fixture("notes.txt", "one\ntwo\n", 1600, 1000);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale " + scale),
           "set-setting ui.scale should run");
    const workspace::RenderInvalidation full{.full = true};
    fixture.ExpectFresh(full, "the file in its tab at scale " + scale);
    const auto file = fixture.ElementRect("menu:File");
    Expect(file.has_value(), "the menu bar shows File at scale " + scale);
    if (!file) {
      return;
    }
    fixture.ExpectFresh(fixture.Click(*file), "the File menu opened at scale " + scale);
    const std::vector<MenuRowProbe> rows = OpenMenuRows(fixture, "File");
    Expect(rows.size() >= 4, "the File menu lists its rows at scale " + scale);
    for (const MenuRowProbe& row : rows) {
      fixture.ExpectFresh(
          DispatchEvents(fixture.shell, {MouseMotionEvent(row.rect.x + 12.0f, row.rect.y + row.rect.h * 0.5f)}),
          "the pointer onto " + row.name + " at scale " + scale);
    }
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(1200.0f, 800.0f)}),
                        "the pointer off the menu at scale " + scale);
    for (int key = 0; key < 3; ++key) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}),
                          "Down in the menu at scale " + scale);
    }
    TestAccess::ActiveEditor(fixture.shell).InsertText("x");
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    fixture.ExpectFresh(NothingAnnounced(), "an edit enabled Revert File at scale " + scale);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}),
                        "Escape closed the menu at scale " + scale);
  }
}

void TestAMenuOpeningSwitchingAndClosingRepaintsItsBarItems() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 840, 600);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab");
  const auto file = fixture.ElementRect("menu:File");
  const auto edit = fixture.ElementRect("menu:Edit");
  const auto view = fixture.ElementRect("menu:View");
  Expect(file.has_value() && edit.has_value() && view.has_value(), "the menu bar shows File, Edit and View");
  if (!file || !edit || !view) {
    return;
  }
  const SDL_FRect bar = TestAccess::CurrentLayout(fixture.shell).menu_bar;
  const auto bar_share = [&](const workspace::RenderInvalidation& redraw) {
    fixture.PinCaret();
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, redraw);
    float covered = damage.full ? bar.w * bar.h : 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect inside{};
      if (SDL_GetRectIntersectionFloat(&rect, &bar, &inside)) {
        covered += inside.w * inside.h;
      }
    }
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    return covered / (bar.w * bar.h);
  };
  const auto onto = [](const SDL_FRect& rect) { return MouseMotionEvent(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f); };

  fixture.ExpectFresh(DispatchEvents(fixture.shell, {onto(*view)}), "the pointer onto View, no menu open");
  fixture.ExpectFresh(fixture.Click(*file), "a click opened File");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {onto(*edit)}), "the pointer switched to Edit");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHT)}), "Right switched to the next menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_LEFT)}), "Left switched back");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the menu");
  fixture.ExpectFresh(fixture.Click(*file), "a click opened File again");
  fixture.ExpectFresh(fixture.Click(*file), "a click on its item closed it");
  fixture.ExpectFresh(fixture.Click(*file), "a click opened File once more");
  fixture.ExpectFresh(fixture.Click(SDL_FRect{700.0f, 520.0f, 2.0f, 2.0f}), "a press outside closed it");
  Expect(!TestAccess::MenuBarOpen(fixture.shell), "the press outside closed the menu");
  const auto metrics = TestAccess::ActiveEditorMetrics(fixture.shell);
  const float text_x = metrics.text_x + TestAccess::TextCharWidth(fixture.shell) * 2.0f;
  const float text_y = metrics.first_line_y + metrics.line_height * 0.5f;
  SDL_Event right_down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, text_x, text_y);
  SDL_Event right_up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, text_x, text_y);
  right_down.button.button = SDL_BUTTON_RIGHT;
  right_up.button.button = SDL_BUTTON_RIGHT;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(text_x, text_y), right_down}),
                      "a right press opened the editor's menu");
  Expect(TestAccess::EditorContextMenuOpen(fixture.shell), "the editor's menu is open");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {right_up}), "the right button released");
  Expect(TestAccess::EditorContextMenuOpen(fixture.shell) &&
             TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) > 1,
         "the editor's menu stays open, its card and rows laid out");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed it");

  const float opened = bar_share(fixture.Click(*file));
  Expect(opened > 0.0f && opened < 0.3f,
         "a menu opening repaints its bar item, not the bar: " + std::to_string(opened) + " of it");
  const float switched = bar_share(DispatchEvents(fixture.shell, {onto(*edit)}));
  Expect(switched > 0.0f && switched < 0.3f,
         "a menu switching repaints the two items: " + std::to_string(switched) + " of the bar");
  const float closed = bar_share(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}));
  Expect(closed > 0.0f && closed < 0.3f,
         "a menu closing repaints its bar item: " + std::to_string(closed) + " of the bar");
}

void TestTheDebugToolbarFollowsItsSessionFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "reverse_late"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  const auto pump = [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); };
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the mock adapter should stop on configurationDone");
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the first stop");
  const auto toolbar_buttons = [&]() {
    std::size_t count = 0;
    const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
    for (const util::JsonValue& element : ui["elements"].AsArray()) {
      count += element["name"].AsString().starts_with("button:debug-toolbar/") ? 1 : 0;
    }
    return count;
  };
  const std::size_t buttons = toolbar_buttons();
  Expect(buttons >= 5 && !fixture.ElementRect("button:debug-toolbar/step-back").has_value(),
         "the toolbar shows, without the reverse buttons");
  const auto asked = [&]() {
    workspace::RenderInvalidation redraw = fixture.shell.ConsumePendingRenderInvalidation();
    Expect(!redraw.full, "a debug callback asks for no full repaint");
    return redraw;
  };

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-repl"), "debug-repl should open its prompt");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("record"), KeyDownEvent(SDLK_RETURN)});
  fixture.ExpectFresh(full, "the evaluate sent");
  Expect(WaitUntil([&]() { return toolbar_buttons() > buttons; }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the late capabilities event adds the reverse buttons");
  fixture.ExpectFresh(asked(), "the toolbar grew by its reverse buttons");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-continue"), "debug-continue should run");
  fixture.ExpectFresh(asked(), "the session resumed");
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the mock adapter stops again");
  fixture.ExpectFresh(asked(), "the session stopped again");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-stop"), "debug-stop should run");
  Expect(WaitUntil([&]() { return !TestAccess::DebugSessionActive(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the session ends");
  fixture.ExpectFresh(asked(), "the toolbar left with its session");
  Expect(toolbar_buttons() == 0, "no toolbar without a session");
}

void TestADebugSessionsCallbacksRepaintNoWindowWhileTheDockIsClosed() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "evaluate"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  const auto pump = [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); };
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the mock adapter should stop on configurationDone");
  for (int round = 0; round < 20; ++round) {
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TestAccess::SetDebugPaneVisible(fixture.shell, false);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "stopped, the debug dock closed");
  const float window = static_cast<float>(fixture.width) * static_cast<float>(fixture.height);

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-repl"), "debug-repl should open its prompt");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("set print elements 200"), KeyDownEvent(SDLK_RETURN)});
  fixture.ExpectFresh(full, "the expression sent");
  workspace::RenderInvalidation asked;
  for (int round = 0; round < 40; ++round) {
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    asked.Merge(fixture.shell.ConsumePendingRenderInvalidation());
  }
  Expect(!TestAccess::CurrentProjectState(fixture.shell).debug_pane.visible, "the dock stays closed");
  float largest = 0.0f;
  for (const SDL_FRect& rect : asked.rects) {
    largest = std::max(largest, rect.w * rect.h);
  }
  Expect(asked.frame, "the session's callbacks owe a frame");
  Expect(!asked.full && largest < window * 0.5f,
         "and name no window-sized rect while the dock is closed: the largest is " +
             std::to_string(largest / window) + " of the window (" + std::to_string(asked.rects.size()) + " rects)");
  fixture.ExpectFresh(asked, "an evaluate and the output it printed, the dock closed");
}

void TestAClosedBottomPanelAsksForNoWindow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  TestAccess::HideBottomPanel(fixture.shell);
  TestAccess::SetDebugPaneVisible(fixture.shell, true);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the panel closed");
  TestAccess::RequestPreparedFrame(fixture.shell);
  const workspace::RenderInvalidation closed = fixture.shell.ConsumePendingRenderInvalidation();
  Expect(closed.frame && !closed.full && closed.rects.empty(),
         "a closed panel's change owes a frame and names no rect (rects=" + std::to_string(closed.rects.size()) + ")");

  const float window = static_cast<float>(fixture.width) * static_cast<float>(fixture.height);
  const auto largest_named = [&](const workspace::RenderInvalidation& asked) {
    float largest = asked.full ? window : 0.0f;
    for (const SDL_FRect& rect : asked.rects) {
      largest = std::max(largest, rect.w * rect.h);
    }
    return largest / window;
  };
  const std::filesystem::path adapter = fixture.temp_dir.path() / "adapter.py";
  WriteFile(adapter, MockAdapterSource());
  TestAccess::RegisterDebugAdapterForTest(fixture.shell, "mock", MockAdapterCommand(adapter, "stop"));
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  fixture.ExpectFresh(full, "debugging enabled");
  const auto pump = [&]() { TestAccess::ConsumeDapCallbacks(fixture.shell); };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-run --type mock /bin/true"),
         "debug-run should start the mock session");
  Expect(WaitUntil([&]() { return TestAccess::DebugSessionStopped(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the mock adapter should stop on configurationDone");
  for (int round = 0; round < 20; ++round) {
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(TestAccess::BottomPanelVisible(fixture.shell), "the session's console opened the panel");
  const workspace::RenderInvalidation started = fixture.shell.ConsumePendingRenderInvalidation();
  Expect(largest_named(started) < 0.5f, "the console opening the panel names no window-sized rect: " +
                                            std::to_string(largest_named(started)));
  fixture.ExpectFresh(started, "the session's console opened the panel");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-stop"), "debug-stop should run");
  Expect(WaitUntil([&]() { return !TestAccess::DebugSessionActive(fixture.shell); }, std::chrono::seconds(10),
                   std::chrono::milliseconds(5), pump),
         "the session ends");
  for (int round = 0; round < 20; ++round) {
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the session ended");
}

void TestTheBottomPanelsBodyRepaintsWhenOtherContentTakesIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("panel.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  {
    auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
    TerminalSessionTestAccess::Reset(session, 8, 80);
    TerminalSessionTestAccess::AppendOutput(
        session, "first$ one\r\nfirst$ two\r\nfirst$ three\r\nfirst$ four\r\nfirst$ five\r\nfirst$ ");
  }
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal with six lines");
  auto& state = TestAccess::CurrentProjectState(fixture.shell);

  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.two", 2);
  fixture.ExpectFresh(NothingAnnounced(), "an output channel took the body from the terminal");
  state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
  fixture.ExpectFresh(NothingAnnounced(), "the terminal took it back");
  TestAccess::AddTerminalTab(fixture.shell);
  {
    auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
    TerminalSessionTestAccess::Reset(session, 8, 80);
    TerminalSessionTestAccess::AppendOutput(session, "second$ ");
  }
  fixture.ExpectFresh(NothingAnnounced(), "a second terminal tab in front");
  state.active_terminal_tab_index = 0;
  fixture.ExpectFresh(NothingAnnounced(), "the first terminal tab in front again");
  TestAccess::HideBottomPanel(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the panel closed");
  state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
  fixture.ExpectFresh(NothingAnnounced(), "the panel opened on its terminal");
}

void TestATerminalsOutputRepaintsItsPaneNotThePanel() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("panel.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term-split"), "term-split should split the terminal");
  Expect(TestAccess::TerminalPaneCount(fixture.shell) == 2, "the terminal should have two panes");
  for (std::size_t pane = 0; pane < 2; ++pane) {
    auto& session = TestAccess::TerminalPaneSession(fixture.shell, pane);
    TerminalSessionTestAccess::Reset(session, 8, 40);
    TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  }
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal tab of two panes");
  const SDL_FRect panel = TestAccess::CurrentLayout(fixture.shell).bottom_panel;
  const auto panel_share = [&](const workspace::RenderInvalidation& asked) {
    fixture.PinCaret();
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, asked);
    float covered = damage.full ? panel.w * panel.h : 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      SDL_FRect inside{};
      if (SDL_GetRectIntersectionFloat(&rect, &panel, &inside)) {
        covered += inside.w * inside.h;
      }
    }
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    return covered / (panel.w * panel.h);
  };
  const auto update = [&]() {
    TestAccess::ConsumeTerminalSessionUpdates(fixture.shell);
    return fixture.shell.ConsumePendingRenderInvalidation();
  };

  TerminalSessionTestAccess::AppendOutput(TestAccess::TerminalPaneSession(fixture.shell, 0), "ls\r\none two three\r\n");
  fixture.ExpectFresh(update(), "output in the first pane");
  TerminalSessionTestAccess::AppendOutput(TestAccess::TerminalPaneSession(fixture.shell, 1), "make\r\n");
  const float one_pane = panel_share(update());
  Expect(one_pane > 0.0f && one_pane < 0.7f,
         "output in one pane repaints that pane, not the panel: " + std::to_string(one_pane) + " of it");

  const float nothing = panel_share(update());
  Expect(nothing == 0.0f, "an update that changed nothing repaints nothing: " + std::to_string(nothing));

  TestAccess::ShowOutputChannelWithLines(fixture.shell, "qa.front", 3);
  fixture.ExpectFresh(NothingAnnounced(), "an output channel in front");
  TerminalSessionTestAccess::AppendOutput(TestAccess::TerminalPaneSession(fixture.shell, 0), "behind\r\n");
  const float behind = panel_share(update());
  Expect(behind == 0.0f, "a terminal's output behind another content repaints nothing: " + std::to_string(behind));
  TestAccess::CurrentProjectState(fixture.shell).panel.content = WorkspaceShell::PanelContentKind::Terminal;
  fixture.ExpectFresh(NothingAnnounced(), "the terminal in front again, with what it printed meanwhile");
}

void TestTheDebugDockOpensAndClosesFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("main.c", "int main() { return 0; }\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"),
         "debug.enabled should turn on");
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "debugging enabled, the dock closed");
  const auto run = [&](const std::string& command, const std::string& what) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command), command + " should run");
    const workspace::RenderInvalidation asked = fixture.shell.ConsumePendingRenderInvalidation();
    Expect(asked.frame && !asked.full && asked.rects.empty(),
           what + " owes a frame and names no rect (rects=" + std::to_string(asked.rects.size()) + ")");
    fixture.ExpectFresh(asked, what);
  };
  run("debug-pane-toggle", "the dock opening");
  Expect(TestAccess::CurrentProjectState(fixture.shell).debug_pane.visible, "the dock is open");
  run("debug-pane-watch", "the dock showing its Watch list");
  run("debug-pane-callstack", "the dock showing its Call Stack");
  run("debug-pane-toggle", "the dock closing");
  Expect(!TestAccess::CurrentProjectState(fixture.shell).debug_pane.visible, "the dock is closed");
  run("debug-pane-breakpoints", "the dock opening on its Breakpoints");
  run("debug-pane-toggle", "the dock closing again");
}

void TestAHoveredTerminalLinkRepaintsItsRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("link.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  TerminalSessionTestAccess::AppendOutput(
      session, "go to https://example.com/x now\r\nplain row\r\nsee https://example.com/a-long-link-that-");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal with a link");
  const SDL_FPoint url = TestAccess::TerminalCellPoint(fixture.shell, 0, 12);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(url.x, url.y)}), "the pointer onto a link");
  Expect(TestAccess::TerminalHoveredLink(fixture.shell).has_value(), "the link is hovered");
  const SDL_FPoint other = TestAccess::TerminalCellPoint(fixture.shell, 2, 10);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(other.x, other.y)}),
                      "the pointer onto another row's link");
  const SDL_FPoint plain = TestAccess::TerminalCellPoint(fixture.shell, 1, 2);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(plain.x, plain.y)}), "the pointer off the links");
  Expect(!TestAccess::TerminalHoveredLink(fixture.shell).has_value(), "no link is hovered");
}

float DamagedShareOf(FlowFixture& fixture, const SDL_FRect& area, const workspace::RenderInvalidation& asked) {
  fixture.PinCaret();
  const workspace::RenderInvalidation damage =
      FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, asked);
  float covered = damage.full ? area.w * area.h : 0.0f;
  for (const SDL_FRect& rect : damage.rects) {
    SDL_FRect inside{};
    if (SDL_GetRectIntersectionFloat(&rect, &area, &inside)) {
      covered += inside.w * inside.h;
    }
  }
  fixture.PinCaret();
  fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  return covered / std::max(1.0f, area.w * area.h);
}

terminal::TerminalSession& TerminalShowing(FlowFixture& fixture, std::size_t rows, std::string_view output) {
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, rows, 80);
  TerminalSessionTestAccess::AppendOutput(session, output);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal showing its output");
  return session;
}

workspace::RenderInvalidation TerminalWake(FlowFixture& fixture) {
  TestAccess::ConsumeTerminalSessionUpdates(fixture.shell);
  return fixture.shell.ConsumePendingRenderInvalidation();
}

void TestATerminalsEchoRepaintsItsRowNotItsPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("echo.txt", "text\n");
  auto& session = TerminalShowing(fixture, 4, "first line\r\nsecond line\r\nprompt$ ");
  const SDL_FRect panel = TestAccess::CurrentLayout(fixture.shell).bottom_panel;
  TerminalSessionTestAccess::AppendOutput(session, "l");
  const float echo = DamagedShareOf(fixture, panel, TerminalWake(fixture));
  Expect(echo > 0.0f && echo < 0.2f, "an echo repaints its row, not the pane: " + std::to_string(echo) + " of the panel");
  TerminalSessionTestAccess::AppendOutput(session, "s");
  fixture.ExpectFresh(TerminalWake(fixture), "an echoed character");
  const workspace::RenderInvalidation elsewhere{.rects = {SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f}}};
  TerminalSessionTestAccess::AppendOutput(session, " -l");
  fixture.ExpectFresh(elsewhere, "an echo sampled by another frame");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[s\x1b[1;1HFIRST LINE\x1b[u");
  fixture.ExpectFresh(TerminalWake(fixture), "a row rewritten above the cursor");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[s\x1b[2;1H\x1b[31;4msecond line\x1b[0m\x1b[u");
  const float restyled = DamagedShareOf(fixture, panel, TerminalWake(fixture));
  Expect(restyled > 0.0f && restyled < 0.2f,
         "a row restyled over the same glyphs repaints, alone: " + std::to_string(restyled));
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[s\x1b[2;1H\x1b[32;4msecond line\x1b[0m\x1b[u");
  fixture.ExpectFresh(TerminalWake(fixture), "a row recoloured over the same glyphs");
  TerminalSessionTestAccess::AppendOutput(session, "\r\nfile-a  file-b\r\nprompt$ ");
  fixture.ExpectFresh(TerminalWake(fixture), "output that adds rows");
  TerminalSessionTestAccess::AppendOutput(session, "x\r\ny\r\nz\r\nprompt$ ");
  fixture.ExpectFresh(TerminalWake(fixture), "output that scrolls the screen");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2J\x1b[Hprompt$ ");
  fixture.ExpectFresh(TerminalWake(fixture), "the screen cleared");
  const float nothing = DamagedShareOf(fixture, panel, TerminalWake(fixture));
  Expect(nothing == 0.0f, "a wake with nothing new repaints nothing: " + std::to_string(nothing));
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5h");
  fixture.ExpectFresh(TerminalWake(fixture), "the screen in reverse video");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5l");
  fixture.ExpectFresh(TerminalWake(fixture), "the screen out of reverse video");
}

void TestATerminalSelectionRepaintsTheRowsItCovers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("select.txt", "text\n");
  TerminalShowing(fixture, 4, "To run a command as administrator\r\nuse sudo <command>\r\nsee man sudo_root\r\nprompt$ ");
  TestAccess::SetFocusPanel(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the panel focused");
  const SDL_FRect panel = TestAccess::CurrentLayout(fixture.shell).bottom_panel;
  using Point = workspace::TerminalSelectionPoint;
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 3}, Point{0, 16});
  const float one_row = DamagedShareOf(fixture, panel, NothingAnnounced());
  Expect(one_row > 0.0f && one_row < 0.2f, "a selection in one row repaints that row: " + std::to_string(one_row));
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 3}, Point{0, 20});
  fixture.ExpectFresh(NothingAnnounced(), "the selection grown along its row");
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 3}, Point{2, 7});
  fixture.ExpectFresh(NothingAnnounced(), "the selection grown over two more rows");
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 3}, Point{2, 9});
  const float last_row = DamagedShareOf(fixture, panel, NothingAnnounced());
  Expect(last_row > 0.0f && last_row < 0.2f,
         "the selection's end moving repaints its last row only: " + std::to_string(last_row));
  TestAccess::SetFocusEditor(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the keyboard left the panel: the selection kept, not live");
  TestAccess::SetFocusPanel(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the keyboard back in the panel");
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, std::nullopt, std::nullopt);
  fixture.ExpectFresh(NothingAnnounced(), "the selection cleared");
}

void TestTerminalFindHighlightsRepaintTheirRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("find.txt", "text\n");
  auto& session = TerminalShowing(fixture, 4, "alpha beta\r\ngamma alpha\r\ndelta\r\nalpha$ ");
  TestAccess::SetFocusPanel(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "terminal-find"), "terminal-find opens the find bar");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the find bar");
  Expect(TestAccess::TerminalFind(fixture.shell).visible(), "the find bar is open");
  for (const char* text : {"a", "l", "p"}) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(text)}),
                        std::string("the query typed: ") + text);
  }
  Expect(TestAccess::TerminalFind(fixture.shell).matches().size() == 3,
         "the query matches three times: " + std::to_string(TestAccess::TerminalFind(fixture.shell).matches().size()) +
             " for [" + std::string(TestAccess::TerminalFind(fixture.shell).query().text()) + "]");
  for (int step = 0; step < 3; ++step) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "the next match");
  }
  TerminalSessionTestAccess::AppendOutput(session, "alp");
  fixture.ExpectFresh(TerminalWake(fixture), "a match typed at the prompt");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_BACKSPACE)}), "the query shortened");
  for (const char* name : {"button:terminal-find/next", "button:terminal-find/match-case",
                           "button:terminal-find/whole-word", "button:terminal-find/previous"}) {
    const auto button = fixture.ElementRect(name);
    Expect(button.has_value(), std::string("the find bar shows ") + name);
    if (!button) {
      return;
    }
    fixture.ExpectFresh(
        DispatchEvents(fixture.shell, {MouseMotionEvent(button->x + button->w * 0.5f, button->y + button->h * 0.5f)}),
        std::string("the pointer onto ") + name);
    fixture.ExpectFresh(fixture.Click(*button), std::string("a click on ") + name);
  }
  const SDL_FPoint grid = TestAccess::TerminalCellPoint(fixture.shell, 2, 30);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(grid.x, grid.y)}), "the pointer off the bar");
  fixture.ExpectFresh(fixture.Click(SDL_FRect{grid.x - 1.0f, grid.y - 1.0f, 2.0f, 2.0f}),
                      "a click on the terminal: the bar keeps its matches and gives the keyboard back");
  Expect(TestAccess::TerminalFind(fixture.shell).visible() && !TestAccess::TerminalFind(fixture.shell).focused(),
         "the bar is open and not focused");
  const workspace::RenderInvalidation typed = DispatchEvents(fixture.shell, {TextInputEvent("x")});
  Expect(TerminalSessionTestAccess::SentBytes(session) == "x",
         "the key went to the terminal: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
  Expect(!typed.full && typed.rects.empty(),
         "a key typed into the terminal names no rect: " + std::to_string(typed.rects.size()));
  fixture.ExpectFresh(typed, "a key typed into the terminal");
  const auto query = fixture.ElementRect("field:terminal-find/query");
  Expect(query.has_value(), "the find bar shows its query field");
  if (!query) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*query), "a click in the query field takes the keyboard back");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "the find bar closed");
  Expect(!TestAccess::TerminalFind(fixture.shell).visible(), "the bar is closed");
}

void TestOneTerminalPanesOutputLeavesTheOtherPaneAlone() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("panes.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term-split"), "term-split should split the terminal");
  Expect(TestAccess::TerminalPaneCount(fixture.shell) == 2, "the terminal should have two panes");
  for (std::size_t pane = 0; pane < 2; ++pane) {
    auto& session = TestAccess::TerminalPaneSession(fixture.shell, pane);
    TerminalSessionTestAccess::Reset(session, 4, 40);
    TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  }
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal tab of two panes");
  const workspace::TerminalPaneRectsLayout rects = TestAccess::TerminalPaneRects(fixture.shell);
  auto& first = TestAccess::TerminalPaneSession(fixture.shell, 0);
  TerminalSessionTestAccess::AppendOutput(first, "ls\r\none two three\r\nprompt$ ");
  const float other = DamagedShareOf(fixture, rects.panes[1], TerminalWake(fixture));
  Expect(other == 0.0f, "rows added to the first pane repaint none of the second: " + std::to_string(other));
  TerminalSessionTestAccess::AppendOutput(first, "x");
  const float own = DamagedShareOf(fixture, rects.panes[0], TerminalWake(fixture));
  Expect(own > 0.0f && own < 0.4f, "an echo in the first pane repaints its row: " + std::to_string(own));
  TerminalSessionTestAccess::AppendOutput(TestAccess::TerminalPaneSession(fixture.shell, 1), "make\r\n");
  TerminalSessionTestAccess::AppendOutput(first, "y");
  fixture.ExpectFresh(TerminalWake(fixture), "output in both panes at once");

  const auto run = [&](const std::string& command, const std::string& what) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command), command + " should run");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), what);
  };
  TestAccess::SetFocusPanel(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the panel focused");
  using Point = workspace::TerminalSelectionPoint;
  TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 0}, Point{0, 6});
  fixture.ExpectFresh(NothingAnnounced(), "a selection in the active pane");
  const std::size_t was_active = TestAccess::ActiveTerminalPaneIndex(fixture.shell);
  run(was_active == 0 ? "term-pane-right" : "term-pane-left", "the other pane focused, the selection gone");
  Expect(TestAccess::ActiveTerminalPaneIndex(fixture.shell) != was_active, "the other pane is active");
  run("terminal-find prompt", "the find bar open on a query both panes hold");
  Expect(!TestAccess::TerminalFind(fixture.shell).matches().empty(), "the query matches in the active pane");
  run(was_active == 0 ? "term-pane-left" : "term-pane-right", "the first pane focused under an open find bar");
  run(was_active == 0 ? "term-pane-right" : "term-pane-left", "and the other again");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "the find bar closed");
}

void TestTerminalRowsRepaintAtFractionalScales() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const std::string scale : {"1.25", "1.6", "2.25"}) {
    FlowFixture fixture("scale.txt", "text\n", 1600, 1000);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale " + scale),
           "set-setting ui.scale should run");
    auto& session =
        TerminalShowing(fixture, 3, "go to https://example.com/x now\r\n\x1b[41mred\x1b[0m gjpqy \x1b[4munder\x1b[0m\r\nprompt$ ");
    TestAccess::SetFocusPanel(fixture.shell);
    fixture.ExpectFresh(NothingAnnounced(), "the panel focused at scale " + scale);
    for (const char* echo : {"g", "j", "\x1b[7m \x1b[0m", "\b\b\b   \b\b\b"}) {
      TerminalSessionTestAccess::AppendOutput(session, echo);
      fixture.ExpectFresh(TerminalWake(fixture), "an echo at scale " + scale);
    }
    using Point = workspace::TerminalSelectionPoint;
    TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{1, 0}, Point{1, 9});
    fixture.ExpectFresh(NothingAnnounced(), "a selection in the middle row at scale " + scale);
    TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, Point{0, 2}, Point{2, 3});
    fixture.ExpectFresh(NothingAnnounced(), "a selection over every row at scale " + scale);
    TestAccess::SetActiveTerminalSelectionForTest(fixture.shell, std::nullopt, std::nullopt);
    fixture.ExpectFresh(NothingAnnounced(), "the selection cleared at scale " + scale);
    const SDL_FPoint url = TestAccess::TerminalCellPoint(fixture.shell, 0, 12);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(url.x, url.y)}),
                        "the pointer onto a link at scale " + scale);
    const SDL_FPoint plain = TestAccess::TerminalCellPoint(fixture.shell, 1, 6);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(plain.x, plain.y)}),
                        "the pointer off the link at scale " + scale);
  }
}

void TestAMenuRowTooNarrowForItsShortcutDrawsInsideItsRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("left.py", "one\ntwo\nthree\n", 427, 240);
  const std::filesystem::path right = fixture.temp_dir.path() / "right.py";
  WriteFile(right, "one\n2\nthree\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "compare-files " + fixture.file.string() + " " + right.string()),
         "compare-files should open a diff");
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a diff in a window at 3x");
  const auto right_click = [&](float x, float y, const std::string& what) {
    SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    down.button.button = SDL_BUTTON_RIGHT;
    up.button.button = SDL_BUTTON_RIGHT;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y), down}), what);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {up}), what + ", the button released");
  };
  right_click(365.0f, 117.0f, "a right press opened the diff's menu at 3x");
  Expect(TestAccess::AnyMenuPopupOpen(fixture.shell), "the diff's menu is open");
  const SDL_FRect window = TestAccess::CurrentLayout(fixture.shell).full;
  for (float y = window.y + 30.0f; y < window.y + window.h; y += 20.0f) {
    for (float x = window.x + 30.0f; x < window.x + window.w; x += 110.0f) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}), "the pointer over the menu at 3x");
    }
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the diff's menu");
}

void TestThePanelHeaderPaintsTheTitlesItPrepared() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("titles.txt", "text\n");
  TestAccess::EnsureTerminalTab(fixture.shell);
  auto& session = TestAccess::ActiveTerminalSession(fixture.shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a terminal titled bash");
  SDL_FlushRenderer(fixture.canvas.renderer());
  SDL_Surface* painted = SDL_DuplicateSurface(fixture.canvas.surface());
  Expect(painted != nullptr, "the painted frame should copy");

  fixture.PinCaret();
  const auto token = fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TerminalSessionTestAccess::SetLaunchLabel(session, "a title a good deal longer than bash was");
  (void)TestAccess::ControlUi(fixture.shell);
  fixture.shell.RenderClip(token, fixture.canvas.renderer(), fixture.width, fixture.height);
  SDL_FlushRenderer(fixture.canvas.renderer());
  const render::PixelDiffResult diff = DiffSurfaces(painted, fixture.canvas.surface());
  SDL_DestroySurface(painted);
  Expect(diff.identical(), "a frame paints the titles it prepared; " + DescribePixelDiff(diff));
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the next frame, with the new title");
}

void TestTheTabStripsPaintTheTabsTheyPrepared() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("first.txt", "one\n");
  const std::filesystem::path other = fixture.temp_dir.path() / "app";
  WriteFile(other / "b.txt", "b\n");
  Expect(TestAccess::OpenProjectTab(fixture.shell, other, false, false), "a second project opens");
  TestAccess::OpenSingleEditorTab(fixture.shell, other / "b.txt");
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "two projects, a file in its tab");
  Expect(TestAccess::CurrentLayout(fixture.shell).project_tab_strip.h > 0.0f, "the project strip shows");
  SDL_FlushRenderer(fixture.canvas.renderer());
  SDL_Surface* painted = SDL_DuplicateSurface(fixture.canvas.surface());
  Expect(painted != nullptr, "the painted frame should copy");

  fixture.PinCaret();
  const auto token = fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  auto& project = TestAccess::CurrentProjectState(fixture.shell);
  Expect(!project.editor_groups.empty() && !project.editor_groups.front().open_tabs.empty(), "the project has a tab");
  project.editor_groups.front().open_tabs.front().title = "a-title-set-under-the-prepared-frame.txt";
  project.id = project::ProjectId::ForRemote("worky", "/srv/app");
  project.remote_status.stages = {{.text = "not connected", .state = editor::WelcomeStageState::Failed}};
  ++project.remote_status_revision;
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  (void)TestAccess::ControlUi(fixture.shell);
  fixture.shell.RenderClip(token, fixture.canvas.renderer(), fixture.width, fixture.height);
  SDL_FlushRenderer(fixture.canvas.renderer());
  const render::PixelDiffResult diff = DiffSurfaces(painted, fixture.canvas.surface());
  Expect(diff.identical(), "a frame paints the tabs it prepared; " + DescribePixelDiff(diff));
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the next frame, with the new title and state");
  SDL_FlushRenderer(fixture.canvas.renderer());
  const render::PixelDiffResult shown = DiffSurfaces(painted, fixture.canvas.surface());
  SDL_DestroySurface(painted);
  Expect(!shown.identical(), "and that frame does show them (the change under test is one the strips draw)");
}

void TestAnUnfocusedDiffsThumbsFollowAnEditInAnotherPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string wide = "a line a good deal wider than either pane of the diff is: ";
  for (int i = 0; i < 12; ++i) {
    wide += "more and more and more text ";
  }
  std::string body;
  for (int i = 0; i < 80; ++i) {
    body += "line " + std::to_string(i) + "\n";
  }
  for (const std::string scale : {"1", "1.75"}) {
    FlowFixture fixture("work.txt", "one\n" + wide + "\n" + body, 1463, 823);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale " + scale),
           "set-setting ui.scale should run");
    const std::filesystem::path base = fixture.root / "base.txt";
    WriteFile(base, "one\n" + wide + " as it was\n" + body);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "split-right should run");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-left"), "focus-group-left should run");
    Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                          "compare-files " + base.string() + " " + fixture.file.string()),
           "compare-files should open a diff");
    auto& project = TestAccess::CurrentProjectState(fixture.shell);
    Expect(project.editor_groups.size() == 2 && project.editor_groups[0].has_active_tab() &&
               project.editor_groups[0].active_tab().compare.has_value(),
           "the left pane shows the diff");
    if (project.editor_groups.size() != 2 || !project.editor_groups[0].active_tab().compare.has_value()) {
      return;
    }
    project.editor_groups[0].active_tab().compare->horizontal_scroll = 40;
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus-group-right"), "focus-group-right should run");
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    const workspace::RenderInvalidation full{.full = true};
    fixture.ExpectFresh(full, "a file and its diff side by side, at scale " + scale);
    Expect(!TestAccess::ActiveTabIsCompare(fixture.shell), "the editor has the keyboard, not the diff");
    TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(1, 30);
    fixture.ExpectFresh(NothingAnnounced(), "the caret in the long line");
    for (int key = 0; key < 14; ++key) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("a")}),
                          "a character that lengthens the longest line, at scale " + scale);
    }
    for (int key = 0; key < 4; ++key) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}),
                          "a line break in the file, at scale " + scale);
    }
    for (int key = 0; key < 8; ++key) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_BACKSPACE)}),
                          "and what shortens it again, at scale " + scale);
    }
  }
}

void TestTheSearchPanelRepaintsWhatChangedFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  std::string many;
  for (int i = 0; i < 6; ++i) {
    many += "alpha line " + std::to_string(i) + " with beta\n";
  }
  FlowFixture fixture("notes.txt", many);
  WriteFile(fixture.root / "src" / "one.txt", "alpha in one\nno match here\nalpha again\n");
  WriteFile(fixture.root / "src" / "two.txt", "beta only\nalpha in two\n");
  TestAccess::ShowSearchSidebar(fixture.shell, "alpha", false);
  WaitForProjectSearch(fixture.shell);
  Expect(TestAccess::ProjectSearchResults(fixture.shell).size() >= 6,
         "the search finds its alphas: " + std::to_string(TestAccess::ProjectSearchResults(fixture.shell).size()));
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the Search panel with its results");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SidebarSearch) > 12,
         "the panel lays out its fields, buttons and rows");
  const SDL_FRect sidebar = TestAccess::CurrentLayout(fixture.shell).sidebar;
  const auto frame_only = [&](const workspace::RenderInvalidation& asked, const std::string& what) {
    Expect(!asked.full && asked.rects.empty(),
           what + " names no rect (full=" + std::to_string(asked.full) + " rects=" +
               std::to_string(asked.rects.size()) + ")");
    fixture.ExpectFresh(asked, what);
  };

  {
    editor::TextViewport& editor = TestAccess::ActiveEditor(fixture.shell);
    editor.MoveCursorTo(0, std::string("alpha line 0 with beta").size());
    editor.InsertText(" and more");
    const SDL_FRect editor_surface = TestAccess::CurrentLayout(fixture.shell).editor_surface;
    TestAccess::CatchUpSearchResults(fixture.shell);
    TestAccess::SettleLiveSearchHits(fixture.shell);
    fixture.ExpectFresh(workspace::RenderInvalidation{.full = false, .rects = {editor_surface}},
                        "a hit's preview following an edit of its line");
  }

  float lit = 0.0f;
  for (std::size_t result = 0; result < 4; ++result) {
    const SDL_FRect row = TestAccess::ProjectSearchResultRect(fixture.shell, result);
    const workspace::RenderInvalidation asked =
        DispatchEvents(fixture.shell, {MouseMotionEvent(row.x + 20.0f, row.y + row.h * 0.5f)});
    lit = std::max(lit, DamagedShareOf(fixture, sidebar, asked));
  }
  Expect(lit > 0.0f && lit < 0.15f, "a lit result row repaints its rows, not the sidebar: " + std::to_string(lit));
  for (std::size_t result = 0; result < 4; ++result) {
    const SDL_FRect row = TestAccess::ProjectSearchResultRect(fixture.shell, result);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(row.x + 20.0f, row.y + row.h * 0.5f)}),
                        "the pointer onto a result row");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(900.0f, 300.0f)}), "the pointer off the list");

  TestAccess::SetFocusSidebar(fixture.shell);
  fixture.ExpectFresh(NothingAnnounced(), "the keyboard in the sidebar");
  const float moved = DamagedShareOf(fixture, sidebar, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}));
  Expect(moved > 0.0f && moved < 0.15f,
         "the selection moving a row repaints its rows, not the sidebar: " + std::to_string(moved));
  for (int key = 0; key < 3; ++key) {
    frame_only(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "Down the results");
  }
  frame_only(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_UP)}), "Up the results");

  const auto settled = [&](workspace::RenderInvalidation asked, const std::string& what) {
    Expect(!asked.full, what + " asks for no full frame");
    std::uint64_t revision = TestAccess::ProjectSearchResultsRevision(fixture.shell);
    Uint64 quiet_since = SDL_GetTicks();
    Expect(WaitUntil(
               [&]() {
                 const std::uint64_t now_revision = TestAccess::ProjectSearchResultsRevision(fixture.shell);
                 if (TestAccess::ProjectSearchRunning(fixture.shell) || now_revision != revision) {
                   revision = now_revision;
                   quiet_since = SDL_GetTicks();
                   return false;
                 }
                 return SDL_GetTicks() - quiet_since >= 60;
               },
               std::chrono::seconds(5), std::chrono::milliseconds(5),
               [&]() { TestAccess::ConsumeProjectSearchUpdates(fixture.shell); }),
           "the project search should settle");
    asked.Merge(fixture.shell.ConsumePendingRenderInvalidation());
    fixture.ExpectFresh(asked, what);
  };
  const auto typed = [&](const SDL_Event& event, const std::string& what) {
    const workspace::RenderInvalidation asked = DispatchEvents(fixture.shell, {event});
    Expect(!asked.full && asked.rects.empty(),
           what + " names no rect (full=" + std::to_string(asked.full) + " rects=" +
               std::to_string(asked.rects.size()) + ")");
    settled(asked, what);
  };

  for (const char* name : {"button:search/case", "button:search/mode", "button:search/hidden", "button:search/scope",
                           "button:search/scope", "button:search/case", "button:search/mode", "button:search/hidden"}) {
    const auto button = fixture.ElementRect(name);
    Expect(button.has_value(), std::string("the panel shows ") + name);
    if (!button) {
      return;
    }
    fixture.ExpectFresh(
        DispatchEvents(fixture.shell, {MouseMotionEvent(button->x + button->w * 0.5f, button->y + button->h * 0.5f)}),
        std::string("the pointer onto ") + name);
    settled(fixture.Click(*button), std::string("a click on ") + name);
  }

  const auto query = fixture.ElementRect("field:search/query");
  Expect(query.has_value(), "the panel shows its query field");
  if (!query) {
    return;
  }
  settled(fixture.Click(*query), "a click in the query field");
  typed(KeyDownEvent(SDLK_END), "End in the query");
  for (const char* text : {" ", "i", "n"}) {
    typed(TextInputEvent(text), std::string("the query typed: ") + text);
  }
  SDL_Event shift_left = KeyDownEvent(SDLK_LEFT);
  shift_left.key.mod = SDL_KMOD_LSHIFT;
  for (int key = 0; key < 3; ++key) {
    typed(shift_left, "Shift+Left selecting in the query");
  }
  typed(KeyDownEvent(SDLK_BACKSPACE), "the selection cut from the query");
  for (const char* text : {"z", "q", "x"}) {
    typed(TextInputEvent(text), std::string("the query typed: ") + text);
  }
  settled(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "Return: the query nothing matches");
  Expect(TestAccess::ProjectSearchResults(fixture.shell).empty(), "nothing matches the query");
  settled(fixture.Click(*query), "a click in the query field again");
  typed(KeyDownEvent(SDLK_END), "End in the query");
  for (int key = 0; key < 3; ++key) {
    typed(KeyDownEvent(SDLK_BACKSPACE), "the query cut back");
  }
  settled(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN)}), "Return: the query that matches");
  Expect(!TestAccess::ProjectSearchResults(fixture.shell).empty(), "the query matches again");

  TestAccess::ShowTreeSidebar(fixture.shell);
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the tree in the sidebar");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SidebarSearch) == 0,
         "the panel out of sight lays out nothing");
  TestAccess::ShowSearchSidebar(fixture.shell, "beta", false);
  settled(fixture.shell.ConsumePendingRenderInvalidation(), "Search again, another query");
}

void TestTheSearchPanelPaintsTheResultsItPrepared() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "alpha one\nalpha two\nbeta three\n");
  TestAccess::ShowSearchSidebar(fixture.shell, "alpha", false);
  WaitForProjectSearch(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the Search panel with two hits");
  SDL_FlushRenderer(fixture.canvas.renderer());
  SDL_Surface* painted = SDL_DuplicateSurface(fixture.canvas.surface());
  Expect(painted != nullptr, "the painted frame should copy");

  fixture.PinCaret();
  const auto token = fixture.shell.PrepareFrameOnce(fixture.canvas.renderer(), fixture.width, fixture.height);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::ShowSearchSidebar(fixture.shell, "beta", false);
  WaitForProjectSearch(fixture.shell);
  (void)TestAccess::ControlUi(fixture.shell);
  fixture.shell.RenderClip(token, fixture.canvas.renderer(), fixture.width, fixture.height);
  SDL_FlushRenderer(fixture.canvas.renderer());
  const render::PixelDiffResult diff = DiffSurfaces(painted, fixture.canvas.surface());
  Expect(diff.identical(), "a frame paints the results it prepared; " + DescribePixelDiff(diff));
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the next frame, with the new results");
  SDL_FlushRenderer(fixture.canvas.renderer());
  const render::PixelDiffResult shown = DiffSurfaces(painted, fixture.canvas.surface());
  SDL_DestroySurface(painted);
  Expect(!shown.identical(), "and that frame does show them");
}

void TestATerminalCaretRepaintsWhereItWasAndIs() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  for (const std::string scale : {"1", "1.25"}) {
    FlowFixture fixture("caret.txt", "text\n", 1600, 1000);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting ui.scale " + scale),
           "set-setting ui.scale should run");
    auto& session = TerminalShowing(fixture, 4, "first row of text\r\n\xE6\x97\xA5\xE6\x9C\xAC wide glyphs\r\nprompt$ ");
    TestAccess::SetFocusPanel(fixture.shell);
    fixture.ExpectFresh(NothingAnnounced(), "the terminal with the keyboard, at scale " + scale);
    const SDL_FRect panel = TestAccess::CurrentLayout(fixture.shell).bottom_panel;
    const auto output = [&](const char* bytes, const std::string& what) {
      TerminalSessionTestAccess::AppendOutput(session, bytes);
      fixture.ExpectFresh(TerminalWake(fixture), what + ", at scale " + scale);
    };
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;7H");
    const float moved = DamagedShareOf(fixture, panel, TerminalWake(fixture));
    Expect(moved > 0.0f && moved < 0.02f,
           "a cursor move repaints the two cells, at scale " + scale + ": " + std::to_string(moved));
    output("\x1b[3;4H", "the cursor moved again");
    output("\x1b[2;1H", "the cursor onto a wide glyph");
    output("\x1b[2;9H", "and off it");
    output("\x1b[5 q", "a blinking bar");
    output("\x1b[4 q", "a steady underline");
    output("\x1b[3 q", "a blinking underline");
    output("\x1b[2 q", "a steady block");
    output("\x1b[1 q", "a blinking block");
    output("\x1b[?25l", "the cursor hidden");
    output("\x1b[1;3H", "moved while hidden");
    output("\x1b[?25h", "shown again elsewhere");
    TestAccess::SetFocusEditor(fixture.shell);
    fixture.ExpectFresh(NothingAnnounced(), "the keyboard in the editor: an outline, at scale " + scale);
    output("\x1b[2;12H", "the outline moved by the program");
    TestAccess::SetFocusPanel(fixture.shell);
    fixture.ExpectFresh(NothingAnnounced(), "the keyboard back: the block, at scale " + scale);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "terminal-find"), "terminal-find opens the find bar");
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(),
                        "the find bar has the keyboard: an outline, at scale " + scale);
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}),
                        "the find bar closed: the block, at scale " + scale);
  }
}

void TestTheHomePageRepaintsWhenWhatItListsChanges() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  const std::filesystem::path other = fixture.temp_dir.path() / "app";
  WriteFile(other / "b.txt", "b\n");
  Expect(TestAccess::OpenProjectTab(fixture.shell, other, false, false), "a second project opens");
  auto& remote = TestAccess::CurrentProjectState(fixture.shell);
  remote.id = project::ProjectId::ForRemote("worky", "/srv/app");
  using Stage = editor::WelcomeStageState;
  remote.remote_status.stages = {{.text = "not connected", .state = Stage::Failed}};
  ++remote.remote_status_revision;
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a remote project with no tab: its home page, offline");
  SDL_Surface* offline = FullFrame(fixture);

  remote.remote_status.stages = {{.text = "connecting to worky", .state = Stage::Active},
                                 {.text = "connected", .state = Stage::Done}};
  ++remote.remote_status_revision;
  workspace::RenderInvalidation frame;
  frame.frame = true;
  fixture.ExpectFresh(frame, "the home page's stages following the connection");
  SDL_Surface* online = FullFrame(fixture);
  const SDL_FRect editor = TestAccess::CurrentLayout(fixture.shell).editor_surface;
  const render::PixelDiffResult changed = DiffSurfaces(offline, online);
  SDL_DestroySurface(offline);
  SDL_DestroySurface(online);
  Expect(changed.count > 0 && static_cast<float>(changed.min_y + changed.box_height) > editor.y + 20.0f,
         "the stages show on the home page (the case under test)");
}

void TestATerminalRowCutByAShortPanelStaysInsideItsElement() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("short.txt", "text\n");
  auto& session = TerminalShowing(fixture, 12, "");
  std::string screen;
  for (int row = 0; row < 12; ++row) {
    screen += "\x1b[4" + std::to_string(1 + row % 6) + "mrow " + std::to_string(row) + " of text\x1b[0m gjpqy";
    screen += row + 1 < 12 ? "\r\n" : "";
  }
  TerminalSessionTestAccess::AppendOutput(session, screen);
  const workspace::RenderInvalidation full{.full = true};
  for (float height = 30.0f; height <= 140.0f; height += 1.0f) {
    TestAccess::SetBottomPanelHeight(fixture.shell, height);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    fixture.ExpectFresh(full, "the panel " + std::to_string(static_cast<int>(height)) + " tall");
    TerminalSessionTestAccess::AppendOutput(session, "x");
    fixture.ExpectFresh(TerminalWake(fixture), "an echo in a panel " + std::to_string(static_cast<int>(height)) + " tall");
  }
}

std::vector<std::pair<std::string, SDL_FRect>> SettingsCardElements(FlowFixture& fixture, std::string_view kind) {
  std::vector<std::pair<std::string, SDL_FRect>> found;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const std::string prefix = std::string(kind) + ":";
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    const std::string& name = entry["name"].AsString();
    if (name.starts_with(prefix)) {
      const auto& r = entry["rect"].AsArray();
      found.emplace_back(name, SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())});
    }
  }
  return found;
}

void TestAPointerLeavingASettingsRowForAToastUnlightsIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "settings"), "settings should open the card");
  TestAccess::ShowNotification(fixture.shell, workspace::NotificationService::Tone::Warning,
                               "No completions available");
  fixture.ExpectFresh(full, "Settings open, a toast at the bottom right");
  const SDL_FRect toast = TestAccess::NotificationToastRect(fixture.shell, 0);
  const auto unlit_by_toast = [&](std::string_view kind) {
    const auto rows = SettingsCardElements(fixture, kind);
    Expect(rows.size() >= 2, "the card lists its " + std::string(kind) + " rows: " + std::to_string(rows.size()));
    for (std::size_t index = 1; index < rows.size(); ++index) {
      const SDL_FRect& rect = rows[index].second;
      const SDL_FPoint point{rect.x + 10.0f, rect.y + rect.h * 0.5f};
      if (workspace::Contains(toast, point.x, point.y)) {
        continue;
      }
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(point.x, point.y)}),
                          "the pointer lit " + rows[index].first);
      fixture.ExpectFresh(
          DispatchEvents(fixture.shell, {MouseMotionEvent(toast.x + toast.w - 4.0f, toast.y + toast.h * 0.5f)}),
          "the pointer left " + rows[index].first + " for the toast");
      return;
    }
    Expect(false, "a " + std::string(kind) + " row the toast leaves uncovered (the case under test)");
  };
  unlit_by_toast("settings-category");
  unlit_by_toast("setting");
}

void TestTheSettingsCardRepaintsWhatChangedFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab");
  const auto frame_only = [&](const workspace::RenderInvalidation& asked, const std::string& what) {
    Expect(asked.frame && !asked.full && asked.rects.empty(),
           what + " owes a frame and names no rect (full=" + std::to_string(asked.full) +
               " rects=" + std::to_string(asked.rects.size()) + ")");
    fixture.ExpectFresh(asked, what);
  };
  const auto key = [&](SDL_Keycode code, const std::string& what) {
    frame_only(DispatchEvents(fixture.shell, {KeyDownEvent(code)}), what);
  };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "settings"), "settings should open the card");
  frame_only(fixture.shell.ConsumePendingRenderInvalidation(), "Settings opening");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SettingsCard) > 6,
         "the card lays out its chrome and its rows");

  for (const char* text : {"t", "a", "b"}) {
    frame_only(DispatchEvents(fixture.shell, {TextInputEvent(text)}), std::string("the filter typed: ") + text);
  }
  key(SDLK_BACKSPACE, "the filter shortened");
  key(SDLK_BACKSPACE, "the filter shortened again");
  key(SDLK_BACKSPACE, "the filter cleared");
  key(SDLK_TAB, "Tab to the rail");
  key(SDLK_DOWN, "Down the rail");
  key(SDLK_DOWN, "Down the rail again");
  key(SDLK_UP, "Up the rail");
  key(SDLK_UP, "Up the rail to the first category");
  key(SDLK_TAB, "Tab to the values");
  key(SDLK_DOWN, "Down the values");
  key(SDLK_DOWN, "Down the values again");
  key(SDLK_UP, "Up the values");
  {
    const SDL_FRect anchor = TestAccess::CurrentLayout(fixture.shell).overlay_anchor;
    const float moved = DamagedShareOf(fixture, anchor, DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}));
    Expect(moved > 0.0f && moved < 0.5f,
           "the selection moving a row repaints its rows, not the card: " + std::to_string(moved));
  }

  if (const auto rail_rows = SettingsCardElements(fixture, "settings-category"); !rail_rows.empty()) {
    fixture.ExpectFresh(fixture.Click(rail_rows.front().second), "a click on the first category");
  }
  const SDL_FRect card = TestAccess::CurrentLayout(fixture.shell).overlay_anchor;
  const auto hover_share = [&](std::string_view kind) {
    const auto rows = SettingsCardElements(fixture, kind);
    Expect(rows.size() >= 2, "the card lists its " + std::string(kind) + " rows: " + std::to_string(rows.size()));
    float worst = 0.0f;
    for (std::size_t index = 0; index < rows.size() && index < 5; ++index) {
      const SDL_FRect& rect = rows[index].second;
      const workspace::RenderInvalidation asked =
          DispatchEvents(fixture.shell, {MouseMotionEvent(rect.x + 10.0f, rect.y + rect.h * 0.5f)});
      Expect(!asked.full && asked.rects.empty(), "a hover in the card names no rect: " + rows[index].first);
      worst = std::max(worst, DamagedShareOf(fixture, card, asked));
    }
    for (std::size_t index = 0; index < rows.size() && index < 5; ++index) {
      const SDL_FRect& rect = rows[index].second;
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(rect.x + 10.0f, rect.y + rect.h * 0.5f)}),
                          "the pointer onto " + rows[index].first);
    }
    return worst;
  };
  const float rail = hover_share("settings-category");
  Expect(rail < 0.2f, "a lit rail row repaints its rows, not the card: " + std::to_string(rail));
  const float values = hover_share("setting");
  Expect(values < 0.4f, "a lit value row repaints its rows, not the card: " + std::to_string(values));
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(4.0f, 4.0f)}), "the pointer off the card");

  const auto categories = SettingsCardElements(fixture, "settings-category");
  if (categories.size() >= 3) {
    fixture.ExpectFresh(fixture.Click(categories[2].second), "a click on a category");
    fixture.ExpectFresh(fixture.Click(categories[1].second), "a click on another");
  }
  const auto settings = SettingsCardElements(fixture, "setting");
  if (!settings.empty()) {
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.mouse_x = settings.front().second.x + 20.0f;
    wheel.wheel.mouse_y = settings.front().second.y + 10.0f;
    wheel.wheel.y = -1.0f;
    wheel.wheel.integer_y = -1;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {wheel}), "the wheel down the values");
    wheel.wheel.y = 1.0f;
    wheel.wheel.integer_y = 1;
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {wheel}), "the wheel back up");
  }
  key(SDLK_ESCAPE, "Escape closing Settings");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::SettingsCard) == 0,
         "the closed card lays out nothing");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "about"), "about should open Help");
  frame_only(fixture.shell.ConsumePendingRenderInvalidation(), "Help opening");
  for (const char* text : {"s", "a", "v"}) {
    frame_only(DispatchEvents(fixture.shell, {TextInputEvent(text)}), std::string("Help filtered: ") + text);
  }
  key(SDLK_BACKSPACE, "Help's filter shortened");
  key(SDLK_DOWN, "Down Help's list");
  key(SDLK_PAGEDOWN, "a page down Help's list");
  key(SDLK_ESCAPE, "Escape closing Help");
}

void TestARemoteProjectsStageRepaintsItsTabFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("a.txt", "a\n");
  const std::filesystem::path other = fixture.temp_dir.path() / "app";
  WriteFile(other / "b.txt", "b\n");
  Expect(TestAccess::OpenProjectTab(fixture.shell, other, false, false), "a second project opens");
  auto& remote = TestAccess::CurrentProjectState(fixture.shell);
  remote.id = project::ProjectId::ForRemote("worky", "/srv/app");
  using Stage = editor::WelcomeStageState;
  remote.remote_status.stages = {{.text = "not connected", .state = Stage::Failed}};
  ++remote.remote_status_revision;
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "two projects, the remote one offline");
  const SDL_FRect strip = TestAccess::CurrentLayout(fixture.shell).project_tab_strip;
  Expect(strip.h > 0.0f, "two projects show the project strip");
  SDL_Surface* offline = FullFrame(fixture);

  remote.remote_status.stages = {{.text = "connected", .state = Stage::Done}};
  ++remote.remote_status_revision;
  workspace::RenderInvalidation redraw;
  redraw.frame = true;
  redraw.rects.push_back(TestAccess::CurrentLayout(fixture.shell).editor_surface);
  fixture.ExpectFresh(redraw, "the remote project connected");
  SDL_Surface* online = FullFrame(fixture);
  const render::PixelDiffResult changed = DiffSurfaces(offline, online);
  SDL_DestroySurface(offline);
  SDL_DestroySurface(online);
  Expect(changed.count > 0 && static_cast<float>(changed.min_y) < strip.y + strip.h,
         "the stage shows on the project strip (changed from y=" + std::to_string(changed.min_y) + ")");
}

void TestAMenuHangingFromAStatusSegmentOpensAndClosesFromAFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n");
  workspace::StatusBarSegmentValue remote;
  remote.text = "worky";
  remote.tooltip = "Remote menu";
  remote.menu = workspace::MenuId::Remote;
  remote.visible = true;
  TestAccess::SetStatusBarSegment(fixture.shell, workspace::StatusBarSegmentId::Remote, remote);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "a status bar with the remote segment");
  const auto segment = fixture.ElementRect("status:remote");
  Expect(segment.has_value(), "the status bar shows the remote segment");
  if (!segment) {
    return;
  }
  const auto owes_a_frame = [](const workspace::RenderInvalidation& redraw, const std::string& what) {
    Expect(!redraw.full && redraw.frame && redraw.rects.empty(), what + " owes a frame and names no rect (rects=" +
                                                                    std::to_string(redraw.rects.size()) + ")");
    return redraw;
  };
  const float x = segment->x + segment->w * 0.5f;
  const float y = segment->y + segment->h * 0.5f;
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)}), "the pointer onto the segment");
  const workspace::RenderInvalidation opened =
      DispatchEvents(fixture.shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  Expect(TestAccess::MenuBarOpen(fixture.shell), "a click on the segment opens its menu");
  fixture.ExpectFresh(owes_a_frame(opened, "the segment's menu opening"), "the Remote menu opened over the bar");
  const std::vector<MenuRowProbe> rows = OpenMenuRows(fixture, "Remote");
  Expect(!rows.empty(), "the Remote menu lists its rows");
  if (!rows.empty()) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(rows.front().rect.x + 12.0f,
                                                                        rows.front().rect.y + rows.front().rect.h * 0.5f)}),
                        "the pointer onto its first row");
  }
  const workspace::RenderInvalidation closed = DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
  fixture.ExpectFresh(owes_a_frame(closed, "the segment's menu closing"), "Escape closed it");
  fixture.ExpectFresh(fixture.Click(*segment), "the segment opened it again");
  fixture.ExpectFresh(fixture.Click(SDL_FRect{700.0f, 300.0f, 2.0f, 2.0f}), "a press outside closed it");
  Expect(!TestAccess::MenuBarOpen(fixture.shell), "the press outside closed the menu");
}

void TestTheMenuOverflowPopupsRowsRepaintRowByRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 640, 480);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in a 640-px window");
  const std::optional<SDL_FRect> chevron = TestAccess::MenuOverflowChevronRect(fixture.shell);
  Expect(chevron.has_value(), "a 640-px window folds menus into the chevron");
  if (!chevron.has_value()) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*chevron), "the overflow popup opened");
  Expect(TestAccess::MenuOverflowPopupOpen(fixture.shell), "the chevron opens the overflow popup");
  const std::vector<MenuRowProbe> rows = OpenMenuRows(fixture, "more");
  Expect(rows.size() >= 2 &&
             TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuOverflowPopup) == rows.size() + 1,
         "the overflow popup lays out its card and a unit per row");
  if (rows.size() < 2) {
    return;
  }
  for (const MenuRowProbe& row : rows) {
    fixture.ExpectFresh(
        DispatchEvents(fixture.shell, {MouseMotionEvent(row.rect.x + 12.0f, row.rect.y + row.rect.h * 0.5f)}),
        "the pointer onto " + row.name);
  }
  const workspace::RenderInvalidation within = DispatchEvents(
      fixture.shell, {MouseMotionEvent(rows.back().rect.x + 30.0f, rows.back().rect.y + rows.back().rect.h * 0.5f)});
  Expect(!within.full && !within.frame && within.rects.empty(),
         "the pointer moving within its overflow row asks for no frame");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_UP)}), "Up in the overflow popup");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_HOME)}), "Home in the overflow popup");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(500.0f, 400.0f)}),
                      "the pointer off the overflow popup");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the overflow popup");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuOverflowPopup) == 0,
         "closed: no units");
}

void TestASubmenuAndAContextMenuRepaintRowByRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.txt", "one\ntwo\n", 840, 600);
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab");
  const auto edit = fixture.ElementRect("menu:Edit");
  Expect(edit.has_value(), "the menu bar shows Edit");
  if (!edit) {
    return;
  }
  fixture.ExpectFresh(fixture.Click(*edit), "the Edit menu opened");
  const std::vector<MenuRowProbe> rows = OpenMenuRows(fixture, "Edit");
  Expect(rows.size() >= 4, "the Edit menu lists its rows");
  if (rows.size() < 4) {
    return;
  }
  TestAccess::OpenSubmenuForTest(fixture.shell, workspace::MenuId::Help, rows[2].rect);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "a submenu opened beside its row");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuSubmenu) > 1,
         "the open submenu lays out its card and rows");
  const std::vector<MenuRowProbe> sub_rows = OpenMenuRows(fixture, "Help");
  Expect(sub_rows.size() >= 2, "the submenu lists its rows");
  for (const MenuRowProbe& row : sub_rows) {
    fixture.ExpectFresh(
        DispatchEvents(fixture.shell, {MouseMotionEvent(row.rect.x + 12.0f, row.rect.y + row.rect.h * 0.5f)}),
        "the pointer onto the submenu's " + row.name);
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "Down in the submenu");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {MouseMotionEvent(rows[1].rect.x + 12.0f, rows[1].rect.y + rows[1].rect.h * 0.5f)}),
      "the pointer back onto the popup");
  for (int i = 0; i < 2 && TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) > 0; ++i) {
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape in the open menu");
  }
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuPopup) == 0 &&
             TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::MenuSubmenu) == 0,
         "the menu and its submenu are closed: no units");

  TestAccess::OpenTreeContextMenuForPath(fixture.shell, workspace::TreeContextTargetKind::File, fixture.file);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the context menu opened");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::ContextMenu) > 1,
         "the context menu lays out its card and rows");
  const std::optional<SDL_FRect> context = TestAccess::TreeContextMenuRect(fixture.shell);
  Expect(context.has_value(), "the context menu has a rect");
  if (context.has_value()) {
    for (const float y : {14.0f, 36.0f, 58.0f}) {
      fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(context->x + 12.0f, context->y + y)}),
                          "the pointer along the context menu");
    }
  }
  if (context.has_value()) {
    const auto owes_nothing = [&](float x, float y, const std::string& what) {
      const workspace::RenderInvalidation redraw = DispatchEvents(fixture.shell, {MouseMotionEvent(x, y)});
      Expect(!redraw.full && !redraw.frame && redraw.rects.empty(),
             what + " asks for no frame (full=" + std::to_string(redraw.full) + " frame=" +
                 std::to_string(redraw.frame) + " rects=" + std::to_string(redraw.rects.size()) + ")");
    };
    owes_nothing(context->x + 40.0f, context->y + 58.0f, "the pointer moving within its context menu row");
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(700.0f, 500.0f)}),
                        "the pointer off the context menu");
    owes_nothing(710.0f, 505.0f, "the pointer moving beside an open context menu");
  }
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN)}), "Down in the context menu");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the context menu");
  Expect(TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::ContextMenu) == 0,
         "closed: no units");

  auto& git = TestAccess::CurrentProjectState(fixture.shell).sidebar.git;
  git.entries.assign(1, workspace::GitSidebarEntry{});
  git.selected_index = 0;
  TestAccess::OpenTreeContextMenuForPath(fixture.shell, workspace::TreeContextTargetKind::GitEntry, fixture.file);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(NothingAnnounced(), "the git entry menu opened");
  const std::size_t units = TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::ContextMenu);
  const std::optional<SDL_FRect> card = TestAccess::TreeContextMenuRect(fixture.shell);
  git.entries[0].staged = true;
  fixture.ExpectFresh(NothingAnnounced(), "the git entry menu's Stage row turned to Unstage");
  const std::optional<SDL_FRect> card_now = TestAccess::TreeContextMenuRect(fixture.shell);
  Expect(units > 1 && TestAccess::DerivedUnitCount(fixture.shell, workspace::DerivedSurface::ContextMenu) == units &&
             card.has_value() && card_now.has_value() && card->w == card_now->w && card->h == card_now->h,
         "the menu kept its card and rows: a label changed");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)}), "Escape closed the git entry menu");
}

void TestATrackedLspRequestRepaintsItsStatusSegmentNotTheWindow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("notes.cpp", "int a = 1;\nint b = 2;\n");
  if (TestAccess::BottomPanelVisible(fixture.shell)) {
    TestAccess::HideBottomPanel(fixture.shell);
  }
  const workspace::RenderInvalidation full{.full = true};
  fixture.ExpectFresh(full, "the file in its tab, the bottom panel hidden");
  const float window = static_cast<float>(fixture.width) * static_cast<float>(fixture.height);
  const auto step = [&](const std::string& what) {
    const workspace::RenderInvalidation asked = fixture.shell.ConsumePendingRenderInvalidation();
    Expect(!asked.full, what + " asks for no full repaint");
    fixture.PinCaret();
    const workspace::RenderInvalidation damage =
        FrameDamage(fixture.shell, fixture.canvas, fixture.width, fixture.height, asked);
    float covered = damage.full ? window : 0.0f;
    for (const SDL_FRect& rect : damage.rects) {
      covered += rect.w * rect.h;
    }
    Expect(covered < window * 0.05f,
           what + " repaints its status segment, not the window: " + std::to_string(covered / window) + " of it");
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  };
  SDL_Surface* idle = FullFrame(fixture);
  TestAccess::BeginTrackedLspRequest(fixture.shell);
  step("a language-server request going out");
  SDL_Surface* working = FullFrame(fixture);
  Expect(DiffSurfaces(idle, working).count > 0, "a request in flight shows in the status bar");
  SDL_DestroySurface(idle);
  SDL_DestroySurface(working);
  TestAccess::FinishTrackedLspRequest(fixture.shell);
  step("its reply landing");
  TestAccess::BeginTrackedLspRequest(fixture.shell);
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "a second request going out");
  TestAccess::FinishTrackedLspRequest(fixture.shell);
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "its reply landing");
}

}

namespace {

std::string StatefulSource(int lines, int changed_every) {
  std::string text;
  for (int i = 0; i < lines; ++i) {
    const int phase = i % 40;
    if (phase == 0) {
      text += "/* region " + std::to_string(i) + " opens here\n";
    } else if (phase == 20) {
      text += "region closes here */\n";
    } else if (changed_every > 0 && i % changed_every == 7) {
      text += "long changed_" + std::to_string(i) + " = 2;  // on this side\n";
    } else {
      text += "int value_" + std::to_string(i) + " = 1;  // note\n";
    }
  }
  return text;
}

void SettleCompare(FlowFixture& fixture) {
  for (int frame = 0; frame < 8; ++frame) {
    fixture.PinCaret();
    fixture.shell.Render(fixture.canvas.renderer(), fixture.width, fixture.height);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
  }
}

std::size_t ExpectCompareTokensOfAFreshWalk(FlowFixture& fixture, const std::string& what,
                                            bool caret_row_on_screen = true, std::size_t at_least = 20) {
  workspace::CompareTabState& tab = TestAccess::ActiveCompare(fixture.shell);
  const std::vector<compare::CompareRow>& rows = tab.model.rows;
  const auto walk = [&](const std::vector<std::uint32_t>& line_rows, bool left, editor::SyntaxState state) {
    std::vector<std::vector<editor::SyntaxTokenKind>> tokens(line_rows.size());
    for (std::size_t line = 0; line < line_rows.size(); ++line) {
      const compare::CompareRow& row = rows[line_rows[line]];
      state = editor::SyntaxHighlighter::HighlightLineInto(left ? row.left_text : row.right_text, tab.path, state,
                                                           &tokens[line]);
    }
    return tokens;
  };
  const auto left =
      walk(tab.left_line_rows, true, editor::SyntaxHighlighter::InitialState(tab.path, *tab.left_content));
  const auto right = walk(tab.right_line_rows, false,
                          editor::SyntaxHighlighter::InitialState(tab.path, tab.right_viewport.lines()));
  std::size_t held = 0;
  std::size_t wrong = 0;
  std::string first_wrong;
  const auto note = [&](bool ok, const std::string& where) {
    if (!ok && wrong++ == 0) {
      first_wrong = where;
    }
  };
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const compare::CompareRow& row = rows[index];
    const auto& left_held = workspace::CompareRowLeftTokens(tab, row);
    const auto& right_held = workspace::CompareRowRightOwnTokens(tab, row);
    if (!left_held.empty()) {
      ++held;
      note(left_held == left[static_cast<std::size_t>(row.left_line - 1)], "left line " + std::to_string(row.left_line));
      if (workspace::CompareRowRightTokensAliasLeft(tab, index)) {
        note(left_held == right[static_cast<std::size_t>(row.right_line - 1)],
             "right line " + std::to_string(row.right_line) + " (drawn from the left pane's)");
      }
    }
    if (!right_held.empty()) {
      ++held;
      note(right_held == right[static_cast<std::size_t>(row.right_line - 1)],
           "right line " + std::to_string(row.right_line));
    }
  }
  Expect(wrong == 0, what + ": " + std::to_string(wrong) +
                         " token buffers are not what a walk from the top gives, first at " + first_wrong);
  const std::size_t caret_line = tab.right_viewport.cursor_line();
  if (caret_row_on_screen && caret_line < tab.right_line_rows.size()) {
    const std::size_t caret_row = tab.right_line_rows[caret_line];
    const auto& drawn = workspace::CompareRowRightTokensAliasLeft(tab, caret_row)
                            ? workspace::CompareRowLeftTokens(tab, rows[caret_row])
                            : workspace::CompareRowRightOwnTokens(tab, rows[caret_row]);
    Expect(!drawn.empty() || rows[caret_row].right_text.empty(), what + ": the caret's row draws its tokens");
  }
  Expect(held >= at_least, what + ": the windows hold the rows on screen (" + std::to_string(held) + ")");
  return held;
}

struct CompareTokensFixture {
  FlowFixture fixture;
  std::filesystem::path right_path;

  explicit CompareTokensFixture(const std::string& left = StatefulSource(400, 0),
                                const std::string& right = StatefulSource(400, 23))
      : fixture("left.cpp", left) {
    right_path = fixture.root / "right.cpp";
    WriteFile(right_path, right);
    Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                          "compare-files " + fixture.file.string() + " " + right_path.string()),
           "compare-files should open a diff");
    compare().right_view_active = true;
    TestAccess::SetFocusEditor(fixture.shell);
    TestAccess::NoteLayoutInputsChanged(fixture.shell);
    Settle();
  }

  workspace::CompareTabState& compare() { return TestAccess::ActiveCompare(fixture.shell); }
  void Settle() { SettleCompare(fixture); }
  std::size_t ExpectTokensOfAFreshWalk(const std::string& what, bool caret_row_on_screen = true,
                                       std::size_t at_least = 20) {
    return ExpectCompareTokensOfAFreshWalk(fixture, what, caret_row_on_screen, at_least);
  }
};

void TestACompareEditRestartsItsSyntaxWalkAtTheEditedLine() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  CompareTokensFixture f;
  workspace::CompareTabState& tab = f.compare();
  editor::TextViewport& right = tab.right_viewport;
  f.ExpectTokensOfAFreshWalk("the diff as opened");

  constexpr std::size_t kLine = 237;
  right.MoveCursorTo(kLine, 4);
  (void)DispatchEvents(f.fixture.shell, {KeyDownEvent(SDLK_RIGHT)});
  f.Settle();
  f.ExpectTokensOfAFreshWalk("the caret at line 237");
  const std::size_t left_walked = tab.left_token_window.frontier();
  const std::size_t right_walked = tab.right_token_window.frontier();
  Expect(left_walked > kLine && right_walked > kLine, "both panes are walked past the caret's line (" +
                                                          std::to_string(left_walked) + ", " +
                                                          std::to_string(right_walked) + ")");

  (void)DispatchEvents(f.fixture.shell, {TextInputEvent("x")});
  Expect(tab.left_token_window.frontier() == left_walked,
         "a keystroke in the working side leaves the left pane's walk where it was: " +
             std::to_string(tab.left_token_window.frontier()) + " of " + std::to_string(left_walked));
  Expect(tab.right_token_window.frontier() == kLine,
         "and restarts the right pane's at the edited line: " + std::to_string(tab.right_token_window.frontier()));
  f.Settle();
  f.ExpectTokensOfAFreshWalk("a character typed at line 237");

  const auto edited = [&](const std::string& what) {
    TestAccess::RefreshActiveCompareDerivedState(f.fixture.shell);
    Expect(tab.left_token_window.frontier() == left_walked, what + ": the left pane's walk stands");
    f.Settle();
    f.ExpectTokensOfAFreshWalk(what);
  };
  right.MoveCursorTo(245, 0);
  right.InsertText("/* opened here ");
  edited("a block comment opened at line 245");
  right.MoveCursorTo(247, 0);
  right.InsertText("\n");
  edited("a line added at line 247");
  right.InsertText("int pasted_a = 1;\nint pasted_b = 2; /* and\nclosed */ int pasted_c = 3;\n");
  edited("three lines pasted");
  right.MoveCursorTo(240, 0);
  right.MoveCursorTo(241, 0, true);
  right.Backspace();
  edited("a line deleted at line 240");
  Expect(right.Undo() && right.Undo(), "the edits undo");
  edited("two edits undone");
  Expect(right.Redo(), "one redoes");
  edited("one redone");
  right.MoveCursorTo(0, 0);
  right.InsertText("// a first line\n");
  TestAccess::RefreshActiveCompareDerivedState(f.fixture.shell);
  Expect(tab.right_token_window.frontier() == 0, "an edit of the first line restarts the right pane's walk at the top");
  f.Settle();
  f.ExpectTokensOfAFreshWalk("a line added at the top", false);
  right.MoveCursorTo(right.line_count() - 2, 0);
  (void)DispatchEvents(f.fixture.shell, {KeyDownEvent(SDLK_RIGHT)});
  f.Settle();
  Expect(tab.right_token_window.frontier() > 380, "the walk reached the bottom of the file: " +
                                                      std::to_string(tab.right_token_window.frontier()));
  f.ExpectTokensOfAFreshWalk("scrolled to the bottom after the edits", false);
}

void TestACollapsedRunsRowsAreNotTokenized() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  constexpr int kLines = 1500;
  constexpr int kFirstChange = 6;
  constexpr int kSecondChange = 1486;
  std::string left = StatefulSource(kLines, 0);
  std::string right;
  {
    std::size_t at = 0;
    for (int line = 0; line < kLines; ++line) {
      const std::size_t end = left.find('\n', at) + 1;
      right += line == kFirstChange || line == kSecondChange ? "long changed_here = 2;  // on this side\n"
                                                             : left.substr(at, end - at);
      at = end;
    }
  }
  CompareTokensFixture f(left, right);
  workspace::CompareTabState& tab = f.compare();
  f.Settle();
  std::optional<std::size_t> run_row;
  for (std::size_t index = 0; index < tab.presentation.rows.size(); ++index) {
    if (tab.presentation.rows[index].kind == compare::ComparePresentationRowKind::CollapsedContext &&
        tab.presentation.rows[index].collapsed_run_length > 1000) {
      run_row = index;
    }
  }
  Expect(run_row.has_value(), "the unchanged middle of the file collapses to one row");
  if (!run_row.has_value()) {
    return;
  }
  const auto shown_rows = [&]() {
    std::vector<bool> shown(tab.model.rows.size(), false);
    for (const compare::ComparePresentationRow& row : tab.presentation.rows) {
      if (row.kind == compare::ComparePresentationRowKind::Model && row.model_row_index < shown.size()) {
        shown[row.model_row_index] = true;
      }
    }
    return shown;
  };
  const auto held = [&](const std::vector<bool>& shown, bool of_shown) {
    std::size_t count = 0;
    for (std::size_t row = 0; row < tab.model.rows.size(); ++row) {
      if (shown[row] != of_shown) {
        continue;
      }
      count += workspace::CompareRowLeftTokens(tab, tab.model.rows[row]).empty() ? 0 : 1;
      count += workspace::CompareRowRightOwnTokens(tab, tab.model.rows[row]).empty() ? 0 : 1;
    }
    return count;
  };
  const std::vector<bool> shown = shown_rows();
  const std::size_t hidden = static_cast<std::size_t>(std::count(shown.begin(), shown.end(), false));
  Expect(hidden > 1000, "the run hides its middle: " + std::to_string(hidden) + " rows");
  Expect(tab.left_token_window.frontier() >= tab.left_line_rows.size() - 1,
         "the walk went through the run to the change below it: " + std::to_string(tab.left_token_window.frontier()));
  Expect(held(shown, false) == 0, "no row the run hides is tokenized: " +
                                                   std::to_string(held(shown, false)) + " buffers held");
  const std::size_t drawn = held(shown, true);
  Expect(drawn >= 8, "the rows drawn above and below the run are: " + std::to_string(drawn));
  f.ExpectTokensOfAFreshWalk("a diff with a collapsed run on screen", false, 8);

  const SDL_FRect show_all = TestAccess::ActiveCompareCollapsedContextActionRects(f.fixture.shell, *run_row).all_rect;
  (void)f.fixture.Click(show_all);
  f.Settle();
  const std::vector<bool> expanded = shown_rows();
  const auto still_hidden = static_cast<std::size_t>(std::count(expanded.begin(), expanded.end(), false));
  Expect(still_hidden < 50, "Show all shows the run's rows: " + std::to_string(still_hidden) + " of " +
                                std::to_string(hidden) + " still hidden");
  Expect(held(expanded, false) == 0, "and still no hidden row is tokenized");
  std::size_t newly_drawn = 0;
  for (std::size_t row = 0; row < tab.model.rows.size(); ++row) {
    newly_drawn += !shown[row] && expanded[row] && !workspace::CompareRowLeftTokens(tab, tab.model.rows[row]).empty() ? 1 : 0;
  }
  Expect(newly_drawn >= 8, "expanded, the run's rows on screen are tokenized: " + std::to_string(newly_drawn));
  f.ExpectTokensOfAFreshWalk("the run expanded", false, 8);
}

void TestACompareWhoseBufferIsReplacedRestartsItsSyntaxWalk() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  {
    CompareTokensFixture f;
    workspace::CompareTabState& tab = f.compare();
    editor::TextViewport& right = tab.right_viewport;
    right.MoveCursorTo(237, 4);
    (void)DispatchEvents(f.fixture.shell, {KeyDownEvent(SDLK_RIGHT)});
    f.Settle();
    f.ExpectTokensOfAFreshWalk("the caret at line 237");

    const std::string before = StatefulSource(400, 23);
    std::size_t third_line_end = 0;
    for (int line = 0; line < 3; ++line) {
      third_line_end = before.find('\n', third_line_end) + 1;
    }
    const std::string replaced =
        before.substr(0, third_line_end) + "*/ int closed_early = 1;\n" + StatefulSource(380, 11);
    right.MoveCursorTo(0, 0);
    right.MoveCursorTo(right.line_count() - 1, right.lines()[right.line_count() - 1].size(), true);
    right.InsertText(replaced);
    TestAccess::RefreshActiveCompareDerivedState(f.fixture.shell);
    Expect(tab.right_token_window.frontier() == 3,
           "a text replaced whole restarts the right pane's walk at its first line that differs: " +
               std::to_string(tab.right_token_window.frontier()));
    f.Settle();
    f.ExpectTokensOfAFreshWalk("the working side's text replaced whole", false);

    tab.left_content = compare::MakeCompareText("/* the left pane is one comment\n" + StatefulSource(300, 0));
    TestAccess::RefreshActiveCompareDerivedState(f.fixture.shell);
    Expect(tab.left_token_window.frontier() == 0, "another left buffer restarts the left pane's walk at the top");
    f.Settle();
    f.ExpectTokensOfAFreshWalk("the left pane's buffer replaced", false);

    const std::string loaded = "/* loaded: a comment from the top\n" + StatefulSource(350, 5);
    compare::BuildCompareModelInto(tab.model, tab.left_content, compare::MakeCompareText(loaded), tab.build_options);
    workspace::MarkCompareModelBuilt(tab, tab.build_options, tab.derived_left_line_count);
    f.Settle();
    f.ExpectTokensOfAFreshWalk("a model applied whole", false);
  }

  {
    FlowFixture fixture("notes.txt", "notes\n");
    const std::filesystem::path tracked = fixture.root / "tracked.cpp";
    WriteFile(tracked, StatefulSource(240, 0));
    InitializeGitRepo(fixture.root);
    CommitAll(fixture.root, "base", "base");
    WriteFile(tracked, StatefulSource(240, 23));
    Expect(TestAccess::OpenWorkingTreeComparison(fixture.shell, tracked, "HEAD", "HEAD"), "the diff opens");
    const auto settle = [&]() {
      for (int round = 0; round < 3; ++round) {
        SettleCompare(fixture);
        TestAccess::FlushPendingFileReads(fixture.shell);
      }
      SettleCompare(fixture);
    };
    settle();
    ExpectCompareTokensOfAFreshWalk(fixture, "a working-tree diff as opened", false);
    WriteFile(tracked, "/* changed on disk: a comment from the top\n" + StatefulSource(260, 7));
    TestAccess::MarkCompareTabsStaleForPath(fixture.shell, tracked);
    settle();
    workspace::CompareTabState& tab = TestAccess::ActiveCompare(fixture.shell);
    Expect(tab.right_line_rows.size() > 255, "the diff took the file's new text (" +
                                                 std::to_string(tab.right_line_rows.size()) + " lines)");
    ExpectCompareTokensOfAFreshWalk(fixture, "the file changed on disk", false);
  }
}

}

void RegisterQaFlowRegressionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegression/ACompareEditRestartsItsSyntaxWalkAtTheEditedLine",
          TestACompareEditRestartsItsSyntaxWalkAtTheEditedLine);
  AddTest(tests, "QaFlowRegression/ACollapsedRunsRowsAreNotTokenized", TestACollapsedRunsRowsAreNotTokenized);
  AddTest(tests, "QaFlowRegression/ACompareWhoseBufferIsReplacedRestartsItsSyntaxWalk",
          TestACompareWhoseBufferIsReplacedRestartsItsSyntaxWalk);
  AddTest(tests, "QaFlowRegression/AnOpenMenusRowsRepaintWhatChanged", TestAnOpenMenusRowsRepaintWhatChanged);
  AddTest(tests, "QaFlowRegression/AnOpenMenusRowsRepaintAtFractionalScales",
          TestAnOpenMenusRowsRepaintAtFractionalScales);
  AddTest(tests, "QaFlowRegression/ATrackedLspRequestRepaintsItsStatusSegmentNotTheWindow",
          TestATrackedLspRequestRepaintsItsStatusSegmentNotTheWindow);
  AddTest(tests, "QaFlowRegression/AnUnchangedCompareRowReadsTheLeftTokensUntilTheRightWalkHasIt",
          TestAnUnchangedCompareRowReadsTheLeftTokensUntilTheRightWalkHasIt);
  AddTest(tests, "QaFlowRegression/AMenuOpeningSwitchingAndClosingRepaintsItsBarItems",
          TestAMenuOpeningSwitchingAndClosingRepaintsItsBarItems);
  AddTest(tests, "QaFlowRegression/TheDebugToolbarFollowsItsSessionFromAFrame",
          TestTheDebugToolbarFollowsItsSessionFromAFrame);
  AddTest(tests, "QaFlowRegression/ADebugSessionsCallbacksRepaintNoWindowWhileTheDockIsClosed",
          TestADebugSessionsCallbacksRepaintNoWindowWhileTheDockIsClosed);
  AddTest(tests, "QaFlowRegression/AClosedBottomPanelAsksForNoWindow", TestAClosedBottomPanelAsksForNoWindow);
  AddTest(tests, "QaFlowRegression/TheBottomPanelsBodyRepaintsWhenOtherContentTakesIt",
          TestTheBottomPanelsBodyRepaintsWhenOtherContentTakesIt);
  AddTest(tests, "QaFlowRegression/ATerminalsOutputRepaintsItsPaneNotThePanel",
          TestATerminalsOutputRepaintsItsPaneNotThePanel);
  AddTest(tests, "QaFlowRegression/TheDebugDockOpensAndClosesFromAFrame", TestTheDebugDockOpensAndClosesFromAFrame);
  AddTest(tests, "QaFlowRegression/AMenuRowTooNarrowForItsShortcutDrawsInsideItsRow",
          TestAMenuRowTooNarrowForItsShortcutDrawsInsideItsRow);
  AddTest(tests, "QaFlowRegression/ThePanelHeaderPaintsTheTitlesItPrepared",
          TestThePanelHeaderPaintsTheTitlesItPrepared);
  AddTest(tests, "QaFlowRegression/APointerLeavingASettingsRowForAToastUnlightsIt",
          TestAPointerLeavingASettingsRowForAToastUnlightsIt);
  AddTest(tests, "QaFlowRegression/TheSettingsCardRepaintsWhatChangedFromAFrame",
          TestTheSettingsCardRepaintsWhatChangedFromAFrame);
  AddTest(tests, "QaFlowRegression/TheTabStripsPaintTheTabsTheyPrepared", TestTheTabStripsPaintTheTabsTheyPrepared);
  AddTest(tests, "QaFlowRegression/AnUnfocusedDiffsThumbsFollowAnEditInAnotherPane",
          TestAnUnfocusedDiffsThumbsFollowAnEditInAnotherPane);
  AddTest(tests, "QaFlowRegression/TheSearchPanelRepaintsWhatChangedFromAFrame",
          TestTheSearchPanelRepaintsWhatChangedFromAFrame);
  AddTest(tests, "QaFlowRegression/TheSearchPanelPaintsTheResultsItPrepared",
          TestTheSearchPanelPaintsTheResultsItPrepared);
  AddTest(tests, "QaFlowRegression/ATerminalCaretRepaintsWhereItWasAndIs", TestATerminalCaretRepaintsWhereItWasAndIs);
  AddTest(tests, "QaFlowRegression/TheHomePageRepaintsWhenWhatItListsChanges",
          TestTheHomePageRepaintsWhenWhatItListsChanges);
  AddTest(tests, "QaFlowRegression/ATerminalRowCutByAShortPanelStaysInsideItsElement",
          TestATerminalRowCutByAShortPanelStaysInsideItsElement);
  AddTest(tests, "QaFlowRegression/AHoveredTerminalLinkRepaintsItsRow", TestAHoveredTerminalLinkRepaintsItsRow);
  AddTest(tests, "QaFlowRegression/ATerminalsEchoRepaintsItsRowNotItsPane",
          TestATerminalsEchoRepaintsItsRowNotItsPane);
  AddTest(tests, "QaFlowRegression/ATerminalSelectionRepaintsTheRowsItCovers",
          TestATerminalSelectionRepaintsTheRowsItCovers);
  AddTest(tests, "QaFlowRegression/TerminalFindHighlightsRepaintTheirRows", TestTerminalFindHighlightsRepaintTheirRows);
  AddTest(tests, "QaFlowRegression/OneTerminalPanesOutputLeavesTheOtherPaneAlone",
          TestOneTerminalPanesOutputLeavesTheOtherPaneAlone);
  AddTest(tests, "QaFlowRegression/TerminalRowsRepaintAtFractionalScales", TestTerminalRowsRepaintAtFractionalScales);
  AddTest(tests, "QaFlowRegression/ARemoteProjectsStageRepaintsItsTabFromAFrame",
          TestARemoteProjectsStageRepaintsItsTabFromAFrame);
  AddTest(tests, "QaFlowRegression/AMenuHangingFromAStatusSegmentOpensAndClosesFromAFrame",
          TestAMenuHangingFromAStatusSegmentOpensAndClosesFromAFrame);
  AddTest(tests, "QaFlowRegression/TheMenuOverflowPopupsRowsRepaintRowByRow",
          TestTheMenuOverflowPopupsRowsRepaintRowByRow);
  AddTest(tests, "QaFlowRegression/ASubmenuAndAContextMenuRepaintRowByRow",
          TestASubmenuAndAContextMenuRepaintRowByRow);
  AddTest(tests, "QaFlowRegression/TheSidebarsModeRowRepaintsWhatChangedWithNoDamageRequest",
          TestTheSidebarsModeRowRepaintsWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/ADerivedSidebarViewCommitsItsSurfacesUnits",
          TestADerivedSidebarViewCommitsItsSurfacesUnits);
  AddTest(tests, "QaFlowRegression/TheFileTreesRowsRepaintWhatChangedWithNoDamageRequest",
          TestTheFileTreesRowsRepaintWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/TheFileTreeFollowsItsScrollAndARefresh", TestTheFileTreeFollowsItsScrollAndARefresh);
  AddTest(tests, "QaFlowRegression/AGitWakeWithProblemsInFrontRepaintsNothingInTheSidebar",
          TestAGitWakeWithProblemsInFrontRepaintsNothingInTheSidebar);
  AddTest(tests, "QaFlowRegression/TheGitViewsSelectionAndScrollRepaintTheirRows",
          TestTheGitViewsSelectionAndScrollRepaintTheirRows);
  AddTest(tests, "QaFlowRegression/APromptCaretBlinkAtScaleThreeLeavesNoColumn",
          TestAPromptCaretBlinkAtScaleThreeLeavesNoColumn);
  AddTest(tests, "QaFlowRegression/ACompareTabsActionsRepaintTheirRows", TestACompareTabsActionsRepaintTheirRows);
  AddTest(tests, "QaFlowRegression/ANewChangeInADiffRepaintsItsRowsNotTheChangesBelow",
          TestANewChangeInADiffRepaintsItsRowsNotTheChangesBelow);
  AddTest(tests, "QaFlowRegression/ScrollingADiffLeavesThePaneBesideItAlone",
          TestScrollingADiffLeavesThePaneBesideItAlone);
  AddTest(tests, "QaFlowRegression/AMergeTabsActionsRepaintWhatTheyChange", TestAMergeTabsActionsRepaintWhatTheyChange);
  AddTest(tests, "QaFlowRegression/ACompareWorkingSideRepaintsItsDiagnosticsAndDecorations",
          TestACompareWorkingSideRepaintsItsDiagnosticsAndDecorations);
  AddTest(tests, "QaFlowRegression/AMergeResultRepaintsTheFarBracketOfItsPair",
          TestAMergeResultRepaintsTheFarBracketOfItsPair);
  AddTest(tests, "QaFlowRegression/TheStickyBandFollowsTheCaretOffItsLine", TestTheStickyBandFollowsTheCaretOffItsLine);
  AddTest(tests, "QaFlowRegression/SplitPanesKeepTheirOwnStickyLines", TestSplitPanesKeepTheirOwnStickyLines);
  AddTest(tests, "QaFlowRegression/TypingBesideAPluginPresentationRepaintsTheEditedLine",
          TestTypingBesideAPluginPresentationRepaintsTheEditedLine);
  AddTest(tests, "QaFlowRegression/AnInsetGapThatOpensOrClosesRepaintsTheRowsItMoves",
          TestAnInsetGapThatOpensOrClosesRepaintsTheRowsItMoves);
  AddTest(tests, "QaFlowRegression/AnIndentWidthChangeRepaintsTheIndentGuides",
          TestAnIndentWidthChangeRepaintsTheIndentGuides);
  AddTest(tests, "QaFlowRegression/AWrappedLinesRowsKeyOnEveryByteOfIt", TestAWrappedLinesRowsKeyOnEveryByteOfIt);
  AddTest(tests, "QaFlowRegression/ACaretBlinkRepaintsTheCaretsRowsOnly", TestACaretBlinkRepaintsTheCaretsRowsOnly);
  AddTest(tests, "QaFlowRegression/ACaretBlinkDamagesNothingInTheOtherPane", TestACaretBlinkDamagesNothingInTheOtherPane);
  AddTest(tests, "QaFlowRegression/ScrollingTheProjectStripRepaintsIt", TestScrollingTheProjectStripRepaintsIt);
  AddTest(tests, "QaFlowRegression/AContextMenuClosesOverTheRowsItPainted", TestAContextMenuClosesOverTheRowsItPainted);
  AddTest(tests, "QaFlowRegression/APointerOnAToastHoversNoMenuRowBeneathIt",
          TestAPointerOnAToastHoversNoMenuRowBeneathIt);
  AddTest(tests, "QaFlowRegression/ATabGhostOverANarrowerStripStaysInsideIt",
          TestATabGhostOverANarrowerStripStaysInsideIt);
  AddTest(tests, "QaFlowRegression/APointerMovingOntoAToastUnlightsTheMenuRowItLeft",
          TestAPointerMovingOntoAToastUnlightsTheMenuRowItLeft);
  AddTest(tests, "QaFlowRegression/APointerMovingOntoAToastUnlightsTheTabItLeft",
          TestAPointerMovingOntoAToastUnlightsTheTabItLeft);
  AddTest(tests, "QaFlowRegression/ABreadcrumbLabelChangeRepaintsTheBand", TestABreadcrumbLabelChangeRepaintsTheBand);
  AddTest(tests, "QaFlowRegression/TabInANarrowDirtyPromptRepaintsEveryButton",
          TestTabInANarrowDirtyPromptRepaintsEveryButton);
  AddTest(tests, "QaFlowRegression/ClosingTheLastTabWhenZoomedInRepaintsTheStrip",
          TestClosingTheLastTabWhenZoomedInRepaintsTheStrip);
  AddTest(tests, "QaFlowRegression/TypingInAnEditorWithNoRowsRepaintsTheStatusBar",
          TestTypingInAnEditorWithNoRowsRepaintsTheStatusBar);
  AddTest(tests, "QaFlowRegression/TypingIntoAMirroredPlaceholderRepaintsEveryMirrorsLine",
          TestTypingIntoAMirroredPlaceholderRepaintsEveryMirrorsLine);
  AddTest(tests, "QaFlowRegression/ABreakpointFollowsAnEditOnceNotOnEveryRedraw",
          TestABreakpointFollowsAnEditOnceNotOnEveryRedraw);
  AddTest(tests, "QaFlowRegression/OutputTabRepaintsWhenItsChannelGetsALabel",
          TestOutputTabRepaintsWhenItsChannelGetsALabel);
  AddTest(tests, "QaFlowRegression/TypingRepaintsOnlyTheLinesWhoseOccurrencesChanged",
          TestTypingRepaintsOnlyTheLinesWhoseOccurrencesChanged);
  AddTest(tests, "QaFlowRegression/TypingPastTheViewsEdgeRepaintsTheBarNotThePane",
          TestTypingPastTheViewsEdgeRepaintsTheBarNotThePane);
  AddTest(tests, "QaFlowRegression/TypingOffAWordRepaintsItsOccurrences",
          TestTypingOffAWordRepaintsItsOccurrences);
  AddTest(tests, "QaFlowRegression/HoverUnderAStickyBandResolvesThePointersLine",
          TestHoverUnderAStickyBandResolvesThePointersLine);
  AddTest(tests, "QaFlowRegression/MergeToolbarHoverRepaintsTheButton",
          TestMergeToolbarHoverRepaintsTheButton);
  AddTest(tests, "QaFlowRegression/ATabDragAbandonedWithEscapeThenReleasedPaintsFresh",
          TestATabDragAbandonedWithEscapeThenReleasedPaintsFresh);
  AddTest(tests, "QaFlowRegression/ATabDraggedAcrossThreePanesPaintsFresh",
          TestATabDraggedAcrossThreePanesPaintsFresh);
  AddTest(tests, "QaFlowRegression/MergeToolbarStaysInsideANarrowPane",
          TestMergeToolbarStaysInsideANarrowPane);
  AddTest(tests, "QaFlowRegression/MergeToolbarStaysInsideAShortPane",
          TestMergeToolbarStaysInsideAShortPane);
  AddTest(tests, "QaFlowRegression/LeavingAPanelTabMidSlideLeavesNoHover", TestLeavingAPanelTabMidSlideLeavesNoHover);
  AddTest(tests, "QaFlowRegression/APanelShownMaximizedTakesTheKeyboardAtOnce",
          TestAPanelShownMaximizedTakesTheKeyboardAtOnce);
  AddTest(tests, "QaFlowRegression/ATabDraggedAcrossStackedPanesPaintsItsGhostInTheTargetStrip",
          TestATabDraggedAcrossStackedPanesPaintsItsGhostInTheTargetStrip);
  AddTest(tests, "QaFlowRegression/ADragAcrossPanelTabsLeavesNoHoveredCloseButton",
          TestADragAcrossPanelTabsLeavesNoHoveredCloseButton);
  AddTest(tests, "QaFlowRegression/SearchResultsCaughtUpAtFramePrepRepaintTheirCount",
          TestSearchResultsCaughtUpAtFramePrepRepaintTheirCount);
  AddTest(tests, "QaFlowRegression/AKeyInAMenuUnderARestingPointerRepaintsTheRows",
          TestAKeyInAMenuUnderARestingPointerRepaintsTheRows);
  AddTest(tests, "QaFlowRegression/LeavingTheCommitSubjectFieldRepaintsItsCaret",
          TestLeavingTheCommitSubjectFieldRepaintsItsCaret);
  AddTest(tests, "QaFlowRegression/ACommitBodySelectionStaysInsideItsField",
          TestACommitBodySelectionStaysInsideItsField);
  AddTest(tests, "QaFlowRegression/MovingOntoAPanelTabsCloseButtonRepaintsIt",
          TestMovingOntoAPanelTabsCloseButtonRepaintsIt);
  AddTest(tests, "QaFlowRegression/FindWidgetReplaceRepaintsTheDirtyTab",
          TestFindWidgetReplaceRepaintsTheDirtyTab);
  AddTest(tests, "QaFlowRegression/BannerReloadRepaintsTheCleanTab", TestBannerReloadRepaintsTheCleanTab);
  AddTest(tests, "QaFlowRegression/ThePointerLeavingATabOverflowChipRepaintsIt",
          TestThePointerLeavingATabOverflowChipRepaintsIt);
  AddTest(tests, "QaFlowRegression/ThePointerOnABannerButtonRepaintsItsHover",
          TestThePointerOnABannerButtonRepaintsItsHover);
  AddTest(tests, "QaFlowRegression/TheBannerSitsInTheFocusedGroupAndLeavesWithIt",
          TestTheBannerSitsInTheFocusedGroupAndLeavesWithIt);
  AddTest(tests, "QaFlowRegression/ACodeActionsTitleStaysInsideItsMenu",
          TestACodeActionsTitleStaysInsideItsMenu);
  AddTest(tests, "QaFlowRegression/TabDragDropPreviewPaints", TestTabDragDropPreviewPaints);
  AddTest(tests, "QaFlowRegression/ASearchRowThatDoesNotFitTheSidebarIsLeftOut",
          TestASearchRowThatDoesNotFitTheSidebarIsLeftOut);
  AddTest(tests, "QaFlowRegression/APaneNarrowedToItsMinimumPaintsNothingOutsideIt",
          TestAPaneNarrowedToItsMinimumPaintsNothingOutsideIt);
  AddTest(tests, "QaFlowRegression/ClosingAPromptOverTheFinderGivesTheFinderTheKeyboard",
          TestClosingAPromptOverTheFinderGivesTheFinderTheKeyboard);
  AddTest(tests, "QaFlowRegression/ToastsShowWithTheStatusBarHidden", TestToastsShowWithTheStatusBarHidden);
  AddTest(tests, "QaFlowRegression/OpeningAModalUnderThePointerDropsTheHoverItTakes",
          TestOpeningAModalUnderThePointerDropsTheHoverItTakes);
  AddTest(tests, "QaFlowRegression/DraggingTheSidebarShutKeepsEveryFrameFresh",
          TestDraggingTheSidebarShutKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/DividersDraggedInANarrowEditorColumnKeepEveryFrameFresh",
          TestDividersDraggedInANarrowEditorColumnKeepEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/EveryDividerDraggedToItsExtremesKeepsEveryFrameFresh",
          TestEveryDividerDraggedToItsExtremesKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/TogglingTheMergeBasePaneKeepsTheToolbarAndEveryFrameFresh",
          TestTogglingTheMergeBasePaneKeepsTheToolbarAndEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/AConflictOutOfSightHasNoButtons", TestAConflictOutOfSightHasNoButtons);
  AddTest(tests, "QaFlowRegression/ADraggedTabPaintsNothingUnderTheStrip",
          TestADraggedTabPaintsNothingUnderTheStrip);
  AddTest(tests, "QaFlowRegression/ADeeplyIndentedTreeRowKeepsItsLabelInsideTheRow",
          TestADeeplyIndentedTreeRowKeepsItsLabelInsideTheRow);
  AddTest(tests, "QaFlowRegression/ACaretMoveThatScrollsTheMergePanesRepaintsThemAll",
          TestACaretMoveThatScrollsTheMergePanesRepaintsThemAll);
  AddTest(tests, "QaFlowRegression/EditorRowsRecolourWhenACommentOpensAboveThem",
          TestEditorRowsRecolourWhenACommentOpensAboveThem);
  AddTest(tests, "QaFlowRegression/EditorRowsRepaintTheirIndentGuides", TestEditorRowsRepaintTheirIndentGuides);
  AddTest(tests, "QaFlowRegression/CompareRowDamageFollowsEveryStateTransition",
          TestCompareRowDamageFollowsEveryStateTransition);
  AddTest(tests, "QaFlowRegression/MergeRowDamageFollowsEveryStateTransition",
          TestMergeRowDamageFollowsEveryStateTransition);
  AddTest(tests, "QaFlowRegression/EditorRowDamageFollowsEveryStateTransition",
          TestEditorRowDamageFollowsEveryStateTransition);
  AddTest(tests, "QaFlowRegression/ASettledFrameDerivesNoDamage", TestASettledFrameDerivesNoDamage);
  AddTest(tests, "QaFlowRegression/AnImePreeditRepaintsAsItChanges", TestAnImePreeditRepaintsAsItChanges);
  AddTest(tests, "QaFlowRegression/ADiagnosticInAnotherFileRepaintsNoEditorPane",
          TestADiagnosticInAnotherFileRepaintsNoEditorPane);
  AddTest(tests, "QaFlowRegression/TheHoverCardFollowsARepublishedDiagnosticUnderAStillPointer",
          TestTheHoverCardFollowsARepublishedDiagnosticUnderAStillPointer);
  AddTest(tests, "QaFlowRegression/EditorRowsRepaintWhatChangedWithNoDamageRequest",
          TestEditorRowsRepaintWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/TheStatusBarRepaintsWhatChangedWithNoDamageRequest",
          TestTheStatusBarRepaintsWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/ThePanelHeaderRepaintsWhatChangedWithNoDamageRequest",
          TestThePanelHeaderRepaintsWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/TheOutputRowsRepaintWhatChangedWithNoDamageRequest",
          TestTheOutputRowsRepaintWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/AnAppendToAHiddenOutputChannelRepaintsNothingInThePanel",
          TestAnAppendToAHiddenOutputChannelRepaintsNothingInThePanel);
  AddTest(tests, "QaFlowRegression/PartialFramesStayFreshAfterFullFramesAtANewSize",
          TestPartialFramesStayFreshAfterFullFramesAtANewSize);
  AddTest(tests, "QaFlowRegression/RepublishedTextStylesAndGutterMarksRepaint",
          TestRepublishedTextStylesAndGutterMarksRepaint);
  AddTest(tests, "QaFlowRegression/ABannerRepaintsFromAFrame", TestABannerRepaintsFromAFrame);
  AddTest(tests, "QaFlowRegression/ThePromptsCaretGoesWhenTheWindowLosesTheFocus",
          TestThePromptsCaretGoesWhenTheWindowLosesTheFocus);
  AddTest(tests, "QaFlowRegression/ATreeDragsPreviewAndGhostRepaintFromAFrame",
          TestATreeDragsPreviewAndGhostRepaintFromAFrame);
  AddTest(tests, "QaFlowRegression/ATabActivatedWithNoRequestRepaintsItsStrip",
          TestATabActivatedWithNoRequestRepaintsItsStrip);
  AddTest(tests, "QaFlowRegression/AHoverCardThatSaysSomethingElseRepaints",
          TestAHoverCardThatSaysSomethingElseRepaints);
  AddTest(tests, "QaFlowRegression/AStillFrameDamagesNothing", TestAStillFrameDamagesNothing);
  AddTest(tests, "QaFlowRegression/TheProblemsListRepaintsItsRowsFromAFrame",
          TestTheProblemsListRepaintsItsRowsFromAFrame);
  AddTest(tests, "QaFlowRegression/TheFindWidgetRepaintsItsFieldsFromAFrame", TestTheFindWidgetRepaintsItsFieldsFromAFrame);
  AddTest(tests, "QaFlowRegression/TheCommandPaletteRepaintsWhatChangedFromAFrame",
          TestTheCommandPaletteRepaintsWhatChangedFromAFrame);
  AddTest(tests, "QaFlowRegression/FoldMarksFollowAnEditFrameByFrame", TestFoldMarksFollowAnEditFrameByFrame);
  AddTest(tests, "QaFlowRegression/APromptsChangesRepaintItFromAFrame", TestAPromptsChangesRepaintItFromAFrame);
  AddTest(tests, "QaFlowRegression/EveryButtonOfTheUnsavedChangesPromptLightsUnderThePointer",
          TestEveryButtonOfTheUnsavedChangesPromptLightsUnderThePointer);
  AddTest(tests, "QaFlowRegression/EditorChangesRepaintTheirRowsNotThePane",
          TestEditorChangesRepaintTheirRowsNotThePane);
  AddTest(tests, "QaFlowRegression/AHoveredTerminalContextMenuRowRepaintsWhole",
          TestAHoveredTerminalContextMenuRowRepaintsWhole);
  AddTest(tests, "QaFlowRegression/ARepublishedCodeLensRepaintsWhatItSays",
          TestARepublishedCodeLensRepaintsWhatItSays);
  AddTest(tests, "QaFlowRegression/AGutterThatWidensRepaintsThePaneAboveTheFirstRow",
          TestAGutterThatWidensRepaintsThePaneAboveTheFirstRow);
  AddTest(tests, "QaFlowRegression/SwitchingThePanelFromAPluginSurfaceToOutputRepaintsTheBody",
          TestSwitchingThePanelFromAPluginSurfaceToOutputRepaintsTheBody);
  AddTest(tests, "QaFlowRegression/TheTerminalChromeRepaintsWhatChangedWithNoDamageRequest",
          TestTheTerminalChromeRepaintsWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/ThePanelDividerShowsAndLightsWhileDragged",
          TestThePanelDividerShowsAndLightsWhileDragged);
  AddTest(tests, "QaFlowRegression/AToastAsksForAFrameNotAFullRepaint", TestAToastAsksForAFrameNotAFullRepaint);
  AddTest(tests, "QaFlowRegression/TheNotificationToastsRepaintWhatChangedWithNoDamageRequest",
          TestTheNotificationToastsRepaintWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/ANewLineInTheMergeResultRepaintsTheVerticalThumb",
          TestANewLineInTheMergeResultRepaintsTheVerticalThumb);
  AddTest(tests, "QaFlowRegression/MovingOntoATabsCloseButtonLightsIt", TestMovingOntoATabsCloseButtonLightsIt);
  AddTest(tests, "QaFlowRegression/ATabStaysInsideItsNarrowStrip", TestATabStaysInsideItsNarrowStrip);
  AddTest(tests, "QaFlowRegression/MovingATabKeepsEveryFrameFresh", TestMovingATabKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/MovingATabWhoseFileChangedKeepsEveryFrameFresh",
          TestMovingATabWhoseFileChangedKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/AnEditInOneHalfOfASplitShowsInTheOther",
          TestAnEditInOneHalfOfASplitShowsInTheOther);
  AddTest(tests, "QaFlowRegression/EveryElementStaysInsideTheWindowAtEverySize",
          TestEveryElementStaysInsideTheWindowAtEverySize);
  AddTest(tests, "QaFlowRegression/AnEditThatAddsTheHorizontalScrollbarRepaintsIt",
          TestAnEditThatAddsTheHorizontalScrollbarRepaintsIt);
  AddTest(tests, "QaFlowRegression/TheCompactLayoutReturnsWhenTheWindowGrows",
          TestTheCompactLayoutReturnsWhenTheWindowGrows);
  AddTest(tests, "QaFlowRegression/EscapeReturnsFocusToWhereTheOverlayOpened",
          TestEscapeReturnsFocusToWhereTheOverlayOpened);
  AddTest(tests, "QaFlowRegression/ATabDraggedAcrossACloseButtonLeavesItIdle",
          TestATabDraggedAcrossACloseButtonLeavesItIdle);
  AddTest(tests, "QaFlowRegression/AHeldTabDragPaintsItsLastSnap", TestAHeldTabDragPaintsItsLastSnap);
  AddTest(tests, "QaFlowRegression/SavingADeletedFileWritesItBack", TestSavingADeletedFileWritesItBack);
  AddTest(tests, "QaFlowRegression/CommandPaletteListsRecentCommandsFirst", TestCommandPaletteListsRecentCommandsFirst);
  AddTest(tests, "QaFlowRegression/ReverseExecutionIsRefusedWithoutAReverseAdapter",
          TestReverseExecutionIsRefusedWithoutAReverseAdapter);
  AddTest(tests, "QaFlowRegression/DetectedIndentationSurvivesTabSwitchesAndSettings",
          TestDetectedIndentationSurvivesTabSwitchesAndSettings);
  AddTest(tests, "QaFlowRegression/ModalsOwnThePointer", TestModalsOwnThePointer);
  AddTest(tests, "QaFlowRegression/HoverTextKeepsItsLines", TestHoverTextKeepsItsLines);
  AddTest(tests, "QaFlowRegression/DebugSessionRepaintsToolbarAndOutput", TestDebugSessionRepaintsToolbarAndOutput);
  AddTest(tests, "QaFlowRegression/DebugToolbarDoesNotHoverUnderAModal", TestDebugToolbarDoesNotHoverUnderAModal);
  AddTest(tests, "QaFlowRegression/TheDebugToolbarCoversNoOtherControl", TestTheDebugToolbarCoversNoOtherControl);
  AddTest(tests, "QaFlowRegression/ACodeLensStaysInsideItsPane", TestACodeLensStaysInsideItsPane);
  AddTest(tests, "QaFlowRegression/TheMergeEditorsColumnsPaintInsideThemselves", TestTheMergeEditorsColumnsPaintInsideThemselves);
  AddTest(tests, "QaFlowRegression/AMergeToolbarButtonLosesItsHoverWhenThePointerLeaves",
          TestAMergeToolbarButtonLosesItsHoverWhenThePointerLeaves);
  AddTest(tests, "QaFlowRegression/HelpAboutKeepsALongCommandInsideTheCard", TestHelpAboutKeepsALongCommandInsideTheCard);
  AddTest(tests, "QaFlowRegression/ASettingsRowValueClickRepaintsTheRows", TestASettingsRowValueClickRepaintsTheRows);
  AddTest(tests, "QaFlowRegression/ExceptionFiltersStayListedAfterAStop", TestExceptionFiltersStayListedAfterAStop);
  AddTest(tests, "QaFlowRegression/StopWithNoSessionLeavesTheDebugPane", TestStopWithNoSessionLeavesTheDebugPane);
  AddTest(tests, "QaFlowRegression/SidebarViewTabsStayInsideANarrowSidebar", TestSidebarViewTabsStayInsideANarrowSidebar);
  AddTest(tests, "QaFlowRegression/ClickingTheOtherGroupAfterATreeDragRepaintsTheTabStrips",
          TestClickingTheOtherGroupAfterATreeDragRepaintsTheTabStrips);
  AddTest(tests, "QaFlowRegression/AClickThatClearsASelectionUnderTheFindWidgetRepaintsIt",
          TestAClickThatClearsASelectionUnderTheFindWidgetRepaintsIt);
  AddTest(tests, "QaFlowRegression/AClickThatClearsATerminalSelectionUnderTheFindWidgetRepaintsIt",
          TestAClickThatClearsATerminalSelectionUnderTheFindWidgetRepaintsIt);
  AddTest(tests, "QaFlowRegression/ATerminalSelectionChangeWithNoDamageRepaints",
          TestATerminalSelectionChangeWithNoDamageRepaints);
  AddTest(tests, "QaFlowRegression/FocusingAnotherGroupByCommandRepaintsBothPanes",
          TestFocusingAnotherGroupByCommandRepaintsBothPanes);
  AddTest(tests, "QaFlowRegression/RedoAfterASaveInASplitKeepsBothPanesFresh",
          TestRedoAfterASaveInASplitKeepsBothPanesFresh);
  AddTest(tests, "QaFlowRegression/LeavingAProblemsRowRepaintsItsHover", TestLeavingAProblemsRowRepaintsItsHover);
  AddTest(tests, "QaFlowRegression/AnEditThatOpensABlockCommentRepaintsTheLinesBelow",
          TestAnEditThatOpensABlockCommentRepaintsTheLinesBelow);
  AddTest(tests, "QaFlowRegression/AFocusedPaneShortenedFromAnotherGroupShowsItsReconciledCaret",
          TestAFocusedPaneShortenedFromAnotherGroupShowsItsReconciledCaret);
  AddTest(tests, "QaFlowRegression/ANarrowSearchToggleHoverKeepsEveryFrameFresh",
          TestANarrowSearchToggleHoverKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/AClickBelowADiffsLastRowRepaintsTheWorkingSide",
          TestAClickBelowADiffsLastRowRepaintsTheWorkingSide);
  AddTest(tests, "QaFlowRegression/ConfirmingASidebarPromptOverAHiddenSidebarKeepsTheKeyboard",
          TestConfirmingASidebarPromptOverAHiddenSidebarKeepsTheKeyboard);
  AddTest(tests, "QaFlowRegression/EveryElementStaysInsideItsSurfaceAtEverySize",
          TestEveryElementStaysInsideItsSurfaceAtEverySize);
  AddTest(tests, "QaFlowRegression/TallMenusStayInsideTheWindow", TestTallMenusStayInsideTheWindow);
  AddTest(tests, "QaFlowRegression/F1AndCtrlNumberFocusGroups", TestF1AndCtrlNumberFocusGroups);
  AddTest(tests, "QaFlowRegression/DebugPaneModeTabHoverRepaints", TestDebugPaneModeTabHoverRepaints);
  AddTest(tests, "QaFlowRegression/AnEditThatAddsAFoldMarkerRepaintsTheGutter",
          TestAnEditThatAddsAFoldMarkerRepaintsTheGutter);
  AddTest(tests, "QaFlowRegression/GitCommitButtonHoverRepaints", TestGitCommitButtonHoverRepaints);
  AddTest(tests, "QaFlowRegression/CtrlFSeedsTheWordUnderTheCaret", TestCtrlFSeedsTheWordUnderTheCaret);
  AddTest(tests, "QaFlowRegression/CtrlHInTheFindWidgetOpensReplace", TestCtrlHInTheFindWidgetOpensReplace);
  AddTest(tests, "QaFlowRegression/ShiftAltRightExpandsTheSelection", TestShiftAltRightExpandsTheSelection);
  AddTest(tests, "QaFlowRegression/TreeF2RenamesAndDeleteDeletes", TestTreeF2RenamesAndDeleteDeletes);
  AddTest(tests, "QaFlowRegression/QuickOpenPrefixesAndLineSuffix", TestQuickOpenPrefixesAndLineSuffix);
  AddTest(tests, "QaFlowRegression/TypingAfterCtrlEndRepaintsTheLastLine", TestTypingAfterCtrlEndRepaintsTheLastLine);
  AddTest(tests, "QaFlowRegression/DoubleClickOnEmptyTabStripOpensUntitled",
          TestDoubleClickOnEmptyTabStripOpensUntitled);
  AddTest(tests, "QaFlowRegression/ShiftWheelScrollsTheEditorSideways", TestShiftWheelScrollsTheEditorSideways);
  AddTest(tests, "QaFlowRegression/CtrlShiftFSeedsTheSelection", TestCtrlShiftFSeedsTheSelection);
  AddTest(tests, "QaFlowRegression/CtrlKChordSurvivesReleasingCtrl", TestCtrlKChordSurvivesReleasingCtrl);
  AddTest(tests, "QaFlowRegression/ColumnSelectionNeverSplitsACharacter", TestColumnSelectionNeverSplitsACharacter);
  AddTest(tests, "QaFlowRegression/DeleteInTheBreakpointsPaneRemovesIt", TestDeleteInTheBreakpointsPaneRemovesIt);
  AddTest(tests, "QaFlowRegression/BreakpointCommandsRepaintTheGutterAndThePane",
          TestBreakpointCommandsRepaintTheGutterAndThePane);
  AddTest(tests, "QaFlowRegression/ALayoutChangeWithNoDamageOfItsOwnRepaints",
          TestALayoutChangeWithNoDamageOfItsOwnRepaints);
  AddTest(tests, "QaFlowRegression/AutosaveOnFocusChangeCoversEveryWayTheEditorLosesFocus",
          TestAutosaveOnFocusChangeCoversEveryWayTheEditorLosesFocus);
  AddTest(tests, "QaFlowRegression/CtrlShiftSSavesANamedFileAs", TestCtrlShiftSSavesANamedFileAs);
  AddTest(tests, "QaFlowRegression/DecorationsFollowTheBufferGoingDirtyAndClean",
          TestDecorationsFollowTheBufferGoingDirtyAndClean);
  AddTest(tests, "QaFlowRegression/ColorschemeListSaysWhichSchemesThereAre",
          TestColorschemeListSaysWhichSchemesThereAre);
  AddTest(tests, "QaFlowRegression/TypingOnTheDiffsWorkingSideNeverRunsReviewLetters",
          TestTypingOnTheDiffsWorkingSideNeverRunsReviewLetters);
  AddTest(tests, "QaFlowRegression/CompareHunkStepRepaintsTheSelectionMarkers",
          TestCompareHunkStepRepaintsTheSelectionMarkers);
  AddTest(tests, "QaFlowRegression/CompareHeaderRepaintsWhenAnEditChangesIt",
          TestCompareHeaderRepaintsWhenAnEditChangesIt);
  AddTest(tests, "QaFlowRegression/AltLeftRightNavigateBackAndForward",
          TestAltLeftRightNavigateBackAndForward);
  AddTest(tests, "QaFlowRegression/CtrlShiftTReopensClosedEditorsNewestFirst",
          TestCtrlShiftTReopensClosedEditorsNewestFirst);
  AddTest(tests, "QaFlowRegression/HoverCardDoesNotSwallowClicksOnThePalette",
          TestHoverCardDoesNotSwallowClicksOnThePalette);
  AddTest(tests, "QaFlowRegression/UserDefaultReachesAnOpenProjectOnSwitch",
          TestUserDefaultReachesAnOpenProjectOnSwitch);
  AddTest(tests, "QaFlowRegression/UserDefaultReachesAProjectOpenedForTheFirstTime",
          TestUserDefaultReachesAProjectOpenedForTheFirstTime);
  AddTest(tests, "QaFlowRegression/PaletteRowWhoseLabelStartsWithACommandRunsTheRow",
          TestPaletteRowWhoseLabelStartsWithACommandRunsTheRow);
  AddTest(tests, "QaFlowRegression/PaletteAsksForTheArgumentsACommandNeeds",
          TestPaletteAsksForTheArgumentsACommandNeeds);
  AddTest(tests, "QaFlowRegression/AnAutomaticGitRefreshInsideTheIntervalIsKept",
          TestAnAutomaticGitRefreshInsideTheIntervalIsKept);
  AddTest(tests, "QaFlowRegression/TheGitPanelHeadRepaintsWhatChangedWithNoDamageRequest",
          TestTheGitPanelHeadRepaintsWhatChangedWithNoDamageRequest);
  AddTest(tests, "QaFlowRegression/AGitRowClickLandsOnThePaintedRowAfterTheListShrinks",
          TestAGitRowClickLandsOnThePaintedRowAfterTheListShrinks);
  AddTest(tests, "QaFlowRegression/TheGitListWheelScrollsTheLiveList",
          TestTheGitListWheelScrollsTheLiveList);
  AddTest(tests, "QaFlowRegression/TreeGitBadgesRepaintWhenTheGitSelectionSurvives",
          TestTreeGitBadgesRepaintWhenTheGitSelectionSurvives);
  AddTest(tests, "QaFlowRegression/MarkingAnOutgoingFileReviewedMarksItsRow",
          TestMarkingAnOutgoingFileReviewedMarksItsRow);
  AddTest(tests, "QaFlowRegression/CtrlQQuits", TestCtrlQQuits);
  AddTest(tests, "QaFlowRegression/ClosingTheLastTerminalUnderAnOutputChannelRepaintsTheTabStrip",
          TestClosingTheLastTerminalUnderAnOutputChannelRepaintsTheTabStrip);
  AddTest(tests, "QaFlowRegression/TypingOnAFoldedOpenerRepaintsItsRow",
          TestTypingOnAFoldedOpenerRepaintsItsRow);
  AddTest(tests, "QaFlowRegression/AnOverlayIsPaintedOverTheBottomPanel", TestAnOverlayIsPaintedOverTheBottomPanel);
  AddTest(tests, "QaFlowRegression/TypingAtTheEndUnderAStickyBandRepaintsTheTypedRow",
          TestTypingAtTheEndUnderAStickyBandRepaintsTheTypedRow);
  AddTest(tests, "QaFlowRegression/TheCaretOnTheLastLineStaysInViewWhereAFoldEndsNearTheEnd",
          TestTheCaretOnTheLastLineStaysInViewWhereAFoldEndsNearTheEnd);
  AddTest(tests, "QaFlowRegression/TypingWhereTheEndLeavesAFoldRepaintsTheTypedRow",
          TestTypingWhereTheEndLeavesAFoldRepaintsTheTypedRow);
  AddTest(tests, "QaFlowRegression/TheMenuOverflowChevronIsAHoverTarget", TestTheMenuOverflowChevronIsAHoverTarget);
  AddTest(tests, "QaFlowRegression/TabOverASelectionEndingOnTheLastLineRepaintsTheLineTheCaretLeft",
          TestTabOverASelectionEndingOnTheLastLineRepaintsTheLineTheCaretLeft);
  AddTest(tests, "QaFlowRegression/ATabTooltipNamesTheTabUnderThePointerInASplit",
          TestATabTooltipNamesTheTabUnderThePointerInASplit);
  AddTest(tests, "QaFlowRegression/TheUnsavedChangesPromptRepaintsWhenWhatItShowsChanges",
          TestTheUnsavedChangesPromptRepaintsWhenWhatItShowsChanges);
  AddTest(tests, "QaFlowRegression/TheUnsavedChangesPromptActsOnItsTabWhereverTheFocusMoved",
          TestTheUnsavedChangesPromptActsOnItsTabWhereverTheFocusMoved);
  AddTest(tests, "QaFlowRegression/ClickingAnUnfocusedGroupsOverflowChipRepaintsItsStrip",
          TestClickingAnUnfocusedGroupsOverflowChipRepaintsItsStrip);
  AddTest(tests, "QaFlowRegression/AFoldChordRightAfterAClickFoldsTheClickedBlock",
          TestAFoldChordRightAfterAClickFoldsTheClickedBlock);
  AddTest(tests, "QaFlowRegression/CommandRefusalsSayWhatIsWrong", TestCommandRefusalsSayWhatIsWrong);
  AddTest(tests, "QaFlowRegression/TheDiffsOwnVerbsAreCommands", TestTheDiffsOwnVerbsAreCommands);
  AddTest(tests, "QaFlowRegression/TheMergeEditorCopiesAConflictSide", TestTheMergeEditorCopiesAConflictSide);
  AddTest(tests, "QaFlowRegression/ATerminalTitleRepaintsItsTab", TestATerminalTitleRepaintsItsTab);
  AddTest(tests, "QaFlowRegression/ATerminalPaneCursorChangeRepaintsItsCaret",
          TestATerminalPaneCursorChangeRepaintsItsCaret);
  AddTest(tests, "QaFlowRegression/TerminalOutputSampledByFramePrepRepaintsItsPane",
          TestTerminalOutputSampledByFramePrepRepaintsItsPane);
  AddTest(tests, "QaFlowRegression/ASidebarScrollOfOneRowRepaints", TestASidebarScrollOfOneRowRepaints);
  AddTest(tests, "QaFlowRegression/SaveAsRevealsTheNewFileInTheTree", TestSaveAsRevealsTheNewFileInTheTree);
  AddTest(tests, "QaFlowRegression/TheGitViewReportsTheSelectedEntrysRow", TestTheGitViewReportsTheSelectedEntrysRow);
  AddTest(tests, "QaFlowRegression/ReloadFromDiskRefusesADirtyBufferWithWhy", TestReloadFromDiskRefusesADirtyBufferWithWhy);
  AddTest(tests, "QaFlowRegression/NothingBeneathAnOpenMenuHovers", TestNothingBeneathAnOpenMenuHovers);
  AddTest(tests, "QaFlowRegression/ADiffPaintsInsideItsPane", TestADiffPaintsInsideItsPane);
  AddTest(tests, "QaFlowRegression/TypingThatReshapesADiffRepaintsIt", TestTypingThatReshapesADiffRepaintsIt);
  AddTest(tests, "QaFlowRegression/TheRemoteSegmentOpensTheRemoteMenu", TestTheRemoteSegmentOpensTheRemoteMenu);
  AddTest(tests, "QaFlowRegression/AFocusMoveByTheMaximizedPanelRulePaintsTheRingInItsFrame",
          TestAFocusMoveByTheMaximizedPanelRulePaintsTheRingInItsFrame);
  AddTest(tests, "QaFlowRegression/TypingUnderAStickyBandRepaintsTheTypedRow",
          TestTypingUnderAStickyBandRepaintsTheTypedRow);
  AddTest(tests, "QaFlowRegression/ACommitFieldUnderAModalPromptTakesNoClickAndPaintsNoCaret",
          TestACommitFieldUnderAModalPromptTakesNoClickAndPaintsNoCaret);
  AddTest(tests, "QaFlowRegression/TypingUnderACodeLensStripRepaintsTheTypedLine",
          TestTypingUnderACodeLensStripRepaintsTheTypedLine);
  AddTest(tests, "QaFlowRegression/RightClicksUnderAStickyBandActOnTheClickedLine",
          TestRightClicksUnderAStickyBandActOnTheClickedLine);
  AddTest(tests, "QaFlowRegression/PartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit",
          TestPartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit);
  AddTest(tests, "QaFlowRegression/ClosingAMenuOverAFindToggleRepaintsItsHover",
          TestClosingAMenuOverAFindToggleRepaintsItsHover);
  AddTest(tests, "QaFlowRegression/TabIndentingASecondarySelectionRepaintsEachOfItsLines",
          TestTabIndentingASecondarySelectionRepaintsEachOfItsLines);
  AddTest(tests, "QaFlowRegression/HoveringARowAfterAltRightToTheNextMenuRepaints",
          TestHoveringARowAfterAltRightToTheNextMenuRepaints);
  AddTest(tests, "QaFlowRegression/AnOpenMenuRepaintsARowWhoseEnablementChanges",
          TestAnOpenMenuRepaintsARowWhoseEnablementChanges);
  AddTest(tests, "QaFlowRegression/APaletteCardOverTheMenuBarKeepsItsItemUnlit",
          TestAPaletteCardOverTheMenuBarKeepsItsItemUnlit);
  AddTest(tests, "QaFlowRegression/LeavingAFindToggleOverTheSidebarRepaintsIt",
          TestLeavingAFindToggleOverTheSidebarRepaintsIt);
}

}
