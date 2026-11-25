#include "TestSupport.h"
#include "TestSupportSdl.h"

#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "WorkspaceShellEventHelpers.h"
#include "support/GitSidebarWait.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "plugin/PluginHost.h"
#include "util/JsonValue.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

std::string ViewportText(const editor::TextViewport& viewport) {
  std::string text;
  for (std::size_t i = 0; i < viewport.lines().LineCount(); ++i) {
    if (i > 0) {
      text += '\n';
    }
    text += viewport.lines().LineView(i);
  }
  return text;
}

struct UxFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;

  UxFixture(std::string_view file_name, std::string_view content) {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    file = root / std::string(file_name);
    WriteFile(file, std::string(content));
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the ux fixture should open its project");
    TestAccess::SetWindowSize(shell, kWidth, kHeight);
    TestAccess::OpenSingleEditorTab(shell, file);
    TestAccess::ShowTreeSidebar(shell);
    TestAccess::SetFocusEditor(shell);
    TestAccess::NoteLayoutInputsChanged(shell);
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  SDL_FPoint LineNumberPoint(int line) {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    const util::JsonValue& pane = ui["editors"].AsArray()[0];
    const float x = static_cast<float>(pane["gutterX"].AsDouble() + pane["gutterWidth"].AsDouble() * 0.4);
    for (const util::JsonValue& row : pane["rows"].AsArray()) {
      if (row["line"].AsInt() == line) {
        return SDL_FPoint{x, static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() / 2.0)};
      }
    }
    Expect(false, "line " + std::to_string(line) + " should be on screen");
    return SDL_FPoint{x, 0.0f};
  }

  SDL_FPoint TextPoint(int line, int column) {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    const util::JsonValue& pane = ui["editors"].AsArray()[0];
    const double char_width = pane["charWidth"].AsDouble();
    const float x = static_cast<float>(pane["textX"].AsDouble() + (column - 1 + 0.2) * char_width);
    for (const util::JsonValue& row : pane["rows"].AsArray()) {
      if (row["line"].AsInt() == line) {
        return SDL_FPoint{x, static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() / 2.0)};
      }
    }
    Expect(false, "line " + std::to_string(line) + " should be on screen");
    return SDL_FPoint{x, 0.0f};
  }

  void Click(SDL_FPoint point, Uint8 button = SDL_BUTTON_LEFT, Uint8 clicks = 1) {
    SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, point.x, point.y);
    SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, point.x, point.y);
    down.button.button = up.button.button = button;
    down.button.clicks = up.button.clicks = clicks;
    (void)DispatchEvents(shell, {MouseMotionEvent(point.x, point.y), down, up});
  }

  std::string ElementLabel(std::string_view name) {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == name) {
        return entry["label"].AsString();
      }
    }
    return {};
  }

  void Frame() { shell.Render(canvas.renderer(), kWidth, kHeight); (void)shell.ConsumePendingRenderInvalidation(); }

  std::string Selection() {
    const editor::TextViewport& viewport = TestAccess::ActiveEditor(shell);
    const auto range = viewport.selection_range();
    std::string anchor = range.has_value() ? std::to_string(range->start.line) + ":" +
                                                 std::to_string(range->start.column)
                                           : std::string("none");
    return anchor + " -> " + std::to_string(viewport.cursor_line()) + ":" +
           std::to_string(viewport.cursor_column());
  }

  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  std::filesystem::path file;
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
};

void TestALineNumberPressSelectsWholeLines() {
  UxFixture fixture("lines.txt", "one\ntwo\nthree\nfour\nfive\n");
  const SDL_FPoint two = fixture.LineNumberPoint(2);
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, two.x, two.y)});
  Expect(fixture.Selection() == "1:0 -> 2:0",
         "a click on line 2's number should select line 2 whole, got " + fixture.Selection());

  const SDL_FPoint four = fixture.LineNumberPoint(4);
  SDL_Event held = MouseMotionEvent(four.x, four.y);
  held.motion.state = SDL_BUTTON_LMASK;
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, two.x, two.y),
                                       held,
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, four.x, four.y)});
  Expect(fixture.Selection() == "1:0 -> 4:0",
         "a drag down the numbers from 2 to 4 should select lines 2-4 whole, got " + fixture.Selection());
}


