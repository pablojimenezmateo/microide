// Multi-caret editing against a reference model.
//
// The reference is VS Code's rule: cursors are normalised before an edit
// (overlapping selections merge; a collapsed caret touching a selection merges
// into it), then every cursor applies the same edit to its own range, highest
// first, and lands at the start of its range plus what it inserted. Random
// documents, random caret sets (collapsed carets and selections, same-line and
// cross-line), and the four edits every caret path shares: a typed character,
// Enter, Backspace, Delete. The alphabet has no whitespace, brackets or quotes so
// auto-indent, indent stops and auto-close pairs stay out of the reference.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextViewport.h"
#include "TestSupport.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

bool Less(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

bool LessEq(const TextPosition& a, const TextPosition& b) {
  return !Less(b, a);
}

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) {
      out.push_back('\n');
    }
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

struct RefSite {
  SelectionRange range;  // normalised; start == end for a collapsed caret
};

struct RefDoc {
  std::vector<std::string> lines;

  TextPosition Prev(TextPosition p) const {
    if (p.column > 0) return TextPosition{p.line, p.column - 1};
    if (p.line > 0) return TextPosition{p.line - 1, lines[p.line - 1].size()};
    return p;
  }
  TextPosition Next(TextPosition p) const {
    if (p.column < lines[p.line].size()) return TextPosition{p.line, p.column + 1};
    if (p.line + 1 < lines.size()) return TextPosition{p.line + 1, 0};
    return p;
  }
  // Replace [start, end) with `text` (which may contain '\n'); returns the
  // position just after the inserted text.
  TextPosition Replace(const SelectionRange& r, std::string_view text) {
    std::string head = lines[r.start.line].substr(0, r.start.column);
    std::string tail = lines[r.end.line].substr(r.end.column);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(r.start.line),
                lines.begin() + static_cast<std::ptrdiff_t>(r.end.line) + 1);
    std::vector<std::string> inserted;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        inserted.emplace_back(text.substr(begin, i - begin));
        begin = i + 1;
      }
    }
    TextPosition landed{r.start.line + inserted.size() - 1,
                        (inserted.size() == 1 ? head.size() : 0) + inserted.back().size()};
    inserted.front() = head + inserted.front();
    inserted.back() += tail;
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(r.start.line), inserted.begin(),
                 inserted.end());
    return landed;
  }
};

enum class Op { Type, Newline, Backspace, Delete };

const char* OpName(Op op) {
  switch (op) {
    case Op::Type: return "type";
    case Op::Newline: return "newline";
    case Op::Backspace: return "backspace";
    case Op::Delete: return "delete";
  }
  return "?";
}

// VS Code's CursorCollection.normalize: sorted by start; two cursors merge when
// their ranges overlap, or touch while one of them is collapsed.
std::vector<RefSite> Normalize(std::vector<RefSite> sites) {
  std::sort(sites.begin(), sites.end(), [](const RefSite& a, const RefSite& b) {
    if (Less(a.range.start, b.range.start)) return true;
    if (Less(b.range.start, a.range.start)) return false;
    return Less(a.range.end, b.range.end);
  });
  std::vector<RefSite> out;
  for (const RefSite& site : sites) {
    if (!out.empty()) {
      RefSite& prev = out.back();
      const bool prev_empty = prev.range.start == prev.range.end;
      const bool cur_empty = site.range.start == site.range.end;
      const bool merge = (prev_empty || cur_empty) ? LessEq(site.range.start, prev.range.end)
                                                   : Less(site.range.start, prev.range.end);
      if (merge) {
        if (Less(prev.range.end, site.range.end)) prev.range.end = site.range.end;
        continue;
      }
    }
    out.push_back(site);
  }
  return out;
}

// Apply one op at every site, highest first. Returns the collapsed carets.
std::vector<TextPosition> ReferenceApply(RefDoc& doc, std::vector<RefSite> sites, Op op, char ch) {
  sites = Normalize(sites);
  std::vector<TextPosition> carets(sites.size());
  for (std::size_t i = sites.size(); i-- > 0;) {
    SelectionRange r = sites[i].range;
    std::string text;
    const bool collapsed = r.start == r.end;
    switch (op) {
      case Op::Type: text.assign(1, ch); break;
      case Op::Newline: text = "\n"; break;
      case Op::Backspace:
        if (collapsed) r.start = doc.Prev(r.start);
        break;
      case Op::Delete:
        if (collapsed) r.end = doc.Next(r.end);
        break;
    }
    const TextPosition landed = doc.Replace(r, text);
    // Higher carets already landed in the pre-edit coordinates of this range;
    // fold the shift this edit produced into them.
    for (std::size_t k = i + 1; k < carets.size(); ++k) {
      TextPosition& c = carets[k];
      if (c.line == r.end.line) {
        c = TextPosition{landed.line, landed.column + (c.column - r.end.column)};
      } else {
        c.line = landed.line + (c.line - r.end.line);
      }
    }
    carets[i] = landed;
  }
  std::sort(carets.begin(), carets.end(), Less);
  carets.erase(std::unique(carets.begin(), carets.end(),
                           [](const TextPosition& a, const TextPosition& b) { return a == b; }),
               carets.end());
  return carets;
}

