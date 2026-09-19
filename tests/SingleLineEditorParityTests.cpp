// The single-line fields and the main editor, held against each other.
//
// Every prompt, search box, rename field and commit-message line in the window
// is a `SingleLineEditor`; the editor pane is a `TextViewport`. They are two
// implementations of the same keyboard contract over the same text, and the
// history here is drift, not bugs in one of them: the word rule lived as three
// file-local statics inside SingleLineEditor.cpp, so the main editor had no word
// verbs at all; double-click resolved its run with a second private scan, so the
// prompt selected nothing where the editor selected `->`. Both are now shared
// (`editor/WordBoundary.h`), which removes the arithmetic as a source of
// divergence but not the code AROUND it -- what a verb does at a boundary, over
// a selection, or when it declines to act is still written twice.
//
// So: drive both with the same text, the same caret, the same selection and the
// same verb, and require the same (text, caret, selection) out. Over the shapes
// the two paths get wrong -- multi-byte scalars, tabs, operator runs, leading
// and trailing whitespace, an empty field.
//
// Deliberately NOT compared: Home and End. `TextViewport::MoveCursorLineStart`
// is VS Code's `cursorHome`, which stops at the first non-whitespace character
// and toggles to column 0 from there; a single-line field has no indentation to
// toggle against and goes to 0, which is what every GTK/Qt entry does. That is a
// real difference in the contract, not drift, and it has its own tests on both
// sides.

#include "TestSupport.h"

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::SingleLineEditor;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

// One comparable state: the text, where the caret is, and what is selected.
struct State {
  std::string text;
  std::size_t caret = 0;
  std::optional<std::size_t> anchor;

  bool operator==(const State& other) const = default;
};

State Read(const SingleLineEditor& editor) {
  State state;
  state.text = editor.text();
  state.caret = editor.caret();
  if (editor.HasSelection()) {
    state.anchor = editor.selection_anchor();
  }
  return state;
}

State Read(const TextViewport& viewport) {
  State state;
  state.text = std::string(viewport.lines().LineView(0));
  state.caret = viewport.cursor_column();
  if (const auto selection = viewport.selection_range(); selection.has_value()) {
    state.anchor = selection->start.column == state.caret ? selection->end.column
                                                          : selection->start.column;
  }
  return state;
}

std::string Show(const State& state) {
  std::string out = "\"";
  for (char c : state.text) {
    out += c == '\t' ? std::string("\\t") : std::string(1, c);
  }
  out += "\" caret=" + std::to_string(state.caret);
  if (state.anchor.has_value()) {
    out += " anchor=" + std::to_string(*state.anchor);
  }
  return out;
}

enum class Verb {
  kLeft,
  kRight,
  kShiftLeft,
  kShiftRight,
  kWordLeft,
  kWordRight,
  kShiftWordLeft,
  kShiftWordRight,
  kInsert,
  kBackspace,
  kDeleteForward,
  kDeleteWordLeft,
  kDeleteWordRight,
  kSelectAll,
  kSelectWord,
  kCount,
};

const char* VerbName(Verb verb) {
  switch (verb) {
    case Verb::kLeft: return "Left";
    case Verb::kRight: return "Right";
    case Verb::kShiftLeft: return "Shift+Left";
    case Verb::kShiftRight: return "Shift+Right";
    case Verb::kWordLeft: return "Ctrl+Left";
    case Verb::kWordRight: return "Ctrl+Right";
    case Verb::kShiftWordLeft: return "Ctrl+Shift+Left";
    case Verb::kShiftWordRight: return "Ctrl+Shift+Right";
    case Verb::kInsert: return "type";
    case Verb::kBackspace: return "Backspace";
    case Verb::kDeleteForward: return "Delete";
    case Verb::kDeleteWordLeft: return "Ctrl+Backspace";
    case Verb::kDeleteWordRight: return "Ctrl+Delete";
    case Verb::kSelectAll: return "Ctrl+A";
    case Verb::kSelectWord: return "double-click";
    case Verb::kCount: return "?";
  }
  return "?";
}

void Apply(SingleLineEditor& editor, Verb verb, char typed, std::size_t word_at) {
  switch (verb) {
    case Verb::kLeft: editor.MoveLeft(); break;
    case Verb::kRight: editor.MoveRight(); break;
    case Verb::kShiftLeft: editor.MoveLeft(true); break;
    case Verb::kShiftRight: editor.MoveRight(true); break;
    case Verb::kWordLeft: editor.MoveWordLeft(); break;
    case Verb::kWordRight: editor.MoveWordRight(); break;
    case Verb::kShiftWordLeft: editor.MoveWordLeft(true); break;
    case Verb::kShiftWordRight: editor.MoveWordRight(true); break;
    case Verb::kInsert: editor.Insert(std::string_view(&typed, 1)); break;
    case Verb::kBackspace: editor.Backspace(); break;
    case Verb::kDeleteForward: editor.DeleteForward(); break;
    case Verb::kDeleteWordLeft: editor.DeleteWordLeft(); break;
    case Verb::kDeleteWordRight: editor.DeleteWordRight(); break;
    case Verb::kSelectAll: editor.SelectAll(); break;
    case Verb::kSelectWord:
      // Both sides put the caret on the click point first, which is what the two
      // mouse handlers do; comparing a bare SelectWordAt against the editor's
      // move-then-select would be comparing two different gestures.
      editor.SetSelectionAnchor(std::nullopt);
      editor.SetCaret(word_at);
      editor.SelectWordAt(word_at);
      break;
    case Verb::kCount: break;
  }
}