void TestAltEnterInTheFindWidgetSelectsEveryMatch() {
  UxFixture fixture("find.txt", "foo bar Foo\nbaz foo\nfoo\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL), TextInputEvent("foo")});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_C, SDL_KMOD_ALT)});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN, SDL_KMOD_ALT)});
  const editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(viewport.secondary_caret_range_view().size() + 1 == 3,
         "Alt+Enter puts a caret on each of the three case-matching matches, got " +
             std::to_string(viewport.secondary_caret_range_view().size() + 1));
  (void)DispatchEvents(fixture.shell, {TextInputEvent("X")});
  Expect(ViewportText(viewport) == "X bar Foo\nbaz X\nX\n",
         "typing goes to the editor and replaces every match, got " + ViewportText(viewport));
}

void TestARightClickInsideASelectionKeepsIt() {
  UxFixture fixture("words.txt", "alpha beta gamma\nsecond line\n");
  fixture.Click(fixture.TextPoint(1, 8), SDL_BUTTON_LEFT, 2);
  Expect(fixture.Selection() == "0:6 -> 0:10", "the double-click selects beta, got " + fixture.Selection());
  fixture.Click(fixture.TextPoint(1, 9), SDL_BUTTON_RIGHT);
  Expect(fixture.Selection() == "0:6 -> 0:10",
         "a right-click inside the selection keeps it, got " + fixture.Selection());
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
  fixture.Click(fixture.TextPoint(2, 4), SDL_BUTTON_RIGHT);
  Expect(fixture.Selection() == "none -> 1:3",
         "a right-click outside it moves the caret there, got " + fixture.Selection());
}

void TestARightClickBelowAGhostSuggestionLandsOnTheClickedLine() {
  UxFixture fixture("ghost.txt", "one\ntwo\nthree\nfour\nfive\n");
  Expect(TestAccess::SetSettingValueTransient(fixture.shell, "plugins.ghost_text", "true"),
         "plugins.ghost_text is settable");
  TestAccess::ActiveEditor(fixture.shell).MoveCursorTo(0, 3);
  plugin::PluginHost::GhostTextRequest request;
  request.text = "X\nY\nZ";
  TestAccess::PublishGhostText(fixture.shell, "test", request);
  fixture.Frame();
  Expect(TestAccess::GhostText(fixture.shell) != nullptr, "precondition: the suggestion is live");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const util::JsonValue& pane = ui["editors"].AsArray()[0];
  const double line_height = pane["lineHeight"].AsDouble();
  const SDL_FPoint line4{
      static_cast<float>(pane["textX"].AsDouble() + 1.2 * pane["charWidth"].AsDouble()),
      static_cast<float>(pane["firstLineY"].AsDouble() + (3 + 2 + 0.5) * line_height)};
  fixture.Click(line4, SDL_BUTTON_RIGHT);
  Expect(fixture.Selection() == "none -> 3:1",
         "a right-click on line 4 below the suggestion puts the caret there, got " +
             fixture.Selection());
}

void TestTheFindWidgetFollowsATabSwitch() {
  UxFixture fixture("one.txt", "xx foo\nyy foo\nzz foo\n");
  WriteFile(fixture.root / "two.txt", "a\nb\nc\nd\nfoo here\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL), TextInputEvent("foo")});
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open " + (fixture.root / "two.txt").string()),
         "two.txt opens");
  fixture.shell.Render(fixture.canvas.renderer(), UxFixture::kWidth, UxFixture::kHeight);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["overlay"]["visible"].AsBool() && ui["overlay"]["count"].AsInt() == 1,
         "the widget counts two.txt's one match, got " + std::to_string(ui["overlay"]["count"].AsInt()));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F3)});
  Expect(fixture.Selection() == "4:0 -> 4:3", "F3 selects foo in two.txt, got " + fixture.Selection());
}

void TestCtrlKSSavesEveryDirtyFile() {
  UxFixture fixture("one.txt", "one\n");
  WriteFile(fixture.root / "two.txt", "two\n");
  auto type_into_active = [&](const char* text) {
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_CTRL), TextInputEvent(text)});
  };
  type_into_active("ONE");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "split-right"), "the editor splits");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open " + (fixture.root / "two.txt").string()),
         "two.txt opens in the new group");
  type_into_active("TWO");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "tab"), "an untitled buffer opens");
  type_into_active("scratch");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_K, SDL_KMOD_CTRL), KeyDownEvent(SDLK_S),
                                       TextInputEvent("s")});
  Expect(ReadFile(fixture.root / "one.txt") == "one\nONE\n" && ReadFile(fixture.root / "two.txt") == "two\nTWO\n",
         "Ctrl+K S writes both files, got " + ReadFile(fixture.root / "one.txt") + " / " +
             ReadFile(fixture.root / "two.txt"));
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "scratch",
         "the chord's S is not typed, got " + ViewportText(TestAccess::ActiveEditor(fixture.shell)));
  bool named = false;
  for (const auto& notification : TestAccess::ActiveNotifications(fixture.shell)) {
    named = named || notification.message.find("1 untitled buffer needs a name") != std::string::npos;
  }
  Expect(named, "the notice says the untitled buffer was left for Save As");
}