std::vector<RefSite> ViewportSites(const TextViewport& viewport) {
  std::vector<RefSite> sites;
  const TextPosition primary{viewport.cursor_line(), viewport.cursor_column()};
  sites.push_back(RefSite{viewport.selection_range().value_or(SelectionRange{primary, primary})});
  for (const auto& secondary : viewport.secondary_caret_ranges()) {
    SelectionRange r{secondary.position, secondary.position};
    if (secondary.selection_anchor.has_value() && !(*secondary.selection_anchor == r.start)) {
      r = Less(*secondary.selection_anchor, r.start)
              ? SelectionRange{*secondary.selection_anchor, r.start}
              : SelectionRange{r.start, *secondary.selection_anchor};
    }
    sites.push_back(RefSite{r});
  }
  return sites;
}

std::vector<TextPosition> ViewportCarets(const TextViewport& viewport) {
  std::vector<TextPosition> carets{TextPosition{viewport.cursor_line(), viewport.cursor_column()}};
  for (const TextPosition& p : viewport.secondary_carets()) carets.push_back(p);
  std::sort(carets.begin(), carets.end(), Less);
  carets.erase(std::unique(carets.begin(), carets.end(),
                           [](const TextPosition& a, const TextPosition& b) { return a == b; }),
               carets.end());
  return carets;
}

std::string Describe(const std::vector<TextPosition>& carets) {
  std::string out;
  for (const TextPosition& p : carets) {
    out += "(" + std::to_string(p.line) + "," + std::to_string(p.column) + ")";
  }
  return out;
}

std::string Describe(const std::vector<RefSite>& sites) {
  std::string out;
  for (const RefSite& s : sites) {
    out += "[" + std::to_string(s.range.start.line) + "," + std::to_string(s.range.start.column) +
           "-" + std::to_string(s.range.end.line) + "," + std::to_string(s.range.end.column) + "]";
  }
  return out;
}

std::string Quoted(std::string text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') out += "\\n";
    else out.push_back(c);
  }
  return out + "\"";
}


// Typing the character that is already selected is an exact no-op for the
// buffer (nothing is dirtied, no undo entry), but the selection still collapses
// past it, so "ab" typed over a selected "a" gives "ab" and not "b". Same for
// Enter over a selected line break, and for every caret of a multi-caret set.
void TestTypingTheSelectedTextCollapsesTheSelection() {
  {
    TextViewport viewport;
    viewport.SetViewportSize(20, 80);
    viewport.LoadContent("a\n", "/tmp/noop.txt");
    viewport.MoveCursorTo(0, 0);
    viewport.MoveCursorTo(0, 1, /*extend_selection=*/true);
    viewport.InsertCharacter('a');
    Expect(!viewport.selection_range().has_value(), "the selection collapsed");
    Expect(viewport.cursor_column() == 1, "the caret sits after the typed character");
    Expect(!viewport.Undo(), "an exact no-op recorded no undo entry");
    viewport.InsertCharacter('b');
    Expect(DocumentText(viewport) == "ab\n", "typing continues after the selected text: " +
                                                 DocumentText(viewport));
  }
  {
    TextViewport viewport;
    viewport.SetViewportSize(20, 80);
    viewport.LoadContent("x\ny\n", "/tmp/noop.txt");
    viewport.MoveCursorTo(1, 0);
    viewport.MoveCursorTo(0, 1, /*extend_selection=*/true);  // caret at the START
    viewport.InsertNewline();
    Expect(DocumentText(viewport) == "x\ny\n", "Enter over a selected line break changes nothing");
    Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 0,
           "the caret lands on the next line, whichever end it started at");
  }
  {
    TextViewport viewport;
    viewport.SetViewportSize(20, 80);
    viewport.LoadContent("a a\n", "/tmp/noop.txt");
    viewport.MoveCursorTo(0, 1);
    viewport.MoveCursorTo(0, 0, /*extend_selection=*/true);  // primary caret at the START
    viewport.AddSecondaryCaretWithRange(SelectionRange{{0, 2}, {0, 3}});
    viewport.InsertCharacter('a');
    Expect(DocumentText(viewport) == "a a\n", "both carets typed their own selection");
    const std::vector<TextPosition> carets = ViewportCarets(viewport);
    Expect(carets.size() == 2 && carets[0] == TextPosition{0, 1} && carets[1] == TextPosition{0, 3},
           "both carets collapsed past their selection: " + Describe(carets));
  }
}

