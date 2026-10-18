#include "TestSupport.h"

#include <string>
#include <vector>

#include "editor/SmartSelect.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using editor::ExpandSelectionRange;
using editor::SelectionRange;
using editor::TextBuffer;
using editor::TextPosition;

// The text a range covers, '\n' between lines.
std::string Covered(const std::vector<std::string>& lines, const SelectionRange& range) {
  std::string out;
  for (std::size_t line = range.start.line; line <= range.end.line && line < lines.size(); ++line) {
    const std::size_t from = line == range.start.line ? range.start.column : 0;
    const std::size_t to = line == range.end.line ? range.end.column : lines[line].size();
    out += lines[line].substr(from, to - from);
    if (line != range.end.line) {
      out += '\n';
    }
  }
  return out;
}

// Expands from a caret `steps` times and returns what each step covered.
std::vector<std::string> Ladder(const std::vector<std::string>& lines, TextPosition caret, int steps) {
  const TextBuffer buffer(lines);
  SelectionRange current{caret, caret};
  std::vector<std::string> out;
  for (int i = 0; i < steps; ++i) {
    const auto next = ExpandSelectionRange(buffer, current);
    if (!next.has_value()) {
      out.push_back("<none>");
      break;
    }
    out.push_back(Covered(lines, *next));
    current = *next;
  }
  return out;
}

void ExpectLadder(const std::vector<std::string>& got, const std::vector<std::string>& want,
                  const std::string& what) {
  std::string described;
  for (const std::string& step : got) {
    described += "[" + step + "] ";
  }
  Expect(got == want, what + ": got " + described);
}

void TestWordThenBracketsThenLineThenDocument() {
  const std::vector<std::string> lines = {"int main() {", "  call(alpha, beta);", "}"};
  ExpectLadder(Ladder(lines, {1, 10}, 7),
               {"alpha", "alpha, beta", "(alpha, beta)", "call(alpha, beta)", "call(alpha, beta);",
                "  call(alpha, beta);\n", "\n  call(alpha, beta);\n"},
               "from inside alpha");
}

void TestStringContentThenString() {
  const std::vector<std::string> lines = {R"(say("hello world", x);)"};
  ExpectLadder(Ladder(lines, {0, 7}, 4),
               {"hello", "hello world", R"("hello world")", R"("hello world", x)"},
               "from inside a string");
}

void TestBracketsInsideStringsAreSkipped() {
  const std::vector<std::string> lines = {R"x(f(")", y);)x"};
  ExpectLadder(Ladder(lines, {0, 8}, 2), {"y", R"x(")", y)x"},
               "a ')' inside a string is not the closer");
}

void TestTheDocumentIsTheLastStep() {
  const std::vector<std::string> lines = {"a b"};
  const TextBuffer buffer(lines);
  const auto all = ExpandSelectionRange(buffer, {{0, 0}, {0, 3}});
  Expect(!all.has_value(), "the whole document has nothing larger");
}

void TestViewportShrinkWalksBackDownTheLadder() {
  editor::TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.LoadContent("x = f(alpha);", "/tmp/smart-select.txt");
  viewport.MoveCursorTo(0, 8);
  Expect(viewport.ExpandSelection() && viewport.SelectedText() == "alpha", "first step selects the word");
  Expect(viewport.ExpandSelection() && viewport.SelectedText() == "(alpha)", "second step the parentheses");
  Expect(viewport.ShrinkSelection() && viewport.SelectedText() == "alpha", "shrink returns to the word");
  Expect(viewport.ShrinkSelection() && viewport.SelectedText().empty() && viewport.cursor_column() == 8,
         "shrinking again returns to the bare caret where it started");
  Expect(!viewport.ShrinkSelection(), "with the ladder walked down there is nothing to shrink");
  Expect(viewport.ExpandSelection(), "expanding again starts a new ladder");
  viewport.MoveCursorTo(0, 0, true);  // the selection changes another way
  Expect(!viewport.ShrinkSelection(), "a selection changed another way cannot be shrunk");
}

}  // namespace

void RegisterSmartSelectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SmartSelect/WordThenBracketsThenLineThenDocument", TestWordThenBracketsThenLineThenDocument);
  AddTest(tests, "SmartSelect/StringContentThenString", TestStringContentThenString);
  AddTest(tests, "SmartSelect/BracketsInsideStringsAreSkipped", TestBracketsInsideStringsAreSkipped);
  AddTest(tests, "SmartSelect/TheDocumentIsTheLastStep", TestTheDocumentIsTheLastStep);
  AddTest(tests, "SmartSelect/ViewportShrinkWalksBackDownTheLadder", TestViewportShrinkWalksBackDownTheLadder);
}

}  // namespace microide::tests