void TestTheUnsavedPromptsLettersDoNotType() {
  UxFixture fixture("clean.txt", "clean\n");
  const auto prompt_close_untitled = [&](SDL_Keycode key, const char* text) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, "tab"), "an untitled buffer opens");
    (void)DispatchEvents(fixture.shell, {TextInputEvent("scratch")});
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(key), TextInputEvent(text)});
  };
  prompt_close_untitled(SDLK_C, "c");
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "scratch",
         "Cancel keeps the buffer as it was, got " + ViewportText(TestAccess::ActiveEditor(fixture.shell)));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_D), TextInputEvent("d")});
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "clean\n",
         "Don't Save leaves the file that comes to the front untouched, got " +
             ViewportText(TestAccess::ActiveEditor(fixture.shell)));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_X), TextInputEvent("x")});
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "xclean\n",
         "the following key types as usual, got " + ViewportText(TestAccess::ActiveEditor(fixture.shell)));
}

void TestAKeyReadsItsOwnModifiersNotTheNextChords() {
  UxFixture fixture("pairs.c", "a\nb\nc\n");
  {
    const ScopedSdlModState next_chord(static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LALT));
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)});
  }
  const auto& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "End goes to the end of the line, got " + std::to_string(viewport.cursor_line()) + ":" +
             std::to_string(viewport.cursor_column()));

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "tab"), "an untitled buffer opens");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("scratch")});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  {
    const ScopedSdlModState next_chord(SDL_KMOD_LSHIFT);
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_D), TextInputEvent("d")});
  }
  Expect(TestAccess::ActiveEditor(fixture.shell).path() == fixture.file,
         "d answers Don't Save and the file comes back to the front");
}

void TestRevertFileDiscardsChangesUndoably() {
  UxFixture fixture("notes.txt", "one\ntwo\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_CTRL), TextInputEvent("three")});
  WriteFile(fixture.root / "notes.txt", "one\ntwo changed elsewhere\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "revert-file"), "revert-file runs on a dirty file");
  const editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(ViewportText(viewport) == "one\ntwo changed elsewhere\n" && !viewport.dirty(),
         "the buffer holds the disk's text, clean, got " + ViewportText(viewport));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)});
  Expect(ViewportText(viewport) == "one\ntwo\nthree" && viewport.dirty(),
         "Ctrl+Z brings the discarded edit back, dirty, got " + ViewportText(viewport));
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Y, SDL_KMOD_CTRL)});
  Expect(ViewportText(viewport) == "one\ntwo changed elsewhere\n" && !viewport.dirty(),
         "redo returns to the reverted, clean text, got " + ViewportText(viewport));
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "revert-file"),
         "with nothing unsaved revert-file refuses");
}

void TestTheStatusColumnFollowsTheTabSize() {
  UxFixture fixture("tabs.txt", "\tx\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END)});
  fixture.Frame();
  const std::string before = fixture.ElementLabel("status:line-column");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "tab-size 8"), "tab-size 8 runs");
  fixture.Frame();
  const std::string after = fixture.ElementLabel("status:line-column");
  const editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  Expect(after == "Ln 1, Col 10", "with tabs of 8 the caret after \\tx is at Col 10, got " + after +
                                      " (was " + before + "); tab size " + std::to_string(viewport.tab_size()) +
                                      ", visual column " + std::to_string(viewport.cursor_visual_column()) +
                                      ", column " + std::to_string(viewport.cursor_column()));
}

void TestTheFindWidgetRecountsAfterAReload() {
  UxFixture fixture("reload.txt", "foo foo foo\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL), TextInputEvent("foo")});
  fixture.Frame();
  Expect(TestAccess::ControlUi(fixture.shell)["overlay"]["count"].AsInt() == 3, "three matches at first");
  WriteFile(fixture.file, "bar bar\n");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "reopen"), "the clean buffer reloads");
  fixture.Frame();
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "bar bar\n", "the buffer is the new file");
  Expect(ui["overlay"]["count"].AsInt() == 0,
         "the widget counts no foo in the reloaded text, got " + std::to_string(ui["overlay"]["count"].AsInt()));
}

void TestSaveAsRetitlesTheTab() {
  UxFixture fixture("tosave.txt", "x\n");
  fixture.Frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "save " + (fixture.root / "copy-of.txt").string()),
         "save under a new name runs");
  fixture.Frame();
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  std::string labels;
  bool renamed = false;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["kind"].AsString() == "tab") {
      labels += entry["label"].AsString() + " ";
      renamed = renamed || entry["label"].AsString() == "copy-of.txt";
    }
  }
  Expect(renamed, "the tab is titled copy-of.txt, got " + labels);
}

