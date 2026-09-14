// IME composition: the uncommitted text an input method shows before the user
// accepts it.
//
// `active-work.md` § 4 lists IME hardening as open, and the composition path had
// no tests at all. What matters about it is not the happy path but the ABANDONS:
// the preview is state that lives outside the document, so every way of leaving
// it -- opening a menu, moving focus to a surface that cannot show it, an empty
// update from the IME -- has to clear it, or a stale preview is painted over
// text it no longer belongs to.

#include "TestSupport.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

namespace editor = microide::editor;
using microide::workspace::TextInputSurface;
using microide::workspace::WorkspaceShell;
using TestAccess = WorkspaceShell::TestAccess;

SDL_Event EditingEvent(const char* text, int start = 0, int length = 0) {
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_EDITING;
  event.edit.text = text;
  event.edit.start = start;
  event.edit.length = length;
  return event;
}

SDL_Event InputEvent(const char* text) {
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.text = text;
  return event;
}

// Opens a real editor tab, as the surrounding shell tests do: the composition
// path only runs when a text-input surface is actually focused.
struct EditorFixture {
  TemporaryDirectory temp;
  WorkspaceShell shell;

  explicit EditorFixture(std::string_view contents) {
    const std::filesystem::path root = temp.path() / "project";
    const std::filesystem::path file = root / "ime.txt";
    WriteFile(file, std::string(contents));
    TestAccess::SetProjectRoot(shell, root);
    TestAccess::SetWindowSize(shell, 1280, 720);
    TestAccess::OpenSingleEditorTab(shell, file);
  }

  editor::TextViewport& viewport() { return TestAccess::ActiveEditor(shell); }
};

// A composition update records the in-flight text against the surface showing it.
void TestCompositionRecordsTheInFlightText() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));  // "か", mid-composition

  const auto& composition = TestAccess::TextComposition(shell);
  Expect(composition.text == "\xe3\x81\x8b", "the in-flight text is recorded");
  Expect(composition.surface != TextInputSurface::None,
         "the composition names the surface showing it");
}

// Committing clears the preview. Leaving it set would paint the uncommitted text
// on top of the committed text it just became.
void TestCommitClearsTheComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  shell.HandleEvent(InputEvent("\xe6\xbc\xa2"));  // the accepted "漢"
  Expect(TestAccess::TextComposition(shell).text.empty(),
         "accepting the composition clears the preview");
  // ...and the accepted text is actually in the buffer, so the assertion above
  // cannot pass on an insert that silently did nothing.
  Expect(fixture.viewport().lines()[0].find("\xe6\xbc\xa2") != std::string::npos,
         "the accepted text reached the buffer, got: " + std::string(fixture.viewport().lines()[0]));
}

// An empty update is how an input method says "cancelled"; the preview has to go.
void TestEmptyUpdateAbandonsTheComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  shell.HandleEvent(EditingEvent("", 0, 0));
  Expect(TestAccess::TextComposition(shell).text.empty(),
         "an empty composition update abandons the preview");
}

// Composition survives its own updates: a multi-step conversion replaces the
// preview rather than appending to it.
void TestCompositionUpdateReplacesRatherThanAppends() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b\xe3\x82\x93", 0, 2));  // "かん"

  Expect(TestAccess::TextComposition(shell).text == "\xe3\x81\x8b\xe3\x82\x93",
         "a later update replaces the preview, got: " +
             TestAccess::TextComposition(shell).text);
}

// The committed text goes in at EVERY caret, as ordinary typing does: an IME is
// a text source like any other, and a multi-caret edit that only fed the primary
// would silently drop the other carets.
void TestCommitReachesEveryCaret() {
  EditorFixture fixture("ab\nab\n");
  WorkspaceShell& shell = fixture.shell;
  editor::TextViewport* viewport = &fixture.viewport();
  viewport->MoveCursorTo(0, 1);
  viewport->SetSecondaryCarets({{1, 1}});

  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  shell.HandleEvent(InputEvent("\xe6\xbc\xa2"));  // "漢"

  Expect(viewport->lines()[0] == "a\xe6\xbc\xa2\x62" && viewport->lines()[1] == "a\xe6\xbc\xa2\x62",
         "the accepted text lands at both carets");
  Expect(TestAccess::TextComposition(shell).text.empty(), "and the preview is cleared");
}


