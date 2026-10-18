// Regressions the GUI QA flows (tools/qa/flows_*.py) found, pinned in process.
//
// Each test reproduces one finding the way the harness saw it: real events
// through HandleEvent, the redraw folded as the app loop folds it, painted over
// the retained canvas and compared with a full repaint of the same state
// (tests/support/RetainedRedraw.h) -- or the behavioural expectation the flow
// stated, checked against the same queries the harness reads.
#include "TestSupport.h"
#include "TestSupportSdl.h"

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
#include "util/JsonValue.h"
#include "workspace/HoverTextWrap.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

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
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw);
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
  const auto redraw =
      DispatchEvents(shell, {MouseMotionEvent(commit->x + commit->w / 2.0f, commit->y + commit->h / 2.0f)});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, FlowFixture::kWidth, FlowFixture::kHeight, redraw);
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

}  // namespace

void RegisterQaFlowRegressionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegression/FindWidgetReplaceRepaintsTheDirtyTab",
          TestFindWidgetReplaceRepaintsTheDirtyTab);
  AddTest(tests, "QaFlowRegression/BannerReloadRepaintsTheCleanTab", TestBannerReloadRepaintsTheCleanTab);
  AddTest(tests, "QaFlowRegression/TabDragDropPreviewPaints", TestTabDragDropPreviewPaints);
  AddTest(tests, "QaFlowRegression/ModalsOwnThePointer", TestModalsOwnThePointer);
  AddTest(tests, "QaFlowRegression/HoverTextKeepsItsLines", TestHoverTextKeepsItsLines);
  AddTest(tests, "QaFlowRegression/DebugSessionRepaintsToolbarAndOutput", TestDebugSessionRepaintsToolbarAndOutput);
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
  AddTest(tests, "QaFlowRegression/TypingOnAFoldedOpenerRepaintsItsRow",
          TestTypingOnAFoldedOpenerRepaintsItsRow);
}

}  // namespace microide::tests