void TestAClickOnAGitEntryKeepsTheKeyboardOnTheList() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "keys" / "alpha.txt", "alpha\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(root / "keys" / "alpha.txt", "alpha changed\n");
  WorkspaceShell shell;
  Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  TestAccess::SetWindowSize(shell, 1280, 720);
  TestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1), "the git view should list the change");
  std::optional<SDL_FRect> row;
  const util::JsonValue before = TestAccess::ControlUi(shell);
  for (const util::JsonValue& entry : before["elements"].AsArray()) {
    if (entry["name"].AsString() == "row:git/alpha.txt") {
      const auto& r = entry["rect"].AsArray();
      row = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                      static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
    }
  }
  Expect(row.has_value(), "the git view shows alpha.txt");
  if (!row) {
    return;
  }
  const float x = row->x + row->w / 2.0f;
  const float y = row->y + row->h / 2.0f;
  (void)DispatchEvents(shell, {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  const util::JsonValue ui = TestAccess::ControlUi(shell);
  Expect(ui["focus"].AsString() == "sidebar",
         "the click keeps the keyboard on the git view, got " + ui["focus"].AsString());
}

void TestASelectionInsideAStringIsSurrounded() {
  UxFixture fixture("strings.c", "char* s = \"hello world\";\nx\"\ny\"\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(0, 11);
  viewport.MoveCursorTo(0, 16, true);
  (void)DispatchEvents(fixture.shell, {TextInputEvent("(")});
  Expect(viewport.lines()[0] == "char* s = \"(hello) world\";",
         "the word inside the string is surrounded, got " + std::string(viewport.lines()[0]));
  viewport.MoveCursorTo(2, 0);
  viewport.MoveCursorTo(2, 2, true);
  (void)DispatchEvents(fixture.shell, {TextInputEvent("[")});
  Expect(viewport.lines()[2] == "[y\"]", "the line after a lone quote is surrounded, got " +
                                            std::string(viewport.lines()[2]));
  UxFixture three("three.c", "x\"\ny\"\nz\"\n");
  editor::TextViewport& view = TestAccess::ActiveEditor(three.shell);
  view.MoveCursorTo(0, 0);
  view.MoveCursorTo(0, 2, true);
  view.AddSecondaryCaretWithRange(editor::SelectionRange{{1, 0}, {1, 2}});
  view.AddSecondaryCaretWithRange(editor::SelectionRange{{2, 0}, {2, 2}});
  (void)DispatchEvents(three.shell, {TextInputEvent("(")});
  Expect(ViewportText(view) == "(x\")\n(y\")\n(z\")\n",
         "every caret's selection is surrounded, got " + ViewportText(view));
}

void TestEnterContinuesADocComment() {
  UxFixture fixture("doc.c", "/**\n * doc\n */\n/* plain\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(1, 6);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN), TextInputEvent("more")});
  viewport.MoveCursorTo(0, 3);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN), TextInputEvent("first")});
  viewport.MoveCursorTo(5, 8);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_RETURN), TextInputEvent("x")});
  Expect(ViewportText(viewport) == "/**\n * first\n * doc\n * more\n */\n/* plain\nx\n",
         "Enter continues a doc comment and leaves a plain one, got " + ViewportText(viewport));
}

void TestTheWindowTitleNamesTheActiveFileAndItsState() {
  UxFixture fixture("title.txt", "abc\n");
  const std::string project = fixture.root.filename().string();
  auto title = [&] {
    fixture.Frame();
    return TestAccess::ControlUi(fixture.shell)["windowTitle"].AsString();
  };
  Expect(title() == "title.txt \u2014 " + project + " \u2014 microide",
         "the title names the file and the project, got " + title());
  (void)DispatchEvents(fixture.shell, {TextInputEvent("x")});
  Expect(title() == "\u25CF title.txt \u2014 " + project + " \u2014 microide",
         "an edit marks it dirty, got " + title());
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_Z, SDL_KMOD_CTRL)});
  Expect(title() == "title.txt \u2014 " + project + " \u2014 microide",
         "undo back to the saved text clears the mark, got " + title());
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  Expect(title() == project + " \u2014 microide", "with no editor the title is the project's, got " + title());
}