void Apply(TextViewport& viewport, Verb verb, char typed, std::size_t word_at) {
  switch (verb) {
    case Verb::kLeft: viewport.MoveCursorHorizontal(-1); break;
    case Verb::kRight: viewport.MoveCursorHorizontal(1); break;
    case Verb::kShiftLeft: viewport.MoveCursorHorizontal(-1, true); break;
    case Verb::kShiftRight: viewport.MoveCursorHorizontal(1, true); break;
    case Verb::kWordLeft: viewport.MoveCursorWord(-1); break;
    case Verb::kWordRight: viewport.MoveCursorWord(1); break;
    case Verb::kShiftWordLeft: viewport.MoveCursorWord(-1, true); break;
    case Verb::kShiftWordRight: viewport.MoveCursorWord(1, true); break;
    case Verb::kInsert: viewport.InsertCharacter(typed); break;
    case Verb::kBackspace: viewport.Backspace(); break;
    case Verb::kDeleteForward: viewport.DeleteForward(); break;
    case Verb::kDeleteWordLeft: viewport.DeleteWord(-1); break;
    case Verb::kDeleteWordRight: viewport.DeleteWord(1); break;
    case Verb::kSelectAll: viewport.SelectAll(); break;
    case Verb::kSelectWord:
      viewport.MoveCursorTo(0, word_at);
      viewport.SelectWordOrRunAtCursor();
      break;
    case Verb::kCount: break;
  }
}

// The shapes the two paths get wrong: multi-byte scalars, a tab, operator runs
// that are words of their own, leading and trailing whitespace, and the empty
// field every verb has to decline on.
const std::vector<std::string>& ParityTexts() {
  static const std::vector<std::string> texts = {
      "hello world",
      "  leading and trailing   ",
      "a->b.c(d, e)",
      "caf\xc3\xa9 na\xc3\xafve \xe6\x97\xa5\xe6\x9c\xac",
      "\ttab\tseparated\t",
      "one",
      "",
      "   ",
      "x==y",
  };
  return texts;
}

void TestSingleLineAndViewportAgreeOnEveryVerb() {
  std::mt19937 rng(20260919u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  std::vector<int> ran(static_cast<std::size_t>(Verb::kCount), 0);
  std::vector<int> changed(static_cast<std::size_t>(Verb::kCount), 0);

  for (const std::string& text : ParityTexts()) {
    for (int iteration = 0; iteration < 300; ++iteration) {
      SingleLineEditor editor(text);
      TextViewport viewport;
      viewport.SetViewportSize(4, 400);
      viewport.LoadContent(text, "/tmp/single-line-parity.txt");

      // The same caret and the same selection on both, set by BYTE offset so
      // nothing about the placement can differ.
      const std::size_t caret = pick(text.size() + 1);
      editor.SetCaret(caret);
      viewport.MoveCursorTo(0, caret);
      if (pick(2) == 0 && !text.empty()) {
        const std::size_t anchor = pick(text.size() + 1);
        // Caret first, then the anchor: SetSelectionAnchor normalises an anchor
        // that equals the caret away, so the other order silently loses it.
        editor.SetCaret(caret);
        editor.SetSelectionAnchor(anchor);
        viewport.MoveCursorTo(0, anchor);
        viewport.MoveCursorTo(0, caret, /*extend_selection=*/true);
      }
      // Whatever each side's own clamp did to a mid-scalar offset has to agree
      // before a verb is judged, or the first difference is the setup's.
      Expect(Read(editor) == Read(viewport),
             "the setup itself diverged: field " + Show(Read(editor)) + " vs editor " +
                 Show(Read(viewport)));

      std::string trail;
      for (int step = 0; step < 5; ++step) {
        const Verb verb = static_cast<Verb>(pick(static_cast<std::size_t>(Verb::kCount)));
        const char typed = static_cast<char>('a' + pick(26));
        const std::size_t word_at = pick(editor.text().size() + 1);
        trail += std::string(step == 0 ? "" : " -> ") + VerbName(verb);

        const State before = Read(editor);
        Apply(editor, verb, typed, word_at);
        Apply(viewport, verb, typed, word_at);
        ++ran[static_cast<std::size_t>(verb)];

        const State field = Read(editor);
        const State pane = Read(viewport);
        if (!(field == before)) {
          ++changed[static_cast<std::size_t>(verb)];
        }
        Expect(field == pane, trail + " from " + Show(before) + ": field gave " + Show(field) +
                                  ", editor gave " + Show(pane));
        if (!(field == pane)) {
          return;
        }
      }
    }
  }

  for (std::size_t i = 0; i < static_cast<std::size_t>(Verb::kCount); ++i) {
    const char* name = VerbName(static_cast<Verb>(i));
    Expect(ran[i] > 50, std::string("the verb ") + name + " barely ran: " +
                            std::to_string(ran[i]) + " times");
    Expect(changed[i] > 0, std::string("the verb ") + name +
                               " never once changed the state, so the two paths were compared "
                               "only where both decline to act");
  }
}

}  // namespace

void RegisterSingleLineEditorParityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SingleLineEditorParity/AgreeOnEveryVerb",
          TestSingleLineAndViewportAgreeOnEveryVerb);
}

}  // namespace microide::tests
