// Regressions the GUI QA ux flows (tools/qa/flows_ux_*.py) found, pinned in
// process: real events through HandleEvent, checked against the same queries the
// harness reads. The redraw-diff regressions stay in QaFlowRegressionTests.cpp;
// this file holds the behavioural ones -- what a press, a key or a drag DOES.
#include "TestSupport.h"
#include "TestSupportSdl.h"

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

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

// The buffer as one string, lines joined by newlines.
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

  // The window point of the middle of the line-number column on 1-based `line`.
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

  // The window point of 1-based (line, column) in the text, a fifth into the cell.
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

  // A press and release of `button` at `point`, `clicks` deep.
  void Click(SDL_FPoint point, Uint8 button = SDL_BUTTON_LEFT, Uint8 clicks = 1) {
    SDL_Event down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, point.x, point.y);
    SDL_Event up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, point.x, point.y);
    down.button.button = up.button.button = button;
    down.button.clicks = up.button.clicks = clicks;
    (void)DispatchEvents(shell, {MouseMotionEvent(point.x, point.y), down, up});
  }

  // The label of the ui element named `name` ("" when absent).
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

  // The primary selection as 0-based (anchor line, anchor column, caret line, caret column).
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

// editor-mouse: a click on a line number placed a bare caret, and a drag down the
// numbers stopped at the start of the line under the pointer. VS Code selects the
// clicked line whole, and a drag over the numbers takes every line it crosses.
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


// find-select-all: Alt+Enter in the find widget did nothing. VS Code selects every
// match the widget counts (its regex and case options included) and gives the
// editor the keyboard, so the next keystroke replaces them all.
void TestAltEnterInTheFindWidgetSelectsEveryMatch() {
  UxFixture fixture("find.txt", "foo bar Foo\nbaz foo\nfoo\n");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL), TextInputEvent("foo")});
  // Match case on: "Foo" on line 0 is not one of them.
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

// editor-context-menu: a right-click inside a selection collapsed it to the
// pointer, so the menu's Cut then took the whole LINE (an empty-selection cut)
// instead of the word that had been selected. VS Code keeps the selection.
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

// editor-context-menu: the right-click retargets the caret through its own copy
// of the click's pointer-to-position mapping, and that copy did not open the gap
// a multi-line ghost-text suggestion draws below its line -- so a right-click on
// a line below a suggestion put the caret on a line above the one clicked. The
// left click mirrored the gap; both now share one mapping.
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
  // The suggestion draws its two continuation rows between lines 1 and 2, so
  // line 4 sits two rows lower than the gapless row table (ControlUi `rows`) says.
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

// find-across-tabs: the find widget stays open across a tab switch, but its
// matches stayed the previous tab's: the n-of-m kept the old count, and F3 in the
// new tab jumped to the old file's coordinates and selected nothing there.
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

// save-all: there was no Save All. VS Code's File > Save All (Ctrl+K S) writes
// every unsaved file in every group; an untitled buffer has no name, so it is
// left and the notice says so. The chord's second key is a printable S, whose
// text must not land in the buffer.
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

// save-all: the unsaved prompt's letter keys typed themselves. D (Don't Save)
// closed the untitled buffer and its "d" landed in the file that came to the
// front; C (Cancel) put a "c" into the very buffer the prompt was protecting.
// SDL follows a printable key press with its TEXT_INPUT, and the prompt took
// the press but not the text.
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
  // The next press is a fresh one: its text is typed.
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_X), TextInputEvent("x")});
  Expect(ViewportText(TestAccess::ActiveEditor(fixture.shell)) == "xclean\n",
         "the following key types as usual, got " + ViewportText(TestAccess::ActiveEditor(fixture.shell)));
}

// multi-caret-pairs, on CI only: End, then Ctrl+Alt+Down twice, left one caret
// at the end of the DOCUMENT. A key event that carried no modifiers fell back to
// SDL's live modifier state -- which SDL updates when it pumps, so on a runner
// behind its input it already held the NEXT chord's Ctrl and Alt, and End ran as
// Ctrl+End. A key's modifiers are the ones its event carries.
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

  // The unsaved-changes prompt's letters read the same way: a Shift pumped for the
  // next key made `d` a `D`, which answers nothing.
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

// revert-file: there was no way to throw unsaved changes away but undoing them
// one by one -- Reload from Disk refuses a dirty buffer, rightly, because it
// swaps the buffer and its history. VS Code's Revert File takes the disk's text
// as one undoable edit, so Ctrl+Z brings the discarded changes back.
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

// state oracle: changing the tab size moved a caret that sat after a tab. The
// viewport re-derived the caret's character from the VISUAL column it remembered
// for Up/Down, and a tab is worth more columns now: `\tx|` became `\t|x`. The
// status bar's Col (the visible column) then named the wrong place too.
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

// state oracle (wb-project-search): a project Replace All rewrote the file under
// an open find widget and the clean editor reloaded it -- and the widget kept
// counting the five matches that were gone. The match set's freshness was keyed
// on (viewport, content revision), and a reload gives the same viewport a new
// document whose revisions start over, so the old revision recurred over new text.
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

// state oracle: a renamed or Saved As file's tab kept its old name until the next
// edit. The tab strip's geometry cache was keyed on the tabs' dirty marks, not
// their titles, so a retitle with nothing else changing reused the old labels.
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