void TestAnEditorsViewSurvivesCloseReopenAndRestart() {
  std::string text = "void folded() {\n";
  for (int i = 0; i < 20; ++i) {
    text += "  body();\n";
  }
  text += "}\n";
  for (int i = 0; i < 300; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  UxFixture fixture("view.c", text);
  WriteFile(fixture.root / "other.txt", "other\n");
  auto view = [](WorkspaceShell& shell) -> editor::TextViewport& { return TestAccess::ActiveEditor(shell); };
  auto folded_at_zero = [](WorkspaceShell& shell) {
    const auto& tab = TestAccess::OpenTabs(shell)[TestAccess::ActiveTabIndex(shell)];
    return tab.editor_state.has_value() && tab.editor_state->folding_model->IsCollapsedAtOpener(0);
  };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "fold"), "the function folds");
  fixture.Frame();
  Expect(folded_at_zero(fixture.shell), "the caret's function is collapsed");
  view(fixture.shell).MoveCursorTo(200, 3);
  view(fixture.shell).SetScrollLine(150);
  fixture.Frame();
  const std::size_t scroll = view(fixture.shell).scroll_line();

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_T, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)});
  fixture.Frame();
  Expect(view(fixture.shell).cursor_line() == 200 && view(fixture.shell).cursor_column() == 3,
         "Reopen Closed Editor puts the caret back");
  Expect(folded_at_zero(fixture.shell), "and the fold");
  Expect(view(fixture.shell).scroll_line() == scroll,
         "and the scroll it had, got " + std::to_string(view(fixture.shell).scroll_line()) + " for " +
             std::to_string(scroll));

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open " + (fixture.root / "other.txt").string()),
         "other.txt opens");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  TestAccess::SaveSessionState(fixture.shell);

  WorkspaceShell restored;
  Expect(TestAccess::OpenProjectTab(restored, fixture.root, true), "the project reopens with its session");
  TestAccess::SetWindowSize(restored, UxFixture::kWidth, UxFixture::kHeight);
  restored.Render(fixture.canvas.renderer(), UxFixture::kWidth, UxFixture::kHeight);
  restored.Render(fixture.canvas.renderer(), UxFixture::kWidth, UxFixture::kHeight);
  Expect(view(restored).path().filename() == "view.c", "the restart reopens view.c");
  Expect(view(restored).cursor_line() == 200, "with its caret");
  Expect(folded_at_zero(restored), "its fold");
  Expect(view(restored).scroll_line() == scroll,
         "and its scroll, got " + std::to_string(view(restored).scroll_line()) + " for " + std::to_string(scroll));
  (void)DispatchEvents(restored, {KeyDownEvent(SDLK_T, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)});
  Expect(view(restored).path().filename() == "other.txt",
         "Reopen Closed Editor reaches the editor closed before the restart");

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "goto 6:3"), "the caret goes into the body");
  fixture.Frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "fold"), "the function folds around it");
  fixture.Frame();
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_W, SDL_KMOD_CTRL)});
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_T, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)});
  fixture.Frame();
  fixture.Frame();
  Expect(view(fixture.shell).cursor_line() == 5 && folded_at_zero(fixture.shell),
         "the fold around the caret comes back collapsed");
}

void TestSwitchingBackToATabKeepsItsScroll() {
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "  line " + std::to_string(i) + "\n";
  }
  UxFixture fixture("two-space.txt", text);
  WriteFile(fixture.root / "other.txt", "other\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(250, 2);
  viewport.SetScrollLine(40);
  fixture.Frame();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "open " + (fixture.root / "other.txt").string()),
         "other.txt opens");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_PAGEUP, SDL_KMOD_CTRL)});
  fixture.Frame();
  const editor::TextViewport& back = TestAccess::ActiveEditor(fixture.shell);
  Expect(back.path().filename() == "two-space.txt" && back.scroll_line() == 40,
         "the view is where it was left, got scroll " + std::to_string(back.scroll_line()));
}

void TestTheQuickOpenCaretSitsInItsField() {
  UxFixture fixture("caret.txt", "x\n");
  const auto check = [&](const std::string& what, workspace::OverlayMode mode) {
    fixture.Frame();
    const SDL_FRect overlay = TestAccess::ActiveOverlayRect(fixture.shell);
    const float field_top = overlay.y + workspace::OverlayQueryRowOffset(mode) - 4.0f;
    const float field_bottom = field_top + 18.0f;
    const std::optional<SDL_FRect> caret = TestAccess::CaretDirtyRect(fixture.shell);
    Expect(caret.has_value(), what + ": a caret is drawn");
    if (!caret.has_value()) {
      return;
    }
    const float text_y = field_top + std::floor((18.0f - caret->h) * 0.5f);
    Expect(std::fabs(caret->y - (text_y - 1.0f)) < 0.5f && caret->y >= field_top - 1.0f &&
               caret->y + caret->h <= field_bottom + 2.0f,
           what + ": the caret is on its field's text row (caret y " + std::to_string(caret->y) +
               ", the row at " + std::to_string(text_y - 1.0f) + ")");
  };
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "files"), "Go to File opens");
  check("Go to File", workspace::OverlayMode::FileFinder);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
  TestAccess::OpenCommandPalette(fixture.shell);
  check("the command palette", workspace::OverlayMode::CommandPalette);
}

