#include "TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextLayout;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

bool Less(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else out.push_back(c);
  }
  return out + "\"";
}

std::string Show(const TextPosition& p) {
  return "(" + std::to_string(p.line) + "," + std::to_string(p.column) + ")";
}

struct Cursor {
  TextPosition caret;
  std::optional<TextPosition> anchor;
  SelectionRange range;
  bool collapsed = true;
  bool primary = false;
};

std::vector<Cursor> Cursors(const TextViewport& viewport) {
  std::vector<Cursor> out;
  const auto push = [&](const TextPosition& caret, const std::optional<TextPosition>& anchor,
                        bool primary) {
    Cursor c;
    c.primary = primary;
    c.caret = caret;
    c.anchor = anchor;
    c.collapsed = !anchor.has_value() || *anchor == caret;
    c.range = c.collapsed ? SelectionRange{caret, caret}
                          : (Less(*anchor, caret) ? SelectionRange{*anchor, caret}
                                                  : SelectionRange{caret, *anchor});
    out.push_back(c);
  };
  const TextPosition primary{viewport.cursor_line(), viewport.cursor_column()};
  std::optional<TextPosition> primary_anchor;
  if (const std::optional<SelectionRange> sel = viewport.selection_range(); sel.has_value()) {
    primary_anchor = sel->start == primary ? sel->end : sel->start;
  }
  push(primary, primary_anchor, true);
  for (const auto& secondary : viewport.secondary_caret_ranges()) {
    push(secondary.position, secondary.selection_anchor, false);
  }
  std::sort(out.begin(), out.end(), [](const Cursor& a, const Cursor& b) {
    if (Less(a.range.start, b.range.start)) return true;
    if (Less(b.range.start, a.range.start)) return false;
    return Less(a.range.end, b.range.end);
  });
  return out;
}

std::string Describe(const std::vector<Cursor>& cursors) {
  std::string out;
  for (const Cursor& c : cursors) {
    out += "[" + Show(c.range.start) + "-" + Show(c.range.end) + (c.collapsed ? "" : (c.caret == c.range.start ? "<" : ">")) + (c.primary ? "*" : "") + "]";
  }
  return out;
}

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) out.push_back('\n');
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

bool PositionIsLegal(const TextViewport& viewport, const TextPosition& p) {
  if (p.line >= viewport.line_count()) return false;
  const std::string_view text = viewport.lines().LineView(p.line);
  if (p.column > text.size()) return false;
  return TextLayout::ClampTextColumn(text, p.column) == p.column;
}

enum class Motion {
  kLeft,
  kRight,
  kUp,
  kDown,
  kWordLeft,
  kWordRight,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
};

constexpr Motion kAllMotions[] = {
    Motion::kLeft,   Motion::kRight, Motion::kUp,     Motion::kDown,   Motion::kWordLeft,
    Motion::kWordRight, Motion::kHome, Motion::kEnd, Motion::kPageUp, Motion::kPageDown,
};

const char* MotionName(Motion motion) {
  switch (motion) {
    case Motion::kLeft: return "Left";
    case Motion::kRight: return "Right";
    case Motion::kUp: return "Up";
    case Motion::kDown: return "Down";
    case Motion::kWordLeft: return "WordLeft";
    case Motion::kWordRight: return "WordRight";
    case Motion::kHome: return "Home";
    case Motion::kEnd: return "End";
    case Motion::kPageUp: return "PageUp";
    case Motion::kPageDown: return "PageDown";
  }
  return "?";
}

enum class Shaping {
  kMoveLineUp,
  kMoveLineDown,
  kCopyLinesDown,
  kCopyLinesUp,
  kInsertLineBelow,
  kInsertLineAbove,
  kDeleteLine,
  kIndent,
  kOutdent,
  kJoinLines,
  kSortAscending,
  kToggleLineComment,
  kToggleBlockComment,
};

constexpr Shaping kAllShaping[] = {
    Shaping::kMoveLineUp,   Shaping::kMoveLineDown,      Shaping::kCopyLinesDown,
    Shaping::kCopyLinesUp,  Shaping::kInsertLineBelow,   Shaping::kInsertLineAbove,
    Shaping::kDeleteLine,   Shaping::kIndent,            Shaping::kOutdent,
    Shaping::kJoinLines,    Shaping::kSortAscending,     Shaping::kToggleLineComment,
    Shaping::kToggleBlockComment,
};

