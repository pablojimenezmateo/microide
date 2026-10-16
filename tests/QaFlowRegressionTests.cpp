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
#include <thread>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "WorkspaceShellEventHelpers.h"
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

}  // namespace

void RegisterQaFlowRegressionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegression/FindWidgetReplaceRepaintsTheDirtyTab",
          TestFindWidgetReplaceRepaintsTheDirtyTab);
  AddTest(tests, "QaFlowRegression/BannerReloadRepaintsTheCleanTab", TestBannerReloadRepaintsTheCleanTab);
  AddTest(tests, "QaFlowRegression/TabDragDropPreviewPaints", TestTabDragDropPreviewPaints);
  AddTest(tests, "QaFlowRegression/ModalsOwnThePointer", TestModalsOwnThePointer);
  AddTest(tests, "QaFlowRegression/HoverTextKeepsItsLines", TestHoverTextKeepsItsLines);
  AddTest(tests, "QaFlowRegression/TallMenusStayInsideTheWindow", TestTallMenusStayInsideTheWindow);
  AddTest(tests, "QaFlowRegression/TypingOnAFoldedOpenerRepaintsItsRow",
          TestTypingOnAFoldedOpenerRepaintsItsRow);
}

}  // namespace microide::tests
