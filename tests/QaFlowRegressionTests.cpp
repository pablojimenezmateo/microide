// Regressions the GUI QA flows (tools/qa/flows_*.py) found, pinned in process.
//
// Each test reproduces one finding the way the harness saw it: real events
// through HandleEvent, the redraw folded as the app loop folds it, painted over
// the retained canvas and compared with a full repaint of the same state
// (tests/support/RetainedRedraw.h) -- or the behavioural expectation the flow
// stated, checked against the same queries the harness reads.
#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "TerminalSessionTestAccess.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "WorkspaceShellEventHelpers.h"
#include "support/GitSidebarWait.h"
#include "support/MockDapAdapter.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "editor/FoldingModel.h"
#include "editor/PluginDecorationStore.h"
#include "util/JsonValue.h"
#include "workspace/HoverTextWrap.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Pins the caret's blink phase before each of the two paints a redraw diff makes.
// Unpinned, a slow run (TSan) crossed a blink between the partial frame and the
// full repaint and diffed the caret (CI: 1x18 pixels in CompareHunkStep...).
std::function<void()> PinCaretOf(WorkspaceShell& shell) {
  return [&shell]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
}

struct FlowFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;

  explicit FlowFixture(std::string_view file_name, std::string_view content) {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    file = root / std::string(file_name);
    WriteFile(file, std::string(content));
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the flow fixture should open its project");
    TestAccess::SetWindowSize(shell, kWidth, kHeight);
    TestAccess::OpenSingleEditorTab(shell, file);
    TestAccess::ShowTreeSidebar(shell);
    TestAccess::SetFocusEditor(shell);
    TestAccess::NoteLayoutInputsChanged(shell);
    PinCaret();
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  void PinCaret() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); }

  // The element the ui query names `name`, or nullopt.
  std::optional<SDL_FRect> ElementRect(std::string_view name) {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == name) {
        const auto& r = entry["rect"].AsArray();
        return SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
      }
    }
    return std::nullopt;
  }

  workspace::RenderInvalidation Click(const SDL_FRect& rect) {
    const float x = rect.x + rect.w / 2.0f;
    const float y = rect.y + rect.h / 2.0f;
    return DispatchEvents(shell, {MouseMotionEvent(x, y),
                                  MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                  MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  }

  // Paints `redraw` as the app would and expects it to equal a full repaint.
  void ExpectFresh(const workspace::RenderInvalidation& redraw, const std::string& what) {
    if (const char* dump = std::getenv("MQA_TEST_DUMP"); dump != nullptr) {
      for (const auto& r : redraw.rects) {
        std::fprintf(stderr, "%s: rect %.0f,%.0f %.0fx%.0f\n", what.c_str(), r.x, r.y, r.w, r.h);
      }
    }
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw,
                                                  [this]() { PinCaret(); });
    Expect(diff.identical(), what + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }

  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  std::filesystem::path file;
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
};

// branch review: a file marked reviewed from the diff an Outgoing row opens showed
// no marker on that row. The diff stored the mark under its absolute path and the
// git view looked it up under git's repository-relative one. And with no
// branch-review diff open the command reported success and did nothing.
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
  const util::JsonValue before = TestAccess::ControlUi(shell);  // a named value: the loop must not outlive it
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
  // The ui query names the row as before and labels it as painted.
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

// unsaved-continuations: Ctrl+Q did nothing; Quit had no key, while VS Code on
// Linux quits on Ctrl+Q. From the editor and from a focused terminal alike (the
// binding skips the shell, where Ctrl+Q is XON).
// The git view's keyboard selection is an ENTRY index; the rows the `ui` query
// lists are lines, with section headers and directories among them. The query
// reported the entry index as a line index, so with "Unstaged (3)" and a
// directory row above the files, the row it called selected was two above the
// one painted and acted on (QA git-view-keys read Enter as opening the wrong file).
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
  SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE);  // the second entry: beta.txt
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

// Reload from Disk on a dirty buffer did nothing and said nothing (the reload
// refuses rather than throw the edits away, and the executor answered
// "handled"). It now refuses with why, and the edit stays.
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

// A buffer saved under a new name is a new file: the tree selects and reveals
// it, as activating a tab does. It kept the previous selection, so F2 after a
// first save renamed whatever row was selected before (QA remote scenario N).
void TestSaveAsRevealsTheNewFileInTheTree() {
  FlowFixture fixture("a.txt", "a\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "save notes/copy.txt"), "save as a new path");
  Expect(TestAccess::SelectedTreePath(fixture.shell) == fixture.root / "notes" / "copy.txt",
         "the tree selects the file the buffer was saved as (" +
             TestAccess::SelectedTreePath(fixture.shell).string() + ")");
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
  Expect(fixture.shell.ConsumeQuitRequested(), "Ctrl+Q in a focused terminal should quit too");
}

// monkey (seed 1, step 223): `term-close` on the last terminal while the panel
// showed an output channel left the closed terminal's tab on the panel's tab
// strip. Closing it changed neither the panel's content nor whether it was
// visible, and those were the only changes that asked for a repaint.
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

// qa-wayland terminal-programs: a shell setting its title (OSC 0) renames its tab
// in the panel strip, but only the terminal's body was repainted for the output
// that carried it, so the strip kept "bash" until something else covered it.
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
  // What the output's arrival repaints: the terminal's body, below the strip.
  workspace::RenderInvalidation body;
  body.rects.push_back(SDL_FRect{static_cast<float>(panel[0].AsInt()),
                                 static_cast<float>(panel[1].AsInt()) + workspace::kWorkspaceBottomPanelHeaderHeight,
                                 static_cast<float>(panel[2].AsInt()),
                                 static_cast<float>(panel[3].AsInt()) - workspace::kWorkspaceBottomPanelHeaderHeight});
  fixture.ExpectFresh(body, "the shell setting its title");
}