SDL_Event KeyDownEvent(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = modifiers;
  return event;
}

// While an input method is composing, the keys it uses to drive the conversion
// belong to IT, not to the editor. Arrows pick a candidate, Enter accepts,
// Escape cancels, Backspace edits the reading -- if the editor also acts on them
// the caret walks away from the text being composed, or Enter splits the line
// under the preview.
void TestCompositionSwallowsItsConversionKeys() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  fixture.viewport().MoveCursorTo(0, 1);
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));

  const std::size_t lines_before = fixture.viewport().lines().size();
  for (const SDL_Keycode key : {SDLK_RETURN, SDLK_ESCAPE, SDLK_BACKSPACE, SDLK_DELETE, SDLK_TAB,
                                SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN, SDLK_HOME, SDLK_END,
                                SDLK_PAGEUP, SDLK_PAGEDOWN}) {
    shell.HandleEvent(KeyDownEvent(key));
  }

  Expect(fixture.viewport().lines().size() == lines_before,
         "no conversion key may split or join a line under the preview");
  Expect(fixture.viewport().lines()[0] == "ab",
         "no conversion key may edit the text under the preview, got: " +
             std::string(fixture.viewport().lines()[0]));
  Expect(fixture.viewport().cursor_line() == 0 && fixture.viewport().cursor_column() == 1,
         "the caret stays where the composition is anchored");
}

// A CHORD is not a conversion key: Ctrl+S during composition is still Save, and
// swallowing it would make the app stop responding to shortcuts mid-word.
void TestCompositionDoesNotSwallowChords() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  fixture.viewport().MoveCursorTo(0, 2);
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  // The chord has to use a key that IS in the swallow list, or the test proves
  // nothing about the modifier guard -- Ctrl+A passed even with the guard removed,
  // because `A` is never swallowed anyway. Ctrl+Shift+Left is word-select-left,
  // and `Left` is very much on the list.
  const auto chord = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  shell.HandleEvent(KeyDownEvent(SDLK_LEFT, chord));
  Expect(fixture.viewport().has_selection(),
         "a modified conversion key still reaches the editor");
}

// With no composition in flight the same keys are ordinary editing keys again --
// the guard is the composition, not the key.
void TestConversionKeysWorkNormallyWithoutAComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  fixture.viewport().MoveCursorTo(0, 1);

  shell.HandleEvent(KeyDownEvent(SDLK_RETURN));
  Expect(fixture.viewport().lines().size() > 1,
         "Enter splits the line when nothing is being composed");
}

}  // namespace

void RegisterImeCompositionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ImeComposition/RecordsTheInFlightText", TestCompositionRecordsTheInFlightText);
  AddTest(tests, "ImeComposition/CommitClearsTheComposition", TestCommitClearsTheComposition);
  AddTest(tests, "ImeComposition/EmptyUpdateAbandonsTheComposition",
          TestEmptyUpdateAbandonsTheComposition);
  AddTest(tests, "ImeComposition/UpdateReplacesRatherThanAppends",
          TestCompositionUpdateReplacesRatherThanAppends);
  AddTest(tests, "ImeComposition/CommitReachesEveryCaret", TestCommitReachesEveryCaret);
  AddTest(tests, "ImeComposition/SwallowsItsConversionKeys", TestCompositionSwallowsItsConversionKeys);
  AddTest(tests, "ImeComposition/DoesNotSwallowChords", TestCompositionDoesNotSwallowChords);
  AddTest(tests, "ImeComposition/ConversionKeysWorkNormallyWithoutAComposition",
          TestConversionKeysWorkNormallyWithoutAComposition);
}

}  // namespace microide::tests