const char* ShapingName(Shaping shaping) {
  switch (shaping) {
    case Shaping::kMoveLineUp: return "MoveLineUp";
    case Shaping::kMoveLineDown: return "MoveLineDown";
    case Shaping::kCopyLinesDown: return "CopyLinesDown";
    case Shaping::kCopyLinesUp: return "CopyLinesUp";
    case Shaping::kInsertLineBelow: return "InsertLineBelow";
    case Shaping::kInsertLineAbove: return "InsertLineAbove";
    case Shaping::kDeleteLine: return "DeleteLine";
    case Shaping::kIndent: return "Indent";
    case Shaping::kOutdent: return "Outdent";
    case Shaping::kJoinLines: return "JoinLines";
    case Shaping::kSortAscending: return "SortAscending";
    case Shaping::kToggleLineComment: return "ToggleLineComment";
    case Shaping::kToggleBlockComment: return "ToggleBlockComment";
  }
  return "?";
}

void Apply(TextViewport& viewport, Shaping shaping) {
  switch (shaping) {
    case Shaping::kMoveLineUp: editor::MoveLineUp(viewport); break;
    case Shaping::kMoveLineDown: editor::MoveLineDown(viewport); break;
    case Shaping::kCopyLinesDown: editor::CopyLines(viewport, true); break;
    case Shaping::kCopyLinesUp: editor::CopyLines(viewport, false); break;
    case Shaping::kInsertLineBelow: editor::InsertLineBelow(viewport); break;
    case Shaping::kInsertLineAbove: editor::InsertLineAbove(viewport); break;
    case Shaping::kDeleteLine: editor::DeleteLine(viewport); break;
    case Shaping::kIndent: editor::IndentSelection(viewport); break;
    case Shaping::kOutdent: editor::OutdentSelection(viewport); break;
    case Shaping::kJoinLines: editor::JoinLinesAtCarets(viewport); break;
    case Shaping::kSortAscending: editor::SortLines(viewport, true); break;
    case Shaping::kToggleLineComment: editor::ToggleLineComment(viewport, "//"); break;
    case Shaping::kToggleBlockComment: editor::ToggleBlockComment(viewport, "/*", "*/"); break;
  }
}

void Apply(TextViewport& viewport, Motion motion, bool extend) {
  switch (motion) {
    case Motion::kLeft: viewport.MoveCursorHorizontal(-1, extend); break;
    case Motion::kRight: viewport.MoveCursorHorizontal(1, extend); break;
    case Motion::kUp: viewport.MoveCursorVertical(-1, extend); break;
    case Motion::kDown: viewport.MoveCursorVertical(1, extend); break;
    case Motion::kWordLeft: viewport.MoveCursorWord(-1, extend); break;
    case Motion::kWordRight: viewport.MoveCursorWord(1, extend); break;
    case Motion::kHome: viewport.MoveCursorLineStart(extend); break;
    case Motion::kEnd: viewport.MoveCursorLineEnd(extend); break;
    case Motion::kPageUp: viewport.Page(-1, extend); break;
    case Motion::kPageDown: viewport.Page(1, extend); break;
  }
}

struct Coverage {
  int multi_cursor_steps = 0;
  int selection_steps = 0;
  int merge_steps = 0;
  int shaping_steps = 0;
  int foreign_edit_steps = 0;
};

enum class StepKind { kMotion, kExtendingMotion, kEdit, kShaping, kHistory, kForeign };