// QA monkey (seed 1, step 186): with the terminal split, the inactive pane's
// caret outline stayed on screen after its cursor hid (or moved) with no row of
// text changing. Frame prep samples each pane's cursor but compared it with
// nothing, so the old caret rect was only repainted if something else covered it.
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

  // The cursor hides (DECTCEM), then moves while hidden: no row's text changes.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?25l");
  // A frame drawn for something else (the blink of another caret, say).
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{2.0f, 2.0f, 4.0f, 4.0f});
  fixture.ExpectFresh(elsewhere, "an inactive pane's cursor hiding");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?25h\x1b[3;5H");
  fixture.ExpectFresh(elsewhere, "an inactive pane's cursor showing again elsewhere");
}

// The sidebar's repaint-on-change check keyed the view, selection, scroll and
// tree revision into one value and then forced it non-zero with `key |= 1`,
// which erased any change living only in bit 0 of the last term: in the git,
// problems and search views a one-row scroll from an even row (2 to 3) that
// landed between frames repainted nothing.
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
  // A frame drawn for something else.
  workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(SDL_FRect{kWidth - 6.0f, kHeight - 6.0f, 4.0f, 4.0f});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, elsewhere,
                                                [&]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); });
  Expect(diff.identical(), "a one-row scroll of the git view should repaint it; " + DescribePixelDiff(diff));
}

// QA monkey (seed 1, step 254): with a menu open, the pointer moved over the
// sidebar and a button there painted hovered -- with no repaint asked for it, so
// a clip that happened to cover part of it left the rest unhovered. An open menu
// owns the pointer (a press outside it only closes it): nothing beneath it
// hovers, as nothing beneath a modal does.
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

// QA monkey (seed 2, step 62): typing on a diff's working side can regroup the
// whole diff while the keystroke's damage names only the edited line. The left
// side's first line was deleted and its empty second line paired with the
// working side's empty first line; typing there made that pair a change too, so
// the hunk grew a row and every row below it moved down -- on screen they stayed.
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

// edit-find-replace: Replace (and Replace All) from the find widget made a clean
// buffer dirty without repainting the tab strip, so the tab kept its close
// button where a dirty dot belonged until something else repainted it.
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

// edit-external-change: Reload on the external-change banner made a dirty buffer
// clean without repainting the tab strip (the dirty dot stayed).
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

// edit-folding: typing at the end of a collapsed fold's opener line left that row
// stale (the typed text and the inline blame the edit hides).
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

// indentation: Tab over two lines indented them under a closing brace, which made
// that brace's line the opener of an indentation fold. The edit repainted the two
// lines it changed, not the gutter of the line above that gained a fold marker.
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
  // As the flow did it: a click on line 4 (the pointer stays over the text).
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

// wb-editor-groups: dragging a tab over another pane paints VS Code's drop
// preview (an accent-tinted half or whole pane). The chrome pass draws it, and a
// partial frame whose damage missed every tab strip skipped that pass, so the
// preview never appeared on screen -- and its last position stayed after it moved.
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
  // Eight even steps from the tab to the middle of the right pane, as a person's
  // drag (and the QA flow's) goes: across the left pane's body on the way.
  const float x1 = static_cast<float>(FlowFixture::kWidth) * 0.8f;
  const float body_y = static_cast<float>(FlowFixture::kHeight) * 0.45f;
  bool previewed = false;
  for (int step = 1; step <= 8; ++step) {
    const float x = x0 + (x1 - x0) * static_cast<float>(step) / 8.0f;
    const float y = y0 + (body_y - y0) * static_cast<float>(step) / 8.0f;
    SDL_Event motion = MouseMotionEvent(x, y);
    motion.motion.state = SDL_BUTTON_LMASK;  // the button is held: a drag
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

// QA monkey (CI qa-asan, seed 2): a tab dragged along its own strip, leftwards
// across another tab's close button, left that button painted hovered after the
// drop -- the full render drew it idle. Each drag step and the drop must paint
// what a full repaint paints.
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
  // Past the close button, onto the body of second.txt's tab.
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

  // Under load the driver's last motion and its release arrive apart: the last
  // motion the app saw was over a close button, the release is beside it.
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

// QA plugins-reload: Escape on the completion list moved keyboard focus to the
// file tree, so the next key (a Backspace) edited nothing -- every overlay that
// closed "without a target" (the palette, completion, code actions, the commit
// and launch-config pickers) handed focus to the sidebar whenever it was shown,
// wherever the overlay had been opened from. VS Code returns focus to where it
// was.
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

// QA resize-overlays: a window shrunk into the compact layout never came back --
// at 1440 px the menus stayed folded behind the chevron. The frame recorded the
// resolved layout mode only when IT recomputed the layout memo; after a resize a
// hit test or a ui query often recomputed it first, the frame saw a memo hit, and
// the stale "compact" (which the resolver reads back as its previous mode, and
// the memo keys on) held for good.
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
  (void)menus();  // a ui query (or a hit test) resolves the layout before the frame does
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  Expect(menus() > 0, "grown back, the menu bar shows its titles again");
}

// QA diff-commands: pasting a line longer than the editor is wide made the
// horizontal scrollbar appear, and the paste damaged only the edited line -- the
// scrollbar's strip along the editor's bottom kept the old pixels. An edit that
// makes the bar appear (or go) repaints it.
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

// QA monkey (seed 6, the stranded-focus oracle): a prompt opened over the open
// file finder, the sidebar hidden meanwhile, then the prompt dismissed -- focus
// went back to the sidebar it was recorded from, which was no longer there, while
// the finder stayed open with no keyboard. What is still on top takes it.
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

// The QA monkey (seed 11, minimized to six actions): a confirmation opened from
// the sidebar -- Discard Git Changes, Delete, Rename, New Folder -- with the
// sidebar hidden under it gave the keyboard back to the hidden sidebar once
// confirmed, and every key went nowhere. Each continuation set the sidebar's
// focus without asking whether it was still shown.
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

// The QA monkey (seed 9, minimized to four actions): a drag in the file tree's
// empty space, a split, a click in the left group's empty strip and one in the
// left pane -- and the strips kept the right group's tab drawn as the active one.
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
  // The left group's strip past its one tab: empty space that focuses that group.
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

// The QA monkey (seed 16): with the find widget up, a double-click selected a
// word and a single click elsewhere took the selection away -- and the frame
// repainted only the find widget, leaving the word highlighted on screen.
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