void TestMultiCaretEditsAgreeWithReference() {
  std::mt19937 rng(20260905);
  constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
  auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };

  for (int iteration = 0; iteration < 2000; ++iteration) {
    // A random small document.
    RefDoc doc;
    const std::size_t line_count = 1 + pick(5);
    for (std::size_t i = 0; i < line_count; ++i) {
      std::string line;
      const std::size_t len = pick(7);
      for (std::size_t j = 0; j < len; ++j) line.push_back(kAlphabet[pick(kAlphabet.size())]);
      doc.lines.push_back(line);
    }
    std::string content;
    for (std::size_t i = 0; i < doc.lines.size(); ++i) {
      if (i > 0) content.push_back('\n');
      content += doc.lines[i];
    }

    TextViewport viewport;
    viewport.SetViewportSize(20, 80);
    viewport.LoadContent(content, "/tmp/multi-caret-reference.txt");

    auto random_position = [&]() {
      const std::size_t line = pick(doc.lines.size());
      return TextPosition{line, pick(doc.lines[line].size() + 1)};
    };

    // Primary, then 0-3 secondaries; each is a caret or a selection. The
    // viewport's own add/dedup policy decides what survives — the reference reads
    // the resulting set back, so it only judges the edit.
    const auto primary_a = random_position();
    const auto primary_b = pick(2) == 0 ? primary_a : random_position();
    viewport.MoveCursorTo(primary_a.line, primary_a.column);
    if (!(primary_b == primary_a)) {
      viewport.MoveCursorTo(primary_b.line, primary_b.column, /*extend_selection=*/true);
    }
    const std::size_t secondaries = pick(4);
    for (std::size_t s = 0; s < secondaries; ++s) {
      const auto a = random_position();
      if (pick(2) == 0) {
        viewport.AddSecondaryCaret(a.line, a.column);
      } else {
        viewport.AddSecondaryCaretWithRange(SelectionRange{a, random_position()});
      }
    }

    const int steps = 1 + static_cast<int>(pick(3));
    for (int step = 0; step < steps; ++step) {
      const std::vector<RefSite> sites = ViewportSites(viewport);
      const std::string before = DocumentText(viewport);
      const Op op = static_cast<Op>(pick(4));
      const char ch = kAlphabet[pick(kAlphabet.size())];

      RefDoc expected = doc;
      const std::vector<TextPosition> expected_carets = ReferenceApply(expected, sites, op, ch);

      switch (op) {
        case Op::Type: viewport.InsertCharacter(ch); break;
        case Op::Newline: viewport.InsertNewline(); break;
        case Op::Backspace: viewport.Backspace(); break;
        case Op::Delete: viewport.DeleteForward(); break;
      }

      std::string expected_text;
      for (std::size_t i = 0; i < expected.lines.size(); ++i) {
        if (i > 0) expected_text.push_back('\n');
        expected_text += expected.lines[i];
      }
      const std::string actual_text = DocumentText(viewport);
      const std::vector<TextPosition> actual_carets = ViewportCarets(viewport);
      const std::string context = std::string(OpName(op)) + " '" + ch + "' over " + Quoted(before) +
                                  " at " + Describe(sites);
      Expect(actual_text == expected_text, context + ": text " + Quoted(actual_text) +
                                               ", reference " + Quoted(expected_text));
      Expect(actual_carets == expected_carets,
             context + ": carets " + Describe(actual_carets) + ", reference " +
                 Describe(expected_carets));
      if (actual_text != expected_text || actual_carets != expected_carets) {
        return;
      }
      doc = expected;
    }
  }
}

}  // namespace

void RegisterTextViewportMultiCaretReferenceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewport/TypingTheSelectedTextCollapsesTheSelection",
          TestTypingTheSelectedTextCollapsesTheSelection);
  AddTest(tests, "TextViewport/MultiCaretEditsAgreeWithReference",
          TestMultiCaretEditsAgreeWithReference);
}

}  // namespace microide::tests