void CheckWellFormed(const TextViewport& viewport,
                     const std::vector<Cursor>& before,
                     const std::string& text_before,
                     StepKind kind,
                     Coverage& coverage,
                     const std::string& context) {
  const bool extend = kind == StepKind::kExtendingMotion;
  const bool collapses = kind == StepKind::kMotion || kind == StepKind::kEdit;
  if (kind == StepKind::kMotion || kind == StepKind::kExtendingMotion) {
    Expect(DocumentText(viewport) == text_before, context + ": motion changed the document text");
  }

  const std::vector<Cursor> after = Cursors(viewport);
  if (kind == StepKind::kShaping) ++coverage.shaping_steps;
  if (kind == StepKind::kForeign) ++coverage.foreign_edit_steps;
  if (before.size() > 1) ++coverage.multi_cursor_steps;
  if (after.size() < before.size()) ++coverage.merge_steps;
  if (std::any_of(before.begin(), before.end(), [](const Cursor& c) { return !c.collapsed; })) {
    ++coverage.selection_steps;
  }

  const auto secondaries = viewport.secondary_caret_ranges();
  for (std::size_t i = 1; i < secondaries.size(); ++i) {
    Expect(Less(secondaries[i - 1].position, secondaries[i].position) ||
               secondaries[i - 1].position == secondaries[i].position,
           context + ": the secondary caret vector came back out of order");
  }
  const std::string set = " set " + Describe(after) + " (was " + Describe(before) + ")";

  Expect(!after.empty(), context + ": the caret set went empty");
  if (kind != StepKind::kHistory) {
    Expect(after.size() <= before.size(), context + ": the step GREW the caret set," + set);
  }

  for (const Cursor& c : after) {
    Expect(PositionIsLegal(viewport, c.caret),
           context + ": caret " + Show(c.caret) + " is out of bounds or mid-codepoint," + set);
    if (c.anchor.has_value()) {
      Expect(PositionIsLegal(viewport, *c.anchor),
             context + ": anchor " + Show(*c.anchor) + " is out of bounds or mid-codepoint," + set);
    }
    if (collapses) {
      Expect(c.collapsed, context + ": a plain motion or an edit left a selection behind," + set);
    }
  }

  for (std::size_t i = 1; i < after.size(); ++i) {
    const Cursor& prev = after[i - 1];
    const Cursor& cur = after[i];
    Expect(!(cur.caret == prev.caret && cur.anchor == prev.anchor),
           context + ": two identical cursors survived," + set);
    const bool touching_is_allowed = !prev.collapsed && !cur.collapsed;
    const bool overlaps = touching_is_allowed ? Less(cur.range.start, prev.range.end)
                                              : !Less(prev.range.end, cur.range.start);
    Expect(!overlaps, context + ": two cursors overlap," + set);
  }

  if (extend && after.size() == before.size()) {
    for (std::size_t i = 0; i < after.size(); ++i) {
      if (!after[i].anchor.has_value()) continue;
      const TextPosition expected =
          before[i].anchor.value_or(before[i].caret);
      Expect(*after[i].anchor == expected,
             context + ": a Shift motion moved an anchor from " + Show(expected) + " to " +
                 Show(*after[i].anchor) + "," + set);
    }
  }
}

const std::vector<std::string>& MotionDocuments() {
  static const std::vector<std::string> documents = {
      "alpha beta gamma\ndelta\n\nepsilon zeta\n",
      "\tindented\n\t\tdeeper\n  spaces  here\n\n",
      "caf\xc3\xa9 na\xc3\xafve\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e text\n\xf0\x9f\x98\x80 emoji\n",
      "one_very_long_line_with_no_break_opportunity_at_all_so_it_has_to_be_split_mid_word_by_the_wrapper\nshort\n",
      "\n\n\n",
      "a\n",
      "trailing spaces   \n   leading\n\tmixed \t tabs\t\n",
      "x = f(a, b);  // comment\ny = g(c);\nz = h();\n",
  };
  return documents;
}