// The same in the maximized terminal (seed 16 minimized to eight actions): with
// the find widget up, a double-click selected a word of the transcript and a
// click elsewhere took it away while the frame repainted only the find widget.
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
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "search"), "search opens the terminal's find bar");
  fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), "the find widget");
  const SDL_FPoint word = TestAccess::TerminalCellPoint(fixture.shell, 0, 12);
  const SDL_FPoint other = TestAccess::TerminalCellPoint(fixture.shell, 1, 3);
  for (int round = 0; round < 2; ++round) {
    // As a person (and xdotool) double-clicks: two presses, the second with clicks=2.
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

// The terminal's selection changed by a path with no damage of its own (QA
// monkey seed 16: a click under the terminal's find bar cleared a double-clicked
// word, and the frame repainted only the bar). Frame prep repaints the panel body
// whenever a pane's selection differs from what was painted.
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

// The QA monkey (commands profile, seed 8): focus-group-right moved the keyboard
// to the other pane, and only the focused pane draws its current-line highlight
// -- the panes kept the old one, repainted only where the strips are.
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

// The QA monkey (commands profile, seed 8, minimized to seven commands): a new
// file edited, split, saved from the left pane -- the save's trim shortened the
// document, leaving the right pane's caret past its end -- then focus-group-right
// and redo. The right pane drew no current-line highlight until redo clamped its
// caret, in a frame that repainted only the menu and status bars.
void TestRedoAfterASaveInASplitKeepsBothPanesFresh() {
  FlowFixture fixture("main.txt", "alpha\n");
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the start");
  // A path that does not exist yet: an empty buffer the save creates.
  for (const char* command : {"open on", "copy-line-down", "split-right", "focus-group-left", "save", "focus-group-right",
                              "redo", "redo"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, command) || std::string(command) == "redo", command);
    fixture.ExpectFresh(fixture.shell.ConsumePendingRenderInvalidation(), std::string("after ") + command);
  }
}

// QA commands-run (the bounds oracle): with the status bar hidden every toast
// was laid out above the window's top-left corner and never seen -- the stack is
// anchored on the status bar, and a hidden bar's rect was {0,0,0,0}. A hidden bar
// is now a zero-height strip along the window's bottom edge.
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

// QA monkey (seed 7): the pointer on a tab, then Ctrl+Shift+P. Under an open
// modal nothing beneath hovers (ModalsOwnThePointer), so a full render drew the
// tab idle -- and the window kept its hovered close glyph, because opening the
// palette repainted the palette and not the strip it took the hover from.
void TestOpeningAModalUnderThePointerDropsTheHoverItTakes() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  FlowFixture fixture("first.txt", "first\n");
  WriteFile(fixture.root / "second.txt", "second\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "second.txt");
  // Zoomed in once (Ctrl+=), as the monkey was: hover rects in layout units.
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
  // As the monkey had it: the tab clicked active, then edited (dirty).
  fixture.ExpectFresh(fixture.Click(*tab), "first.txt's tab clicked");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent("x")}), "an edit (the tab goes dirty)");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x - 20.0f, y)}), "the pointer back on the tab");
  fixture.ExpectFresh(
      DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_P, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))}),
      "Ctrl+Shift+P with the pointer on a tab");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x + 3.0f, y)}),
                      "a pointer move on the tab under the open palette");
}

// TD-2026-10-20-372 (CI's ASan monkey, seed 4): the sidebar divider dragged to
// the window's edge, and a scheduled partial frame left a strip at the editor's
// new left edge stale. Each motion of the drag, its release, and the scheduled
// wakes after it, against a full repaint.
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

// The sidebar's overflow (TD-372) generalized: every divider the ui names --
// the panel's, the debug pane's, an editor split's -- dragged to both of its
// extremes, a scheduled frame after each motion, each against a full repaint.
// A surface that paints past its own rect once it is small shows up here.
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
        continue;  // a previous drag closed or folded what holds it
      }
      const bool vertical = rect->h > rect->w;  // a vertical bar moves along x
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

// Two controls that partly cover each other: a hover lights one while a click
// lands on the other (the sidebar's ⋯ button over its view tabs, QA monkey seed
// 4). One inside the other (a tab's close button) is a nested control, not an
// overlap. Appends one line per pair to `out` (at most 40 lines in all).
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

// The window from 1280x720 down to 320x240 (and narrow-and-tall, wide-and-short),
// with every surface open -- sidebar, a split, the debug pane, the panel -- and
// then with the palette and Settings over it: at every size each element the ui
// names stays inside the window. The QA wb-layouts / resize-overlays flows look
// at four sizes; this looks at many, in process.
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
    // Under the palette or Settings the controls beneath are occluded, not overlapped.
    if (what == "every surface open") {
      CollectControlOverlaps(ui, what + " at " + std::to_string(width) + "x" + std::to_string(height),
                             violations);
    }
  };
  for (const auto& [w, h] : sizes) {
    check("every surface open", w, h);
  }
  // The stored size is the preference; a small window clamps the layout, not it.
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

// The narrow-sidebar finding (QA monkey seed 4) generalised: the sidebar, the
// debug pane and the panel each dragged from their smallest size to a wide one,
// and every element the ui names for a surface stays inside that surface -- a
// control drawn past its surface's edge is clipped in one frame and hit-tested
// in the next.
void TestEveryElementStaysInsideItsSurfaceAtEverySize() {
  FlowFixture fixture("main.txt", "alpha beta gamma delta\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting debug.enabled true"), "debug on");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "debug-pane-breakpoints"), "the debug pane opens");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "term"), "a terminal panel opens");
  // Which surface an element belongs to, by its name.
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

// QA rename-open-tabs: one file open in two editor groups shares one document,
// and an edit typed into one pane left the OTHER pane drawing the old line -- in
// a full repaint too, so the redraw verifier (which compares partial frames with
// full ones) never saw it. The buffer, the dirty marks and a save all had the
// edit; the other pane's visible-line layout cache, which the painter reads the
// row's text from, was keyed by line and spliced only by the pane that edited.
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