void TestEveryFieldsCaretSitsOnItsText() {
  UxFixture fixture("fields.txt", "alpha\nbeta\n");
  const auto snapshot = [&] {
    TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks());
    fixture.shell.Render(fixture.canvas.renderer(), UxFixture::kWidth, UxFixture::kHeight);
    (void)fixture.shell.ConsumePendingRenderInvalidation();
    SDL_FlushRenderer(fixture.canvas.renderer());
    return SDL_DuplicateSurface(fixture.canvas.surface());
  };
  const auto pixel = [](SDL_Surface* surface, int x, int y) {
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
    return (static_cast<Uint32>(r) << 16) | (static_cast<Uint32>(g) << 8) | b;
  };
  const auto check = [&](const std::string& what) {
    fixture.Frame();
    const auto surface = TestAccess::CurrentTextInputSurface(fixture.shell);
    Expect(surface != workspace::TextInputSurface::Editor && surface != workspace::TextInputSurface::None &&
               surface != workspace::TextInputSurface::Terminal,
           what + ": the field has the keyboard");
    SDL_Surface* empty = snapshot();
    Expect(TestAccess::HandleTextInput(fixture.shell, "MMMM"), what + ": the marker is typed into the field");
    SDL_Surface* typed = snapshot();
    const std::optional<SDL_FRect> caret = TestAccess::CaretDirtyRect(fixture.shell);
    Expect(caret.has_value(), what + ": a caret is drawn");
    if (caret.has_value() && empty != nullptr && typed != nullptr) {
      const float cell = caret->w;
      const int x0 = std::max(0, static_cast<int>(caret->x - cell * 3.0f));
      const int x1 = std::max(0, static_cast<int>(caret->x) - 1);
      int best_top = -1;
      int best_bottom = -1;
      float best_distance = 1e9f;
      const float caret_middle = caret->y + caret->h * 0.5f;
      int run_top = -1;
      for (int y = 0; y <= UxFixture::kHeight; ++y) {
        bool changed = false;
        for (int x = x0; y < UxFixture::kHeight && x < x1 && !changed; ++x) {
          changed = pixel(empty, x, y) != pixel(typed, x, y);
        }
        if (changed && run_top < 0) {
          run_top = y;
        } else if (!changed && run_top >= 0) {
          const float middle = (static_cast<float>(run_top) + static_cast<float>(y - 1)) * 0.5f;
          if (std::fabs(middle - caret_middle) < best_distance) {
            best_distance = std::fabs(middle - caret_middle);
            best_top = run_top;
            best_bottom = y - 1;
          }
          run_top = -1;
        }
      }
      Expect(best_top >= 0, what + ": the typed text is drawn just left of the caret (x " +
                                std::to_string(caret->x) + ")");
      Expect(best_top >= 0 && static_cast<float>(best_top) >= caret->y &&
                 static_cast<float>(best_bottom) <= caret->y + caret->h,
             what + ": the caret (y " + std::to_string(caret->y) + "-" + std::to_string(caret->y + caret->h) +
                 ") is on its text's row (ink " + std::to_string(best_top) + "-" + std::to_string(best_bottom) + ")");
    }
    SDL_DestroySurface(empty);
    SDL_DestroySurface(typed);
    (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
    TestAccess::SetFocusEditor(fixture.shell);
    fixture.Frame();
  };

  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "files"), "Go to File opens");
  check("Go to File");
  TestAccess::OpenCommandPalette(fixture.shell);
  check("the command palette");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "language-mode"), "the language picker opens");
  check("a quick pick (Change Language Mode)");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_G, SDL_KMOD_CTRL)});
  check("the Go to Line prompt");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_S, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  check("the Save As prompt");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)});
  check("the find widget");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_H, SDL_KMOD_CTRL)});
  check("the replace widget's search field");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_H, SDL_KMOD_CTRL), KeyDownEvent(SDLK_TAB)});
  check("the replace widget's replace field");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  check("the sidebar search");
  if (TestAccess::TerminalTabCount(fixture.shell) == 0) {
    TestAccess::OpenTerminalViaService(fixture.shell);
  }
  TestAccess::SetPanelContentTerminal(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "focus panel"), "focus the terminal");
  (void)DispatchEvents(fixture.shell,
                       {KeyDownEvent(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  check("the terminal's find bar");
}

void TestTheFinderCommandsArgumentIsAQuery() {
  UxFixture fixture("finder.txt", "x\n");
  std::filesystem::create_directories(fixture.root / "src");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "files src"), "files runs");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["overlay"]["visible"].AsBool() && ui["overlay"]["mode"].AsString() == "file-finder" &&
             ui["overlay"]["query"].AsString() == "src",
         "the finder opens with the query typed in");
  Expect(TestAccess::ProjectRoot(fixture.shell) == fixture.root, "and the project is still the same one");
}