void RunMotionSweep(bool soft_wrap) {
  std::mt19937 rng(soft_wrap ? 20260919u : 20260920u);
  Coverage coverage;
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };

  for (const std::string& content : MotionDocuments()) {
    for (int iteration = 0; iteration < 200; ++iteration) {
      TextViewport viewport;
      viewport.SetViewportSize(4, 24);
      viewport.SetSoftWrap(soft_wrap);
      viewport.LoadContent(content, "/tmp/multi-caret-motion.txt");

      auto random_position = [&]() {
        const std::size_t line = pick(viewport.line_count());
        const std::string_view text = viewport.lines().LineView(line);
        return TextPosition{line, TextLayout::ClampTextColumn(text, pick(text.size() + 1))};
      };

      const TextPosition primary = random_position();
      viewport.MoveCursorTo(primary.line, primary.column);
      if (pick(2) == 0) {
        const TextPosition to = random_position();
        viewport.MoveCursorTo(to.line, to.column, true);
      }
      if (pick(4) == 0) {
        viewport.SetBoxSelection(random_position(), random_position());
      } else {
        for (std::size_t s = 0, n = pick(4); s < n; ++s) {
          const TextPosition a = random_position();
          if (pick(2) == 0) {
            viewport.AddSecondaryCaret(a.line, a.column);
          } else {
            viewport.AddSecondaryCaretWithRange(SelectionRange{a, random_position()});
          }
        }
      }

      TextViewport sibling = viewport;
      sibling.SetViewportSize(4, 24);

      std::string trail;
      for (int step = 0; step < 12; ++step) {
        const std::vector<Cursor> before = Cursors(viewport);
        const std::string text_before = DocumentText(viewport);
        const std::size_t roll = pick(8);
        std::string context = soft_wrap ? "wrapped " : "unwrapped ";
        StepKind kind = StepKind::kMotion;
        if (roll == 0) {
          kind = StepKind::kEdit;
          context += "type";
          viewport.InsertCharacter('q');
        } else if (roll == 1) {
          kind = StepKind::kEdit;
          context += "backspace";
          viewport.Backspace();
        } else if (roll == 7) {
          kind = StepKind::kForeign;
          context += "foreign-edit";
          const std::size_t line = pick(sibling.line_count());
          sibling.MoveCursorTo(line, 0);
          if (pick(2) == 0 && sibling.line_count() > 1) {
            sibling.MoveCursorTo(std::min(line + 2, sibling.line_count() - 1), 0,
                                 true);
            sibling.Backspace();
          } else {
            sibling.InsertText("foreign\nedit\n");
          }
          const Motion motion = kAllMotions[pick(std::size(kAllMotions))];
          context += std::string(" then ") + MotionName(motion);
          Apply(viewport, motion, false);
        } else if (roll == 2) {
          kind = StepKind::kHistory;
          if (pick(2) == 0) {
            context += "undo";
            viewport.Undo();
          } else {
            context += "redo";
            viewport.Redo();
          }
        } else if (roll == 3) {
          kind = StepKind::kShaping;
          const Shaping shaping = kAllShaping[pick(std::size(kAllShaping))];
          context += ShapingName(shaping);
          Apply(viewport, shaping);
        } else {
          const Motion motion = kAllMotions[pick(std::size(kAllMotions))];
          const bool extend = pick(2) == 0;
          kind = extend ? StepKind::kExtendingMotion : StepKind::kMotion;
          context += std::string(extend ? "Shift+" : "") + MotionName(motion);
          Apply(viewport, motion, extend);
        }
        context += " step " + std::to_string(step);
        trail += (trail.empty() ? "" : " -> ") + context;
        CheckWellFormed(viewport, before, text_before, kind, coverage, trail + " || doc=" + Quoted(content));
      }
    }
  }
  const std::string tail = std::string(soft_wrap ? " (wrapped)" : " (unwrapped)") +
                           ": multi=" + std::to_string(coverage.multi_cursor_steps) +
                           " sel=" + std::to_string(coverage.selection_steps) +
                           " merge=" + std::to_string(coverage.merge_steps) +
                           " shaping=" + std::to_string(coverage.shaping_steps) +
                           " foreign=" + std::to_string(coverage.foreign_edit_steps);
  Expect(coverage.multi_cursor_steps > 1000, "the sweep barely built multi-cursor sets" + tail);
  Expect(coverage.selection_steps > 1000, "the sweep barely built selections" + tail);
  Expect(coverage.merge_steps > 200, "the sweep never merged two cursors" + tail);
  Expect(coverage.shaping_steps > 1200, "the sweep barely ran the line-shaping verbs" + tail);
  Expect(coverage.foreign_edit_steps > 400,
         "the sweep barely ran an edit from the sibling viewport" + tail);
}

