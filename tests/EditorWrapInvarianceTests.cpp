#include "TestSupport.h"

#include <random>
#include <string>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) out.push_back('\n');
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

std::string CaretSet(const TextViewport& viewport) {
  std::string out = "P(" + std::to_string(viewport.cursor_line()) + "," +
                    std::to_string(viewport.cursor_column()) + ")";
  if (const auto sel = viewport.selection_range()) {
    out += "[" + std::to_string(sel->start.line) + "," + std::to_string(sel->start.column) + "-" +
           std::to_string(sel->end.line) + "," + std::to_string(sel->end.column) + "]";
  }
  for (const auto& caret : viewport.secondary_caret_ranges()) {
    out += " S(" + std::to_string(caret.position.line) + "," +
           std::to_string(caret.position.column) + ")";
    if (caret.selection_anchor.has_value()) {
      out += "[" + std::to_string(caret.selection_anchor->line) + "," +
             std::to_string(caret.selection_anchor->column) + "]";
    }
  }
  return out;
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

enum class Edit {
  kType,
  kNewline,
  kBackspace,
  kDeleteForward,
  kMoveLineUp,
  kMoveLineDown,
  kCopyLinesDown,
  kCopyLinesUp,
  kDeleteLine,
  kIndent,
  kOutdent,
  kInsertLineBelow,
  kInsertLineAbove,
  kSortAscending,
  kToggleLineComment,
  kToggleBlockComment,
  kJoinLines,
  kTab,
  kDeleteWordLeft,
  kDeleteWordRight,
  kPaste,
  kSelectWord,
  kSelectLine,
  kLeft,
  kRight,
  kShiftLeft,
  kShiftRight,
  kWordLeft,
  kWordRight,
  kUndo,
  kRedo,
  kCount,
};

const char* EditName(Edit edit) {
  switch (edit) {
    case Edit::kType: return "type";
    case Edit::kNewline: return "newline";
    case Edit::kBackspace: return "backspace";
    case Edit::kDeleteForward: return "delete-forward";
    case Edit::kMoveLineUp: return "move-line-up";
    case Edit::kMoveLineDown: return "move-line-down";
    case Edit::kCopyLinesDown: return "copy-lines-down";
    case Edit::kCopyLinesUp: return "copy-lines-up";
    case Edit::kDeleteLine: return "delete-line";
    case Edit::kIndent: return "indent";
    case Edit::kOutdent: return "outdent";
    case Edit::kInsertLineBelow: return "insert-line-below";
    case Edit::kInsertLineAbove: return "insert-line-above";
    case Edit::kSortAscending: return "sort-ascending";
    case Edit::kToggleLineComment: return "toggle-line-comment";
    case Edit::kToggleBlockComment: return "toggle-block-comment";
    case Edit::kJoinLines: return "join-lines";
    case Edit::kTab: return "tab";
    case Edit::kDeleteWordLeft: return "delete-word-left";
    case Edit::kDeleteWordRight: return "delete-word-right";
    case Edit::kPaste: return "paste";
    case Edit::kSelectWord: return "select-word";
    case Edit::kSelectLine: return "select-line";
    case Edit::kLeft: return "left";
    case Edit::kRight: return "right";
    case Edit::kShiftLeft: return "shift-left";
    case Edit::kShiftRight: return "shift-right";
    case Edit::kWordLeft: return "word-left";
    case Edit::kWordRight: return "word-right";
    case Edit::kUndo: return "undo";
    case Edit::kRedo: return "redo";
    case Edit::kCount: return "?";
  }
  return "?";
}

void ApplyEdit(TextViewport& viewport, Edit edit, char typed) {
  switch (edit) {
    case Edit::kType: viewport.InsertCharacter(typed); break;
    case Edit::kNewline: viewport.InsertNewline(); break;
    case Edit::kBackspace: viewport.Backspace(); break;
    case Edit::kDeleteForward: viewport.DeleteForward(); break;
    case Edit::kMoveLineUp: microide::editor::MoveLineUp(viewport); break;
    case Edit::kMoveLineDown: microide::editor::MoveLineDown(viewport); break;
    case Edit::kCopyLinesDown: microide::editor::CopyLines(viewport, true); break;
    case Edit::kCopyLinesUp: microide::editor::CopyLines(viewport, false); break;
    case Edit::kDeleteLine: microide::editor::DeleteLine(viewport); break;
    case Edit::kIndent: microide::editor::IndentSelection(viewport); break;
    case Edit::kOutdent: microide::editor::OutdentSelection(viewport); break;
    case Edit::kInsertLineBelow: microide::editor::InsertLineBelow(viewport); break;
    case Edit::kInsertLineAbove: microide::editor::InsertLineAbove(viewport); break;
    case Edit::kSortAscending: microide::editor::SortLines(viewport, true); break;
    case Edit::kToggleLineComment: microide::editor::ToggleLineComment(viewport, "//"); break;
    case Edit::kToggleBlockComment:
      microide::editor::ToggleBlockComment(viewport, "/*", "*/");
      break;
    case Edit::kJoinLines: microide::editor::JoinLinesAtCarets(viewport); break;
    case Edit::kTab: viewport.InsertTab(); break;
    case Edit::kDeleteWordLeft: viewport.DeleteWord(-1); break;
    case Edit::kDeleteWordRight: viewport.DeleteWord(1); break;
    case Edit::kPaste: viewport.PasteText("pasted\nlines\n"); break;
    case Edit::kSelectWord: viewport.SelectWordAtCursor(); break;
    case Edit::kSelectLine: viewport.SelectLineAtCursor(); break;
    case Edit::kLeft: viewport.MoveCursorHorizontal(-1); break;
    case Edit::kRight: viewport.MoveCursorHorizontal(1); break;
    case Edit::kShiftLeft: viewport.MoveCursorHorizontal(-1, true); break;
    case Edit::kShiftRight: viewport.MoveCursorHorizontal(1, true); break;
    case Edit::kWordLeft: viewport.MoveCursorWord(-1); break;
    case Edit::kWordRight: viewport.MoveCursorWord(1); break;
    case Edit::kUndo: viewport.Undo(); break;
    case Edit::kRedo: viewport.Redo(); break;
    case Edit::kCount: break;
  }
}

struct WrapPair {
  TextViewport plain;
  TextViewport wrapped;

  void Load(const std::string& content) {
    plain.LoadContent(content, "/tmp/wrap-invariance-plain.cpp");
    plain.SetViewportSize(40, 400);
    plain.SetSoftWrap(false);
    wrapped.LoadContent(content, "/tmp/wrap-invariance-wrapped.cpp");
    wrapped.SetViewportSize(6, 9);
    wrapped.SetSoftWrap(true);
  }

  void Both(const std::function<void(TextViewport&)>& action) {
    action(plain);
    action(wrapped);
  }
};

void TestWrapDoesNotChangeWhatAnEditDoes() {
  const std::vector<std::string> documents = {
      "alpha bravo charlie delta\n  indented line that is long enough to wrap\nzz\n// commented\n",
      "\tint total = 0;\n\tfor (int value : values) {\n\t\ttotal += value;\n\t}\n",
      "short\n" + std::string(40, 'x') + "\nshort again\n\n",
      "\xe4\xbd\xa0\xe5\xa5\xbd world and some more text to wrap\ncaf\xc3\xa9 latte\n",
  };

  std::mt19937 rng(20260914);
  auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };

  constexpr int kIterationsPerDocument = 300;
  std::size_t divergence_opportunities = 0;
  std::vector<int> ran(static_cast<std::size_t>(Edit::kCount), 0);
  std::vector<int> changed(static_cast<std::size_t>(Edit::kCount), 0);
  for (const std::string& content : documents) {
    for (int iteration = 0; iteration < kIterationsPerDocument; ++iteration) {
      WrapPair pair;
      pair.Load(content);
      Expect(pair.wrapped.visual_line_count() > pair.wrapped.line_count(),
             "the wrapped viewport must actually wrap, or this compares two identical "
             "views: " +
                 std::to_string(pair.wrapped.visual_line_count()) + " rows for " +
                 std::to_string(pair.wrapped.line_count()) + " lines");
      Expect(pair.plain.visual_line_count() == pair.plain.line_count(),
             "the plain viewport must not wrap");
      ++divergence_opportunities;

      const std::size_t lines = pair.plain.line_count();
      auto random_position = [&]() {
        const std::size_t line = pick(lines);
        return TextPosition{line, pick(pair.plain.lines().LineLength(line) + 1)};
      };
      const TextPosition anchor = random_position();
      pair.Both([&](TextViewport& v) { v.MoveCursorTo(anchor.line, anchor.column); });
      if (pick(2) == 0) {
        const TextPosition head = random_position();
        pair.Both([&](TextViewport& v) {
          v.MoveCursorTo(head.line, head.column, true);
        });
      }
      const std::size_t secondaries = pick(3);
      for (std::size_t s = 0; s < secondaries; ++s) {
        const TextPosition a = random_position();
        if (pick(2) == 0) {
          pair.Both([&](TextViewport& v) { v.AddSecondaryCaret(a.line, a.column); });
        } else {
          const TextPosition b = random_position();
          pair.Both([&](TextViewport& v) {
            v.AddSecondaryCaretWithRange(SelectionRange{a, b});
          });
        }
      }

      std::string trail;
      const int steps = 1 + static_cast<int>(pick(4));
      Edit previous = Edit::kCount;
      for (int step = 0; step < steps; ++step) {
        const Edit edit = (previous == Edit::kUndo && pick(3) == 0)
                              ? Edit::kRedo
                              : static_cast<Edit>(pick(static_cast<std::size_t>(Edit::kCount)));
        previous = edit;
        const char typed = static_cast<char>('a' + pick(26));
        trail += std::string(step == 0 ? "" : " -> ") + EditName(edit);
        const std::string before_text = DocumentText(pair.plain);
        const std::string before_carets = CaretSet(pair.plain);
        ApplyEdit(pair.plain, edit, typed);
        ApplyEdit(pair.wrapped, edit, typed);
        ++ran[static_cast<std::size_t>(edit)];
        if (DocumentText(pair.plain) != before_text || CaretSet(pair.plain) != before_carets) {
          ++changed[static_cast<std::size_t>(edit)];
        }

        const std::string plain_text = DocumentText(pair.plain);
        const std::string wrapped_text = DocumentText(pair.wrapped);
        Expect(plain_text == wrapped_text,
               "wrap changed the TEXT: " + trail + " over " + Quoted(content) + " gave " +
                   Quoted(wrapped_text) + " wrapped and " + Quoted(plain_text) + " plain");
        const std::string plain_carets = CaretSet(pair.plain);
        const std::string wrapped_carets = CaretSet(pair.wrapped);
        Expect(plain_carets == wrapped_carets,
               "wrap changed the CARET SET: " + trail + " over " + Quoted(content) +
                   " gave " + wrapped_carets + " wrapped and " + plain_carets + " plain");
        if (plain_text != wrapped_text || plain_carets != wrapped_carets) {
          return;
        }
      }
    }
  }
  Expect(divergence_opportunities ==
             documents.size() * static_cast<std::size_t>(kIterationsPerDocument),
         "every iteration must have run against a genuinely wrapped view");
  for (std::size_t i = 0; i < static_cast<std::size_t>(Edit::kCount); ++i) {
    const char* name = EditName(static_cast<Edit>(i));
    Expect(ran[i] > 20, std::string("the verb ") + name + " barely ran: " +
                            std::to_string(ran[i]) + " times");
    Expect(changed[i] > 0, std::string("the verb ") + name +
                               " never once moved the text or the caret set, so wrap "
                               "invariance was not tested for it");
  }
}

}

void RegisterEditorWrapInvarianceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorWrapInvariance/WrapDoesNotChangeWhatAnEditDoes",
          TestWrapDoesNotChangeWhatAnEditDoes);
}

}