void TestTheColourSchemeSettingOffersEveryScheme() {
  UxFixture fixture("scheme.txt", "x\n");
  Expect(TestAccess::SetSettingValue(fixture.shell, "editor.colorscheme", "light"),
         "a bundled scheme is a valid value");
  Expect(TestAccess::GetSettingValue(fixture.shell, "editor.colorscheme") == "light",
         "and it is the active one");
  Expect(!TestAccess::SetSettingValue(fixture.shell, "editor.colorscheme", "no-such-scheme"),
         "a scheme that does not exist is refused");
  Expect(TestAccess::GetSettingValue(fixture.shell, "editor.colorscheme") == "light", "and changes nothing");
  TestAccess::OpenSettingsOverlay(fixture.shell);
  std::set<std::string> seen;
  for (int i = 0; i < 64; ++i) {
    TestAccess::StepSetting(fixture.shell, "editor.colorscheme", true);
    seen.insert(TestAccess::GetSettingValue(fixture.shell, "editor.colorscheme").value_or(""));
  }
  Expect(seen.contains("default") && seen.contains("light"),
         "stepping the row walks the schemes, saw " + std::to_string(seen.size()));
}

void TestTheColorThemePickPreviewsRevertsAndApplies() {
  UxFixture fixture("theme.txt", "x\n");
  auto active = [&] { return TestAccess::GetSettingValue(fixture.shell, "editor.colorscheme").value_or(""); };
  auto overlay = [&] { return TestAccess::ControlUi(fixture.shell)["overlay"]; };
  Expect(active() == "default", "the default scheme is in use");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_K, SDL_KMOD_CTRL), KeyDownEvent(SDLK_T, SDL_KMOD_CTRL)});
  Expect(overlay()["visible"].AsBool() && overlay()["mode"].AsString() == "quick-pick" &&
             overlay()["kind"].AsString() == "color-theme",
         "Ctrl+K Ctrl+T opens the theme pick");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("light")});
  Expect(active() == "light", "typing a filter previews its top match, got " + active());
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});
  Expect(!overlay()["visible"].AsBool() && active() == "default", "Escape puts the scheme back, got " + active());
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "colorscheme"), "a bare colorscheme opens the pick too");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("light"), KeyDownEvent(SDLK_RETURN)});
  Expect(!overlay()["visible"].AsBool() && active() == "light", "Enter keeps the chosen scheme, got " + active());
}

void TestChangeLanguageModeSetsTheDocumentsLanguage() {
  UxFixture fixture("code.txt", "int main() { return 0; }\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  const auto tokens_kinds = [&] {
    std::set<int> kinds;
    for (const editor::SyntaxTokenKind kind : viewport.HighlightedLineTokens(0)) {
      kinds.insert(static_cast<int>(kind));
    }
    return kinds;
  };
  const std::string detected = viewport.language_id();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "language-mode c++"), "language-mode c++ runs");
  fixture.Frame();
  Expect(viewport.language_id() == "c++", "the buffer is C++ now, got " + viewport.language_id());
  Expect(fixture.ElementLabel("status:language") == "c++", "the status bar says so");
  Expect(tokens_kinds().size() > 1, "and the line is highlighted as C++");
  Expect(!TestAccess::ExecuteCommandLine(fixture.shell, "language-mode klingon"), "an unknown language is refused");

  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_K, SDL_KMOD_CTRL), KeyDownEvent(SDLK_M), TextInputEvent("m")});
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["overlay"]["visible"].AsBool() && ui["overlay"]["kind"].AsString() == "language-mode",
         "Ctrl+K M opens the language pick");
  (void)DispatchEvents(fixture.shell, {TextInputEvent("plain text"), KeyDownEvent(SDLK_RETURN)});
  fixture.Frame();
  Expect(viewport.language_id() == "unknown" && fixture.ElementLabel("status:language") == "Plain Text",
         "Plain Text is chosen, got " + viewport.language_id());
  Expect(tokens_kinds().size() <= 1, "and nothing is highlighted");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "language-mode auto"), "auto runs");
  Expect(viewport.language_id() == detected, "Auto Detect goes back to " + detected + ", got " + viewport.language_id());
}