// QA merge-toolbar: Toggle Base painted the ancestor band UPWARD from the pane
// headers, into space the layout never reserved -- over the toolbar and the
// status line, its title over the breadcrumb -- and the partial frame of the
// editor surface clipped what a full repaint drew there. The layout reserves the
// band below the toolbar now, and the headers move down under it.
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

// QA merge-panes: a conflict scrolled out of sight still had buttons -- its
// accept buttons clamped to the bottom of the source panes and its result
// buttons fell back to the top of the result pane, where hovering line 1 showed
// them and a click applied a choice to a conflict 78 lines away. A conflict with
// no row on screen has no buttons, and the ui names only the buttons there are.
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
  // The file scrolls, so there is a vertical bar: beside the rows, not over the
  // toolbar, whose Save and Mark Resolved it cut off.
  const workspace::ScrollSurfaceLayout scroll = TestAccess::ActiveMergeScrollLayout(fixture.shell);
  Expect(scroll.vertical_scrollbar.has_value(), "a long merge has a vertical scrollbar");
  for (const char* button : {"button:merge/save", "button:merge/Mark Resolved"}) {
    const auto rect = fixture.ElementRect(button);
    Expect(rect.has_value() && scroll.vertical_scrollbar.has_value() &&
               !RectsIntersect(*rect, scroll.vertical_scrollbar->track),
           std::string(button) + " is clear of the scrollbar");
  }
  // Where its result buttons used to fall: the top of the result pane, beside the
  // first conflict's own row.
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

// The QA monkey (seed 1): moving from a tab's body onto its own close button
// left the close glyph unlit. The motion path repaints when the hovered element
// changes, and a tab reported its whole rect for both, so nothing repainted the
// glyph whose hover tint had changed. The close button is an element of its own.
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
  // And straight onto the close button from outside the tab: the tab's hover
  // fill changes too (QA monkey, seed 1, once the close button repainted alone).
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(tab->x + tab->w + 40.0f, y + 200.0f)}),
                      "off the strip");
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(close->x + close->w / 2.0f, y)}),
                      "onto the close button from outside the tab");
}

// The QA monkey (editor profile, seed 3): four editor groups side by side, the
// last two narrower than a tab. A group's tabs were laid out with at least 120 px
// of room whatever its strip had, so the third group's tab ran into the fourth's
// strip and the fourth's past the window -- where the ui named it and a click
// meant for the neighbour landed on it. A tab stays inside its own strip.
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

// The QA monkey (editor profile, seed 3): `tabmove` repainted the tab strip but
// not the pane under it, though the pane can change with it.
// With the moved tab's file rewritten on disk since it was painted (the monkey's
// churn): activating it may reload it, and the pane under the strip changes.
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

// wb-layouts: the Edit menu's 31 rows are taller than a 700-px window (the Debug
// menu's 21 than a 480-px one), and the rows past the bottom edge had rects no
// pointer could reach. A popup that fits neither below nor above its anchor now
// caps its height at the window and breaks its rows into columns; every row is
// on screen, and the pointer reaches the last one.
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
        // Hovering the last row highlights it: the hit test reaches it.
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

// wb-help: a modal owns the pointer. A click outside Help/About (or Settings)
// closes it, wherever it lands -- and a click on a status-bar segment under a
// modal must not run that segment's command beneath it.
void TestModalsOwnThePointer() {
  FlowFixture fixture("modal.txt", "modal\n");
  const auto modal = [&]() { return TestAccess::ControlUi(fixture.shell)["modal"]; };
  // Help/About (from the menu and from the keyboard-shortcuts command), then a
  // click in the status bar's empty left corner.
  // With the terminal panel open, as in a default window.
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
  // Settings, then a click on the line/column segment: closes Settings, opens no
  // Go to Line prompt behind it.
  TestAccess::OpenSettingsOverlay(fixture.shell);
  const auto segment = fixture.ElementRect("status:line-column");
  Expect(segment.has_value(), "the status bar should show the caret position");
  if (segment) {
    (void)fixture.Click(*segment);
    Expect(modal().IsNull(), "a click outside Settings should close it, got " + modal().AsString());
  }
  // The unsaved-changes prompt is modal too: a status segment click does nothing.
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

// lsp-hover: a hover card keeps its text's lines (each '\n' a line, a blank line
// a paragraph break), wraps words within a line, and caps the line count with
// the last line carrying the rest. It used to flatten every newline into one run.
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

// dbg-session: a stop or a resume repainted the debug pane and the menu bar, and
// nothing else -- but the floating toolbar's first button is Continue while the
// program is stopped and Pause while it runs, and the adapter's output lands in
// the output panel below. Both kept their old pixels.
void TestDebugSessionRepaintsToolbarAndOutput() {
#if !MICROIDE_HAS_SDL3_TTF || !(defined(__unix__) || defined(__APPLE__))
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
  // The QA run: stopped at a breakpoint, F5, and the program stopped again on
  // the same line -- so the editor had nothing to repaint and the stop's damage
  // was the menu bar and the debug pane. The mock re-stops at the same frame on
  // every resume, which is that case exactly.
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
  // The debug pane open beside the editor, as it is after the first stop in the
  // app: a stop repaints the pane, not the window.
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

// dbg-stepping on the CI ASan lane: with the Add Watch prompt up, the pointer over
// the floating debug toolbar lit its Restart button in a full frame and not in
// the partial one. The toolbar read the raw pointer, not PointerOver, so it was
// the one surface that hovered through a modal.
void TestDebugToolbarDoesNotHoverUnderAModal() {
#if !MICROIDE_HAS_SDL3_TTF || !(defined(__unix__) || defined(__APPLE__))
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

// The QA monkey (seed 2): with the panel maximized, Settings open and the filter
// taking the typing, a click on the second row's stepper value selected that row
// and moved the keyboard to the rows -- and no frame repainted it. Settings asked
// for the editor area, which a maximized panel leaves empty, while the card and
// its backdrop sit on the overlay anchor (the panel).
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

// dbg-exception-breakpoints: after a session was stopped, the Breakpoints view
// lost the adapter's exception filters ("No breakpoints") while the ones turned on
// were still sent to every later session -- a filter that could be neither seen
// nor turned off until a session was running again. VS Code keeps the
// checkboxes between sessions; a new session replaces them with its own.
void TestExceptionFiltersStayListedAfterAStop() {
#if !(defined(__unix__) || defined(__APPLE__))
  return;
#endif
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

// dbg-exception-breakpoints: Stop Debugging (Shift+F5) with nothing running closed
// the debug pane the user had opened and announced "Debugging stopped". VS Code's
// Stop does nothing without a session; here it says so and leaves the pane.
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

// The QA monkey (seed 4, CI's ASan lane): a sidebar dragged down to 46 px drew
// its view tabs at their 16 px minimum past the sidebar's edge and under the "⋯"
// button, so a hover lit one control while the damage covered another. Tabs that
// do not fit go into the "⋯" menu, which lists every view.
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

// edit-transforms: saving a new file in a repository repainted the tree's rows but
// not the git badges the following async status refresh gave them. The refresh
// returned early whenever the git view's selected entry survived it, and that
// return repainted the sidebar only when it showed the git view -- so with the
// file tree up, a badge appeared only when something else repainted its row.
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
  WriteFile(root / "a.txt", "a changed\n");  // the git view's selected entry

  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the badge fixture should open");
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  TestAccess::OnFramePresented(shell);
  // A visit to the git view fills its list (a.txt, selected); back on the tree,
  // later refreshes are status-only and keep that selection.
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

// git-branches: "Git: Create Branch…" from the palette (or the Git menu) failed
// with "Usage: git-create-branch <name> [start-point]" -- every command whose
// usage needs an argument did, run bare from a menu, the palette or a shortcut.
// It now asks for the arguments; a command with a bare form keeps it.
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

  // Go to Line has a bare form (its own prompt): it is not asked for arguments.
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_G, SDL_KMOD_CTRL)});
  ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["prompt"].IsObject() && ui["prompt"]["title"].AsString() == "Go to Line",
         "Ctrl+G should still open Go to Line");
}