void TestMultiCaretMotionAcrossACollapsedFold() {
  static constexpr std::string_view kSource =
      "head\n"
      "void f() {\n"
      "  aaa;\n"
      "  bbb;\n"
      "  if (x) {\n"
      "    ccc;\n"
      "  }\n"
      "}\n"
      "void g() {\n"
      "  ddd;\n"
      "}\n"
      "tail\n";

  std::mt19937 rng(20260921u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  Coverage coverage;
  int hidden_line_checks = 0;
  int fold_crossings = 0;

  for (int iteration = 0; iteration < 400; ++iteration) {
    TextViewport viewport;
    viewport.SetViewportSize(6, 20);
    viewport.SetSoftWrap(iteration % 2 == 0);
    viewport.LoadContent(std::string(kSource), "/tmp/multi-caret-fold.cpp");

    editor::FoldingModel folding;
    editor::FoldingModel::ComputeOptions options;
    options.bracket_pairs = {{'{', '}'}};
    options.use_indent_source = true;
    options.tab_size = 4;
    Expect(folding.Compute(viewport.lines().Snapshot(), options), "the fold fixture computes");
    Expect(folding.Collapse(1), "the outer function fold collapses");
    if (pick(2) == 0) {
      folding.Collapse(8);
    }
    viewport.SetFoldingModel(&folding);

    auto visible_position = [&]() {
      for (int attempt = 0; attempt < 16; ++attempt) {
        const std::size_t line = pick(viewport.line_count());
        if (folding.IsLineHidden(line)) continue;
        const std::string_view text = viewport.lines().LineView(line);
        return TextPosition{line, TextLayout::ClampTextColumn(text, pick(text.size() + 1))};
      }
      return TextPosition{0, 0};
    };

    const TextPosition primary = visible_position();
    viewport.MoveCursorTo(primary.line, primary.column);
    for (std::size_t s = 0, n = pick(4); s < n; ++s) {
      const TextPosition a = visible_position();
      if (pick(2) == 0) {
        viewport.AddSecondaryCaret(a.line, a.column);
      } else {
        viewport.AddSecondaryCaretWithRange(SelectionRange{a, visible_position()});
      }
    }

    for (int step = 0; step < 10; ++step) {
      const Motion motion = kAllMotions[pick(std::size(kAllMotions))];
      const bool extend = pick(2) == 0;
      const std::vector<Cursor> before = Cursors(viewport);
      const std::string text_before = DocumentText(viewport);
      const std::string context = std::string("folded ") + (extend ? "Shift+" : "") +
                                  MotionName(motion) + " step " + std::to_string(step);
      Apply(viewport, motion, extend);
      CheckWellFormed(viewport, before, text_before,
                      extend ? StepKind::kExtendingMotion : StepKind::kMotion, coverage, context);
      for (const Cursor& c : Cursors(viewport)) {
        ++hidden_line_checks;
        for (const Cursor& b : before) {
          if ((b.caret.line <= 1 && c.caret.line >= 7) ||
              (b.caret.line >= 7 && c.caret.line <= 1)) {
            ++fold_crossings;
          }
        }
        Expect(!folding.IsLineHidden(c.caret.line),
               context + ": a caret landed on the hidden line " + std::to_string(c.caret.line) +
                   ", set " + Describe(Cursors(viewport)));
      }
    }
  }
  Expect(coverage.multi_cursor_steps > 1000,
         "the folded sweep barely built multi-cursor sets: " +
             std::to_string(coverage.multi_cursor_steps));
  Expect(hidden_line_checks > 1000,
         "the folded sweep barely checked a caret: " + std::to_string(hidden_line_checks));
  Expect(fold_crossings > 400, "no caret ever walked over the collapsed block: " +
                                  std::to_string(fold_crossings));
}

void TestExtendingVerticalMotionMergesABoxSelection() {
  TextViewport viewport;
  viewport.SetViewportSize(40, 200);
  std::string content;
  for (int line = 0; line < 40; ++line) {
    content += "alpha bravo charlie delta\n";
  }
  viewport.LoadContent(content, "/tmp/box-merge.txt");
  viewport.SetBoxSelection(TextPosition{2, 4}, TextPosition{33, 12});
  Expect(Cursors(viewport).size() == 32,
         "the box should place one cursor per spanned line, got " +
             std::to_string(Cursors(viewport).size()));

  viewport.MoveCursorVertical(1, true);
  const std::vector<Cursor> after = Cursors(viewport);
  Expect(after.size() == 1, "one Shift+Down over the box should leave one cursor, got " +
                                std::to_string(after.size()) + ": " + Describe(after));
  Expect(after[0].range.start == (TextPosition{2, 4}) &&
             after[0].range.end == (TextPosition{34, 12}),
         "the merged cursor should span the union of what the box covered, got " +
             Describe(after));

  viewport.SetBoxSelection(TextPosition{2, 4}, TextPosition{33, 12});
  viewport.MoveCursorVertical(1);
  Expect(Cursors(viewport).size() == 32,
         "a plain Down should keep one cursor per line, got " +
             std::to_string(Cursors(viewport).size()));
}

void TestMultiCaretPairInsertKeepsTheSetWellFormed() {
  auto contract = std::make_shared<editor::LanguageContractView>();
  contract->auto_close_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"}, {"\"", "\""}, {"'", "'"}};
  contract->surround_pairs = contract->auto_close_pairs;
  contract->indent_after_open_patterns = {"{", "(", "["};
  contract->dedent_on_close_chars = {"}", ")", "]"};
  contract->line_comment = "//";
  contract->auto_close_enabled = true;
  contract->surround_enabled = true;
  contract->smart_indent_enabled = true;

  static constexpr std::string_view kPairChars = "()[]{}\"'";
  const std::vector<std::string> documents = {
      "int f(int a) { return a; }\nvoid g() {}\nconst char* s = \"hi\";\n",
      "a b c\n(d) [e] {f}\n\n\"q\" 'r'\n",
      "\tif (x) {\n\t\ty();\n\t}\n",
  };

  std::mt19937 rng(20260922u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  Coverage coverage;
  int surround_steps = 0;

  for (const std::string& content : documents) {
    for (bool soft_wrap : {false, true}) {
      for (int iteration = 0; iteration < 250; ++iteration) {
        TextViewport viewport;
        viewport.SetViewportSize(6, 16);
        viewport.SetSoftWrap(soft_wrap);
        viewport.LoadContent(content, "/tmp/pair-sweep.cpp");
        viewport.SetLanguageContractView(contract);

        auto random_position = [&]() {
          const std::size_t line = pick(viewport.line_count());
          const std::string_view text = viewport.lines().LineView(line);
          return TextPosition{line, TextLayout::ClampTextColumn(text, pick(text.size() + 1))};
        };
        const TextPosition primary = random_position();
        viewport.MoveCursorTo(primary.line, primary.column);
        if (pick(2) == 0) {
          const TextPosition to = random_position();
          viewport.MoveCursorTo(to.line, to.column, true);
        }
        for (std::size_t s = 0, n = 1 + pick(3); s < n; ++s) {
          const TextPosition a = random_position();
          if (pick(2) == 0) {
            viewport.AddSecondaryCaret(a.line, a.column);
          } else {
            viewport.AddSecondaryCaretWithRange(SelectionRange{a, random_position()});
          }
        }

        for (int step = 0; step < 6; ++step) {
          const std::vector<Cursor> before = Cursors(viewport);
          const std::string text_before = DocumentText(viewport);
          if (std::any_of(before.begin(), before.end(),
                          [](const Cursor& c) { return !c.collapsed; })) {
            ++surround_steps;
          }
          const bool had_selection = std::any_of(
              before.begin(), before.end(), [](const Cursor& c) { return !c.collapsed; });
          const char ch = kPairChars[pick(kPairChars.size())];
          const std::string context = std::string(soft_wrap ? "wrapped " : "unwrapped ") +
                                      "type '" + std::string(1, ch) + "' step " +
                                      std::to_string(step);
          viewport.InsertCharacter(ch);
          CheckWellFormed(viewport, before, text_before, StepKind::kShaping, coverage, context);
          if (!had_selection) {
            for (const Cursor& c : Cursors(viewport)) {
              Expect(c.collapsed,
                     context + ": auto-close created a selection from a collapsed set, " +
                         Describe(Cursors(viewport)));
            }
          }
        }
      }
    }
  }
  Expect(coverage.multi_cursor_steps > 2000,
         "the pair sweep barely built multi-cursor sets: " +
             std::to_string(coverage.multi_cursor_steps));
  Expect(surround_steps > 500,
         "the pair sweep barely typed over a selection, so surround went unswept: " +
             std::to_string(surround_steps));
}

void TestMultiCaretMotionKeepsTheSetWellFormed() { RunMotionSweep(false); }

void TestMultiCaretMotionKeepsTheSetWellFormedWhenWrapped() { RunMotionSweep(true); }

}

void RegisterEditorMultiCaretMotionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorMultiCaretMotion/PairInsertKeepsTheSetWellFormed",
          TestMultiCaretPairInsertKeepsTheSetWellFormed);
  AddTest(tests, "EditorMultiCaretMotion/KeepsTheSetWellFormed",
          TestMultiCaretMotionKeepsTheSetWellFormed);
  AddTest(tests, "EditorMultiCaretMotion/KeepsTheSetWellFormedWhenWrapped",
          TestMultiCaretMotionKeepsTheSetWellFormedWhenWrapped);
  AddTest(tests, "EditorMultiCaretMotion/ExtendingVerticalMotionMergesABoxSelection",
          TestExtendingVerticalMotionMergesABoxSelection);
  AddTest(tests, "EditorMultiCaretMotion/AcrossACollapsedFold",
          TestMultiCaretMotionAcrossACollapsedFold);
}

}