// git-view click: a click on a git view entry opened its diff and handed the
// keyboard to the editor, so the view's own `s` (stage) typed an "s" into the
// file. The tree keeps the keyboard on a click, as VS Code's source control view
// does; Enter is the open that moves it.
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

// surround: a pair key over a selection that starts inside a string -- or on a
// line a lone quote above made the highlighter call a string -- replaced the
// selection with the opener instead of surrounding it. The auto-close rule (no
// pairs inside strings) had been applied to surround, where the alternative to
// surrounding is deleting what was selected.
void TestASelectionInsideAStringIsSurrounded() {
  UxFixture fixture("strings.c", "char* s = \"hello world\";\nx\"\ny\"\n");
  editor::TextViewport& viewport = TestAccess::ActiveEditor(fixture.shell);
  viewport.MoveCursorTo(0, 11);
  viewport.MoveCursorTo(0, 16, /*extend_selection=*/true);  // "hello" inside the literal
  (void)DispatchEvents(fixture.shell, {TextInputEvent("(")});
  Expect(viewport.lines()[0] == "char* s = \"(hello) world\";",
         "the word inside the string is surrounded, got " + std::string(viewport.lines()[0]));
  viewport.MoveCursorTo(2, 0);
  viewport.MoveCursorTo(2, 2, /*extend_selection=*/true);  // y" on the line after x"
  (void)DispatchEvents(fixture.shell, {TextInputEvent("[")});
  Expect(viewport.lines()[2] == "[y\"]", "the line after a lone quote is surrounded, got " +
                                            std::string(viewport.lines()[2]));
  // The multi-caret path had its own copy of the rule: three carets selecting
  // three lines, the middle one after the lone quote.
  UxFixture three("three.c", "x\"\ny\"\nz\"\n");
  editor::TextViewport& view = TestAccess::ActiveEditor(three.shell);
  view.MoveCursorTo(0, 0);
  view.MoveCursorTo(0, 2, /*extend_selection=*/true);
  view.AddSecondaryCaretWithRange(editor::SelectionRange{{1, 0}, {1, 2}});
  view.AddSecondaryCaretWithRange(editor::SelectionRange{{2, 0}, {2, 2}});
  (void)DispatchEvents(three.shell, {TextInputEvent("(")});
  Expect(ViewportText(view) == "(x\")\n(y\")\n(z\")\n",
         "every caret's selection is surrounded, got " + ViewportText(view));
}

// doc comments: Enter inside a `/** */` comment started the next line with the
// indentation only. VS Code's C-family rules continue it with ` * `.
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

// TD-2026-10-22-384: the window was always titled "microide", so two windows
// were two identical taskbar entries and nothing outside the tab strip showed an
// unsaved file. VS Code's `● file — folder — app`.
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

// TD-2026-10-24-390 / TD-2026-10-18-355: Reopen Closed Editor restored the caret
// but revealed it rather than the scroll the editor had, and dropped its folds; a
// restart dropped every fold and forgot the closed editors. VS Code restores an
// editor's whole view state either way.
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

  // Close another file so the closed list has something to carry over.
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

  // A fold collapsed around a caret standing still stays collapsed (VS Code's
  // revealCursor reveals only a caret that MOVED in); restoring that view is not
  // a move.
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

// Found by the fold-restore test (TD-2026-10-24-390): every activation set the
// tab size from the settings and then back to a 2-space file's own, each change
// revealing the caret, so switching back to a tab whose caret was scrolled off
// screen jumped the view to it. VS Code keeps a tab's scroll across switches.
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

// TD-2026-10-20-370: `files src` opened `src` as a second project and showed
// the finder there; its argument is now Go to File's query.
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

// TD-2026-10-20-371: the colour-scheme setting was an enum of one value,
// `default`: the Settings row cycled through nothing and `set-setting
// editor.colorscheme <name>` refused every other scheme. Its values are now the
// themes there are.
void TestTheColourSchemeSettingOffersEveryScheme() {
  UxFixture fixture("scheme.txt", "x\n");
  // `default` and `light` are the schemes on every machine (CI's whole list).
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

// TD-2026-10-18-363: no colour-theme picker; a scheme could be applied only by
// name. Ctrl+K Ctrl+T (a bare `colorscheme`, the palette's Color Theme…) opens a
// quick pick of the schemes that previews each as the selection moves (VS Code),
// puts the one in use back on Escape and keeps the chosen one on Enter.
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

// TD-2026-10-24-388: nothing set a buffer's language by hand. Change Language Mode
// (Ctrl+K M, the status bar's language, `language-mode`) sets it for the document:
// highlighting, the status bar and the language id follow; Plain Text highlights
// nothing; Auto Detect goes back to what the file says.
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

// menu_hover_paint allocated 60% more than v2.12.0: one File/Edit switch queued
// the same popup rect four times, and the duplicates overflowed the invalidation's
// inline storage on every hover. A rect a queued one covers adds nothing; one that
// covers queued rects replaces them.
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

}  // namespace

void RegisterQaFlowRegressionUxTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegressionUx/ALineNumberPressSelectsWholeLines", TestALineNumberPressSelectsWholeLines);
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

}  // namespace microide::tests