// compare-clipboard: the palette row "Compare with Clipboard", picked by Enter or
// a click, ran the command line `compare with clipboard` instead -- `compare` is a
// command, so its first word made the query "a verb with arguments" -- and failed
// with "Compare path does not exist: .../with". A query that starts a row's label
// is a search for that row ("open file" -> Open File…).
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

// settings-scopes: a tab size made the user default (the Settings row's scope
// chip) did not reach a project opened for the first time -- its Settings row
// read 5 while its buffers indented by 4. A project with no saved config skipped
// the materializing of the editor preferences, so it kept the built-in defaults
// instead of project override -> user default -> built-in default.
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

// settings-scopes, the other half: with both projects open, a default promoted in
// one did not reach the other on switching to it -- activation rebound the
// settings store but kept the editor preferences it had cached when it was last
// active, so a project's buffers indented by whatever was the default back then.
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

// lsp-more: a hover card left open over the editor stayed live under the command
// palette -- painted over, but still hit-tested first -- so a click on a palette
// row inside the card's rect was swallowed and ran nothing. A list overlay or a
// modal covers the hover: it is neither drawn nor hit while one is open.
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
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);  // own it: the loop borrows
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

// The coverage audit: no Reopen Closed Editor (VS Code's Ctrl+Shift+T). Closing a
// tab now remembers its file and caret; Ctrl+Shift+T reopens the newest closed
// editor where its caret was, then the one before it.
void TestCtrlShiftTReopensClosedEditorsNewestFirst() {
  FlowFixture fixture("first.txt", "one\ntwo\nthree\n");
  const std::filesystem::path second = fixture.root / "second.txt";
  WriteFile(second, "alpha\nbeta\ngamma\n");
  Expect(TestAccess::OpenFileInNewTab(fixture.shell, second), "second.txt should open");
  TestAccess::ActiveEditor(fixture.shell).JumpCursorTo(1, 2);
  // The active tab's file, or empty with none open.
  const auto active_path = [&]() -> std::filesystem::path {
    const auto paths = TestAccess::GroupTabPaths(fixture.shell, 0);
    const std::size_t active = TestAccess::ActiveTabIndex(fixture.shell);
    return active < paths.size() ? paths[active] : std::filesystem::path{};
  };
  const SDL_Keymod ctrl_shift = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});  // second.txt
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});  // first.txt
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

// dbg-stepping and wb-git: a pointer moving onto a debug-pane mode tab, or onto the
// git view's Commit button, lit it up in a full repaint but not on screen. The
// hover probe frame prep and pointer motion share (HoveredInteractiveRect) did not
// know either control, and the git view's buttons had a probe of their own in the
// motion handler that frame prep never ran.
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
    // The buttons carry the labels the render draws (Sync with its counts).
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

// find-options: Ctrl+F with nothing selected opened on the last query, where VS
// Code seeds the word under the caret; a caret in whitespace keeps the last one.
void TestCtrlFSeedsTheWordUnderTheCaret() {
  FlowFixture fixture("seed.txt", "cat catalog  cat\n");
  const auto query = [&]() {
    return TestAccess::ControlQuery(fixture.shell, "ui")["overlay"]["query"].AsString();
  };
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 6);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  Expect(query() == "catalog", "Ctrl+F on catalog should seed it, got '" + query() + "'");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE)});
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 12);  // between the two spaces
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  Expect(query() == "catalog", "a caret in whitespace should keep the last query, got '" + query() + "'");
}

// find-options: Ctrl+H typed into the find widget did nothing (the field took it);
// VS Code shows the replace row and focuses it, keeping the query.
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

// caret-motion: Shift+Alt+Right extended the selection by a character; it is VS
// Code's Expand Selection (word, brackets, ...) and Shift+Alt+Left shrinks it back.
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

// wb-tree: F2 and Delete on a tree entry did nothing; in VS Code's explorer they
// rename and delete it, through the same prompts as the context menu.
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

// Quick Open (Ctrl+P) as VS Code's: `file:line` opens the file at the line, `>`
// switches to the command palette and `:` to Go to Line, keeping what follows.
// QA: all three matched no file and Enter did nothing.
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

