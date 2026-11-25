#include "TestSupport.h"
#include "WorkspaceShellEventHelpers.h"

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

SDL_Event InputEvent(const char* text) { return TextInputEvent(text); }

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

void TestCompositionRecordsTheInFlightText() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));

  const auto& composition = TestAccess::TextComposition(shell);
  Expect(composition.text == "\xe3\x81\x8b", "the in-flight text is recorded");
  Expect(composition.surface != TextInputSurface::None,
         "the composition names the surface showing it");
}

void TestCommitClearsTheComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  shell.HandleEvent(InputEvent("\xe6\xbc\xa2"));
  Expect(TestAccess::TextComposition(shell).text.empty(),
         "accepting the composition clears the preview");
  Expect(fixture.viewport().lines()[0].find("\xe6\xbc\xa2") != std::string::npos,
         "the accepted text reached the buffer, got: " + std::string(fixture.viewport().lines()[0]));
}

void TestEmptyUpdateAbandonsTheComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  shell.HandleEvent(EditingEvent("", 0, 0));
  Expect(TestAccess::TextComposition(shell).text.empty(),
         "an empty composition update abandons the preview");
}

void TestCompositionUpdateReplacesRatherThanAppends() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b\xe3\x82\x93", 0, 2));

  Expect(TestAccess::TextComposition(shell).text == "\xe3\x81\x8b\xe3\x82\x93",
         "a later update replaces the preview, got: " +
             TestAccess::TextComposition(shell).text);
}

void TestCommitReachesEveryCaret() {
  EditorFixture fixture("ab\nab\n");
  WorkspaceShell& shell = fixture.shell;
  editor::TextViewport* viewport = &fixture.viewport();
  viewport->MoveCursorTo(0, 1);
  viewport->SetSecondaryCarets({{1, 1}});

  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  shell.HandleEvent(InputEvent("\xe6\xbc\xa2"));

  Expect(viewport->lines()[0] == "a\xe6\xbc\xa2\x62" && viewport->lines()[1] == "a\xe6\xbc\xa2\x62",
         "the accepted text lands at both carets");
  Expect(TestAccess::TextComposition(shell).text.empty(), "and the preview is cleared");
}


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

void TestCompositionDoesNotSwallowChords() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  fixture.viewport().MoveCursorTo(0, 2);
  shell.HandleEvent(EditingEvent("\xe3\x81\x8b", 0, 1));
  Expect(!TestAccess::TextComposition(shell).text.empty(), "composition is in flight");

  const auto chord = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  shell.HandleEvent(KeyDownEvent(SDLK_LEFT, chord));
  Expect(fixture.viewport().has_selection(),
         "a modified conversion key still reaches the editor");
}

void TestConversionKeysWorkNormallyWithoutAComposition() {
  EditorFixture fixture("ab\n");
  WorkspaceShell& shell = fixture.shell;
  fixture.viewport().MoveCursorTo(0, 1);

  shell.HandleEvent(KeyDownEvent(SDLK_RETURN));
  Expect(fixture.viewport().lines().size() > 1,
         "Enter splits the line when nothing is being composed");
}

}

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

}