void TestARenderInvalidationKeepsNoCoveredRect() {
  workspace::RenderInvalidation redraw;
  const SDL_FRect big{0.0f, 0.0f, 100.0f, 100.0f};
  for (int i = 0; i < 4; ++i) {
    redraw.Add(big);
  }
  redraw.Add(SDL_FRect{10.0f, 10.0f, 5.0f, 5.0f});
  Expect(redraw.rects.size() == 1, "a repeated or covered rect is queued once, got " +
                                       std::to_string(redraw.rects.size()));
  workspace::RenderInvalidation small;
  small.Add(SDL_FRect{1.0f, 1.0f, 2.0f, 2.0f});
  small.Add(SDL_FRect{200.0f, 0.0f, 10.0f, 10.0f});
  small.Merge(redraw);
  Expect(small.rects.size() == 2, "a merged rect replaces the queued ones it covers, got " +
                                      std::to_string(small.rects.size()));
}

}

void RegisterQaFlowRegressionUxTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegressionUx/ALineNumberPressSelectsWholeLines", TestALineNumberPressSelectsWholeLines);
  AddTest(tests, "QaFlowRegressionUx/TheQuickOpenCaretSitsInItsField", TestTheQuickOpenCaretSitsInItsField);
  AddTest(tests, "QaFlowRegressionUx/EveryFieldsCaretSitsOnItsText", TestEveryFieldsCaretSitsOnItsText);
  AddTest(tests, "QaFlowRegressionUx/AltEnterInTheFindWidgetSelectsEveryMatch",
          TestAltEnterInTheFindWidgetSelectsEveryMatch);
  AddTest(tests, "QaFlowRegressionUx/ARightClickInsideASelectionKeepsIt", TestARightClickInsideASelectionKeepsIt);
  AddTest(tests, "QaFlowRegressionUx/ARightClickBelowAGhostSuggestionLandsOnTheClickedLine",
          TestARightClickBelowAGhostSuggestionLandsOnTheClickedLine);
  AddTest(tests, "QaFlowRegressionUx/TheFindWidgetFollowsATabSwitch", TestTheFindWidgetFollowsATabSwitch);
  AddTest(tests, "QaFlowRegressionUx/CtrlKSSavesEveryDirtyFile", TestCtrlKSSavesEveryDirtyFile);
  AddTest(tests, "QaFlowRegressionUx/TheUnsavedPromptsLettersDoNotType", TestTheUnsavedPromptsLettersDoNotType);
  AddTest(tests, "QaFlowRegressionUx/AKeyReadsItsOwnModifiersNotTheNextChords",
          TestAKeyReadsItsOwnModifiersNotTheNextChords);
  AddTest(tests, "QaFlowRegressionUx/RevertFileDiscardsChangesUndoably", TestRevertFileDiscardsChangesUndoably);
  AddTest(tests, "QaFlowRegressionUx/TheStatusColumnFollowsTheTabSize", TestTheStatusColumnFollowsTheTabSize);
  AddTest(tests, "QaFlowRegressionUx/TheFindWidgetRecountsAfterAReload", TestTheFindWidgetRecountsAfterAReload);
  AddTest(tests, "QaFlowRegressionUx/SaveAsRetitlesTheTab", TestSaveAsRetitlesTheTab);
  AddTest(tests, "QaFlowRegressionUx/AClickOnAGitEntryKeepsTheKeyboardOnTheList",
          TestAClickOnAGitEntryKeepsTheKeyboardOnTheList);
  AddTest(tests, "QaFlowRegressionUx/ASelectionInsideAStringIsSurrounded", TestASelectionInsideAStringIsSurrounded);
  AddTest(tests, "QaFlowRegressionUx/EnterContinuesADocComment", TestEnterContinuesADocComment);
  AddTest(tests, "QaFlowRegressionUx/ARenderInvalidationKeepsNoCoveredRect", TestARenderInvalidationKeepsNoCoveredRect);
  AddTest(tests, "QaFlowRegressionUx/ChangeLanguageModeSetsTheDocumentsLanguage",
          TestChangeLanguageModeSetsTheDocumentsLanguage);
  AddTest(tests, "QaFlowRegressionUx/TheColorThemePickPreviewsRevertsAndApplies",
          TestTheColorThemePickPreviewsRevertsAndApplies);
  AddTest(tests, "QaFlowRegressionUx/TheColourSchemeSettingOffersEveryScheme",
          TestTheColourSchemeSettingOffersEveryScheme);
  AddTest(tests, "QaFlowRegressionUx/TheFinderCommandsArgumentIsAQuery", TestTheFinderCommandsArgumentIsAQuery);
  AddTest(tests, "QaFlowRegressionUx/SwitchingBackToATabKeepsItsScroll", TestSwitchingBackToATabKeepsItsScroll);
  AddTest(tests, "QaFlowRegressionUx/AnEditorsViewSurvivesCloseReopenAndRestart",
          TestAnEditorsViewSurvivesCloseReopenAndRestart);
  AddTest(tests, "QaFlowRegressionUx/TheWindowTitleNamesTheActiveFileAndItsState",
          TestTheWindowTitleNamesTheActiveFileAndItsState);
}

}