// undo-semantics: Ctrl+End to the last line of a file longer than the pane, then
// type -- the typed text was damaged three rows above where it is painted.
// EnsureCursorVisible's left-edge branch returned before clamping the vertical
// scroll, so the bottom margin left it past its maximum until the next paint.
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

// A double-click on the tab strip's empty space opens an untitled editor, as in
// VS Code (it did nothing).
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

// mouse-and-wrap: Shift+wheel over a long line scrolls it sideways (VS Code).
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
  SDL_SetModState(SDL_KMOD_LSHIFT);
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(x, y), MouseWheelEvent(x, y, -3.0f)});
  SDL_SetModState(SDL_KMOD_NONE);
  Expect(TestAccess::ActiveEditor(fixture.shell).horizontal_scroll() > 0,
         "Shift+wheel down should scroll the line to the right, horizontal scroll is " +
             std::to_string(TestAccess::ActiveEditor(fixture.shell).horizontal_scroll()));
}

// search-options: Ctrl+Shift+F with a word selected opened the search view on the
// last query; VS Code seeds it with the selection.
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

// groups-keyboard: Ctrl+K, release Ctrl, Ctrl+Left -- the second Ctrl press
// cancelled the chord, so every Ctrl+K chord worked only with Ctrl held through
// both keys. A modifier key on its own must leave a pending chord armed.
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

// Column selection (Ctrl+Shift+Alt+arrows) steps by visual columns; over multibyte
// text it must still never put a selection edge inside a character, or a find
// query or a copy seeded from it holds half of one. (Probed while chasing a
// monkey run whose find query was one raw byte -- copied, it turned out, from a
// binary file opened as text, which is that file's real content.)
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
        // Appended, not `"..." + to_string(...) + "..."`: GCC 13 reports a false
        // -Warray-bounds on that temporary chain.
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

// debug-keys: Delete in the Breakpoints pane did nothing; VS Code removes the
// selected breakpoint.
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
  // The first row is the section header; Down onto the breakpoint, then Delete.
  for (int i = 0; i < 3 && breakpoint_count() == 1; ++i) {
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DELETE, SDL_KMOD_NONE)});
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN, SDL_KMOD_NONE)});
  }
  Expect(breakpoint_count() == 0, "Delete on the breakpoint row should remove it");
}

// debug-keys: `breakpoint-remove` (Delete in the Breakpoints pane) changed the
// store and the pane's rows but repainted neither the gutter dot nor the pane --
// nor did `breakpoint-set` or any other path that does not also move the caret.
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

// plugins-ui: a plugin cleared its panel surface, the bottom panel went away, and
// the sidebar's newly exposed bottom kept the panel's pixels -- the path asked
// only for the editor. A region that appears, vanishes or moves repaints where
// it was and where it is, whatever changed it.
void TestALayoutChangeWithNoDamageOfItsOwnRepaints() {
  FlowFixture fixture("layout.txt", "x\n");
  TestAccess::ShowOutputChannelWithLines(fixture.shell, "layout", 5);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  workspace::RenderInvalidation full;
  full.full = true;
  fixture.ExpectFresh(full, "the output panel shown");
  Expect(TestAccess::BottomPanelVisible(fixture.shell), "the panel should be up");
  // As the plugin path did: the panel goes, and the only damage asked for is the
  // editor's (here a caret move's).
  TestAccess::HideBottomPanel(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_DOWN, SDL_KMOD_NONE)}),
                      "the panel gone with only the editor damaged");
}

// recents-and-autosave: `editor.autosave = on_focus_change` saved on a tab-strip
// switch and on window blur only. Opening another file (Ctrl+P, the tree, a jump)
// or moving the keyboard to the sidebar or the terminal left the buffer dirty;
// VS Code saves whenever the editor loses focus.
void TestAutosaveOnFocusChangeCoversEveryWayTheEditorLosesFocus() {
  FlowFixture fixture("focus.txt", "start\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "set-setting editor.autosave on_focus_change"),
         "the setting should apply");
  const std::filesystem::path other = fixture.root / "other.txt";
  WriteFile(other, "other\n");
  const auto frame = [&] {
    fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
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

// VS Code's Ctrl+Shift+S: Save As on a file that already has a name. Only an
// untitled buffer could reach the Save As prompt.
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

// plugins-ui: plugin decorations are hidden while the buffer is dirty (they were
// computed for the saved text). The keystroke that made a clean buffer dirty hid
// every annotation in the file but repainted only its own row, so an end-of-line
// note on another line stayed on screen -- and undo back to clean brought them
// back nowhere.
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

// commands-run: every command once, bare. A refusal must say what is wrong, and
// a command must not act on what is not there:
//  - term-relaunch with no terminal said "The terminal is still running";
//  - terminal-find said "No terminal to search" while the panel showed another
//    tab over a live terminal (it now brings the terminal up and opens find);
//  - term-pane-left/right from the command line said "command failed" (the
//    silent decline is for the Alt+Left/Right chord, which then goes to the shell);
//  - find-previous in a diff named find-next;
//  - split-right in a diff said "Failed to split the active editor";
//  - compare-clipboard with no project diffed the welcome page as "Untitled";
//  - with no project, remote-remove-mirror / remote-apply-deletes named an empty
//    path, and tests-discover (there, and with no file open) said "Test
//    discovery failed".
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
  // The project's own terminals first: with none left, there is nothing to relaunch.
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
  expect_said("remote-remove-mirror", "No active project");
  expect_said("remote-apply-deletes", "No active project");
  expect_said("tests-discover", "No active project");
}

// commands-run: a diff's own verbs -- ignore / show whitespace, copy the change
// or the file as a patch, copy the path, the review's next and previous file --
// were implemented on CompareMergeService and reachable from no key, menu or
// command. They are commands now, and each refuses with its reason.
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

  TestAccess::ActiveCompare(fixture.shell).selected_row = 0;  // "alpha": no change there
  clipboard.clear();
  Expect(!run("diff-copy-hunk") && clipboard.empty() && feedback().find("No change") != std::string::npos,
         "diff-copy-hunk on an unchanged row should refuse, said \"" + feedback() + "\"");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RIGHTBRACKET, SDL_KMOD_ALT)});
  Expect(run("diff-copy-hunk") && clipboard.find("@@") != std::string::npos,
         "diff-copy-hunk on a change should copy it, copied [" + clipboard + "], said \"" + feedback() + "\"");

  Expect(!run("diff-next-file") && feedback().find("not part of a review") != std::string::npos,
         "diff-next-file outside a review should say so, said \"" + feedback() + "\"");
}

// The merge editor's side panes are read-only views with no selection of their
// own; copying a conflict's side was implemented (CopyMergeSideSnippet) and
// reachable from nothing. merge-copy-incoming / merge-copy-current copy the
// selected conflict's side.
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

  Expect(!run("merge-copy-incoming") && feedback().find("Not a merge") != std::string::npos,
         "outside a merge it refuses, said \"" + feedback() + "\"");
  Expect(run("merge " + fixture.file.string() + " " + incoming.string() + " " + current.string()),
         "merge should open the merge editor, said \"" + feedback() + "\"");
  Expect(run("merge-copy-incoming") && clipboard.find("two from incoming") != std::string::npos &&
             clipboard.find("current") == std::string::npos,
         "merge-copy-incoming should copy the incoming side, copied [" + clipboard + "]");
  Expect(run("merge-copy-current") && clipboard.find("two from current") != std::string::npos,
         "merge-copy-current should copy the current side, copied [" + clipboard + "]");
}

// plugins-ui: `colorscheme list` refreshed the scheme names and reported nothing.
void TestColorschemeListSaysWhichSchemesThereAre() {
  FlowFixture fixture("theme.txt", "x\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "colorscheme list"), "colorscheme list should run");
  const std::string feedback = TestAccess::CommandFeedbackText(fixture.shell);
  // The two built-in schemes, on any machine (CI's list is exactly these; "dark"
  // comes from whatever else a developer has installed).
  Expect(feedback.find("default") != std::string::npos && feedback.find("light") != std::string::npos,
         "colorscheme list should name the schemes, said '" + feedback + "'");
}

// compare-keys: typing " edited" on a diff's working side inserted " e" and then
// the `d` opened "Discard Patch" -- the row-review letters (j/k/a/c/d/o) fired
// while the editable side had the caret. Being typed into, they are text.
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
  // A working-tree diff says so from the start (it read "Branch review" until the
  // first edit re-derived it).
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
  // Ctrl+S writes the working side, as Ctrl+S in any editor does.
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_S, SDL_KMOD_CTRL)});
  for (int i = 0; i < 100 && ReadFile(file).find("adjkoc") == std::string::npos; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)DispatchEvents(shell, {});
  }
  Expect(ReadFile(file).find("beta changedadjkoc") != std::string::npos,
         "Ctrl+S in the diff should write the working side, disk has '" + ReadFile(file) + "'");
}

// compare-keys: stepping between a diff's changes left the previous change's
// selection markers on screen -- they sit at the surface's outer edges, and the
// rows' damage started at the left pane's text.
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

// compare-keys: the diff's header counts its hunks, and an edit that adds one
// changed the header without repainting it.
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
  compare.right_viewport.MoveCursorTo(19, 7);  // "line 20", far from the first hunk
  shell.Render(canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::string before = compare.review_header.summary_line;
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, FlowFixture::kWidth, FlowFixture::kHeight,
                                                DispatchEvents(shell, {TextInputEvent("X")}), PinCaretOf(shell));
  Expect(TestAccess::ActiveCompare(shell).review_header.summary_line != before,
         "the edit should add a hunk to the header");
  Expect(diff.identical(), "the header should repaint with its new hunk count; " + DescribePixelDiff(diff));
}

// TD-2026-10-16-353: Alt+Left / Alt+Right are VS Code's Go Back / Go Forward. A
// far jump in a file and a switch to another file are places; going back to a
// file closed since reopens it with the caret where it was.
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

  // The file ahead was closed: going forward reopens it, caret where it was.
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

  // A new place from the middle of the history drops what was ahead of it.
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

// TD-2026-10-16-353 (VS Code shortcuts with no binding): F1 opens the command
// palette; Ctrl+1..8 focus editor group N, and the group one past the last is
// made by splitting right (Ctrl+2 from a single editor opens the side one).
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

// An output tab opened before its channel's first line (Find References opens it
// at once; the answer comes later) repaints when the line brings the channel's
// label: the strip showed the id, "lsp.referenc...", until something unrelated
// repainted it (QA plugin-api).
void TestOutputTabRepaintsWhenItsChannelGetsALabel() {
  FlowFixture fixture("notes.txt", "notes\n");
  TestAccess::SetOutputChannelTabs(fixture.shell, {"qa.late"}, "qa.late");
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  TestAccess::AppendOutputLine(fixture.shell, "qa.late", "Late Results", "a result");
  // The next frame -- any: a caret move here -- paints what the line changed.
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)}),
                      "the frame after the channel's first line");
}

// Typing that moves the caret off a highlighted word repaints every occurrence it
// un-highlights: the occurrence highlight follows the word at the caret, and a
// keystroke repainted only the line it changed, so the highlight on the word's
// other lines stayed (QA plugin-api, accepting a completion after `greet`).
void TestTypingOffAWordRepaintsItsOccurrences() {
  FlowFixture fixture("words.txt", "greet one\nfiller\ngreet\n");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(2, 5);  // the end of the last `greet`
  fixture.shell.Render(fixture.canvas.renderer(), FlowFixture::kWidth, FlowFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  fixture.ExpectFresh(DispatchEvents(fixture.shell, {TextInputEvent(" ")}), "typing a space after the word");
}

// A hover over a function body scrolled under its sticky header asks about the
// line under the pointer. Hover resolved points against a grid with no sticky band,
// so it asked about the line the band's height further down -- and showed that
// line's symbol -- while a click on the same spot landed right.
void TestHoverUnderAStickyBandResolvesThePointersLine() {
  std::string body = "int outer() {\n";
  for (int i = 0; i < 80; ++i) {
    body += "  int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  body += "}\n";
  FlowFixture fixture("sticky.cpp", body);
  // Scroll into the body, so `int outer() {` pins above it.
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

// Hovering a merge toolbar button lights it, and the frame shows it: the buttons
// read the pointer at paint time, but merge hover tracking knew only conflict
// regions, so moving onto Save requested no repaint of it and the next partial
// frame (a caret blink) kept the unlit button (QA merge-mouse, both lanes).
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
    // Off every button, onto the result pane's header band below the toolbar.
    fixture.ExpectFresh(DispatchEvents(fixture.shell, {MouseMotionEvent(x, y + 120.0f)}),
                        "leaving " + std::string(name));
  }
}

}  // namespace

void RegisterQaFlowRegressionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegression/OutputTabRepaintsWhenItsChannelGetsALabel",
          TestOutputTabRepaintsWhenItsChannelGetsALabel);
  AddTest(tests, "QaFlowRegression/TypingOffAWordRepaintsItsOccurrences",
          TestTypingOffAWordRepaintsItsOccurrences);
  AddTest(tests, "QaFlowRegression/HoverUnderAStickyBandResolvesThePointersLine",
          TestHoverUnderAStickyBandResolvesThePointersLine);
  AddTest(tests, "QaFlowRegression/MergeToolbarHoverRepaintsTheButton",
          TestMergeToolbarHoverRepaintsTheButton);
  AddTest(tests, "QaFlowRegression/FindWidgetReplaceRepaintsTheDirtyTab",
          TestFindWidgetReplaceRepaintsTheDirtyTab);
  AddTest(tests, "QaFlowRegression/BannerReloadRepaintsTheCleanTab", TestBannerReloadRepaintsTheCleanTab);
  AddTest(tests, "QaFlowRegression/TabDragDropPreviewPaints", TestTabDragDropPreviewPaints);
  AddTest(tests, "QaFlowRegression/ClosingAPromptOverTheFinderGivesTheFinderTheKeyboard",
          TestClosingAPromptOverTheFinderGivesTheFinderTheKeyboard);
  AddTest(tests, "QaFlowRegression/ToastsShowWithTheStatusBarHidden", TestToastsShowWithTheStatusBarHidden);
  AddTest(tests, "QaFlowRegression/OpeningAModalUnderThePointerDropsTheHoverItTakes",
          TestOpeningAModalUnderThePointerDropsTheHoverItTakes);
  AddTest(tests, "QaFlowRegression/DraggingTheSidebarShutKeepsEveryFrameFresh",
          TestDraggingTheSidebarShutKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/EveryDividerDraggedToItsExtremesKeepsEveryFrameFresh",
          TestEveryDividerDraggedToItsExtremesKeepsEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/TogglingTheMergeBasePaneKeepsTheToolbarAndEveryFrameFresh",
          TestTogglingTheMergeBasePaneKeepsTheToolbarAndEveryFrameFresh);
  AddTest(tests, "QaFlowRegression/AConflictOutOfSightHasNoButtons", TestAConflictOutOfSightHasNoButtons);
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
  AddTest(tests, "QaFlowRegression/ModalsOwnThePointer", TestModalsOwnThePointer);
  AddTest(tests, "QaFlowRegression/HoverTextKeepsItsLines", TestHoverTextKeepsItsLines);
  AddTest(tests, "QaFlowRegression/DebugSessionRepaintsToolbarAndOutput", TestDebugSessionRepaintsToolbarAndOutput);
  AddTest(tests, "QaFlowRegression/DebugToolbarDoesNotHoverUnderAModal", TestDebugToolbarDoesNotHoverUnderAModal);
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
  AddTest(tests, "QaFlowRegression/TreeGitBadgesRepaintWhenTheGitSelectionSurvives",
          TestTreeGitBadgesRepaintWhenTheGitSelectionSurvives);
  AddTest(tests, "QaFlowRegression/MarkingAnOutgoingFileReviewedMarksItsRow",
          TestMarkingAnOutgoingFileReviewedMarksItsRow);
  AddTest(tests, "QaFlowRegression/CtrlQQuits", TestCtrlQQuits);
  AddTest(tests, "QaFlowRegression/ClosingTheLastTerminalUnderAnOutputChannelRepaintsTheTabStrip",
          TestClosingTheLastTerminalUnderAnOutputChannelRepaintsTheTabStrip);
  AddTest(tests, "QaFlowRegression/TypingOnAFoldedOpenerRepaintsItsRow",
          TestTypingOnAFoldedOpenerRepaintsItsRow);
  AddTest(tests, "QaFlowRegression/CommandRefusalsSayWhatIsWrong", TestCommandRefusalsSayWhatIsWrong);
  AddTest(tests, "QaFlowRegression/TheDiffsOwnVerbsAreCommands", TestTheDiffsOwnVerbsAreCommands);
  AddTest(tests, "QaFlowRegression/TheMergeEditorCopiesAConflictSide", TestTheMergeEditorCopiesAConflictSide);
  AddTest(tests, "QaFlowRegression/ATerminalTitleRepaintsItsTab", TestATerminalTitleRepaintsItsTab);
  AddTest(tests, "QaFlowRegression/ATerminalPaneCursorChangeRepaintsItsCaret",
          TestATerminalPaneCursorChangeRepaintsItsCaret);
  AddTest(tests, "QaFlowRegression/ASidebarScrollOfOneRowRepaints", TestASidebarScrollOfOneRowRepaints);
  AddTest(tests, "QaFlowRegression/SaveAsRevealsTheNewFileInTheTree", TestSaveAsRevealsTheNewFileInTheTree);
  AddTest(tests, "QaFlowRegression/TheGitViewReportsTheSelectedEntrysRow", TestTheGitViewReportsTheSelectedEntrysRow);
  AddTest(tests, "QaFlowRegression/ReloadFromDiskRefusesADirtyBufferWithWhy", TestReloadFromDiskRefusesADirtyBufferWithWhy);
  AddTest(tests, "QaFlowRegression/NothingBeneathAnOpenMenuHovers", TestNothingBeneathAnOpenMenuHovers);
  AddTest(tests, "QaFlowRegression/TypingThatReshapesADiffRepaintsIt", TestTypingThatReshapesADiffRepaintsIt);
}

}  // namespace microide::tests
