#include "TestSupport.h"

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/DiffSyntax.h"
#include "compare/CompareMoves.h"
#include "compare/HunkRefinement.h"
#include "compare/StructuralDiff.h"
#include "util/StringUtil.h"

// The structural (difftastic-style) diff: lexing into a syntax forest, the
// structural match, and what the compare model shows from it.
namespace microide::tests {
namespace {

using compare::BuildCompareModel;
using compare::CompareBuildOptions;
using compare::CompareModel;
using compare::CompareRow;
using compare::CompareRowKind;
using compare::CompareSideChange;
using compare::DiffContentInterner;
using compare::DiffSyntax;
using compare::DiffSyntaxTree;
using compare::DiffTokenKind;
using compare::kNoDiffNode;
using compare::StructuralMatch;

std::shared_ptr<const DiffSyntax> CSyntax() {
  static const std::shared_ptr<const DiffSyntax> kSyntax = [] {
    auto syntax = std::make_shared<DiffSyntax>();
    syntax->line_comments = {"//"};
    syntax->block_comments = {{"/*", "*/"}};
    syntax->delimiters = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
    return std::shared_ptr<const DiffSyntax>(std::move(syntax));
  }();
  return kSyntax;
}

std::shared_ptr<const DiffSyntax> PythonSyntax() {
  static const std::shared_ptr<const DiffSyntax> kSyntax = [] {
    auto syntax = std::make_shared<DiffSyntax>();
    syntax->line_comments = {"#"};
    syntax->delimiters = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
    return std::shared_ptr<const DiffSyntax>(std::move(syntax));
  }();
  return kSyntax;
}

CompareModel Build(const std::string& left, const std::string& right,
                   const std::shared_ptr<const DiffSyntax>& syntax = CSyntax()) {
  CompareBuildOptions options;
  options.syntax = syntax;
  return BuildCompareModel(left, right, options);
}

// Lines of `text` as views into it (terminators excluded), like the builder's.
std::vector<std::string_view> Lines(const std::string& text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.emplace_back(text.data() + start, text.size() - start);
      break;
    }
    lines.emplace_back(text.data() + start, end - start);
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> Tokens(const std::string& text,
                                const std::shared_ptr<const DiffSyntax>& syntax = CSyntax()) {
  const std::vector<std::string_view> lines = Lines(text);
  DiffContentInterner interner;
  DiffSyntaxTree tree;
  compare::BuildDiffSyntaxTree(lines, *syntax, interner, tree);
  std::vector<std::string> out;
  for (const auto& token : tree.tokens) {
    out.emplace_back(token.text);
  }
  return out;
}

// The novel text of one side of a row, spans joined by '|'.
std::string Novel(std::string_view text, const std::vector<compare::CompareTextSpan>& spans) {
  std::string out;
  for (const auto& span : spans) {
    if (!out.empty()) {
      out += '|';
    }
    out.append(text.substr(span.start, span.end - span.start));
  }
  return out;
}

const CompareRow* FindRight(const CompareModel& model, std::string_view right_text) {
  for (const auto& row : model.rows) {
    if (row.right_line != 0 && row.right_text == right_text) {
      return &row;
    }
  }
  return nullptr;
}

const CompareRow* FindLeft(const CompareModel& model, std::string_view left_text) {
  for (const auto& row : model.rows) {
    if (row.left_line != 0 && row.left_text == left_text) {
      return &row;
    }
  }
  return nullptr;
}

void TestLexerReadsCommentsStringsAndOperators() {
  const auto tokens = Tokens("a->b += \"x \\\" y\" /* c */ f(1, 2); // tail");
  const std::vector<std::string> expected = {"a",        "->", "b", "+=", "\"x \\\" y\"",
                                             "/* c */",  "f",  "(", "1",  ",",
                                             "2",        ")",  ";", "// tail"};
  Expect(tokens == expected, "lexer should keep strings, comments and operator runs whole");
}

void TestLexerApostropheInWordIsNotAQuote() {
  const auto tokens = Tokens("don't stop 'q'", PythonSyntax());
  Expect(std::find(tokens.begin(), tokens.end(), "'q'") != tokens.end(),
         "a quote after a non-word byte should open a string");
  Expect(std::find(tokens.begin(), tokens.end(), "don") != tokens.end() &&
             std::find(tokens.begin(), tokens.end(), "t") != tokens.end(),
         "an apostrophe inside a word should not open a string");
}

void TestLexerSpansLinesForBlockCommentsAndTripleQuotes() {
  const auto comment = Tokens("x /* one\n two */ y");
  Expect(comment.size() == 3 && comment[1] == "/* one\n two */",
         "a block comment should span the lines it covers");
  const auto triple = Tokens("s = \"\"\"a\nb\"\"\"\nt", PythonSyntax());
  Expect(triple.size() == 4 && triple[2] == "\"\"\"a\nb\"\"\"",
         "a triple-quoted string should span lines");
}

void TestUnbalancedDelimitersBecomePunctuation() {
  const std::string text = "if (x) {\n  y(";
  const std::vector<std::string_view> lines = Lines(text);
  DiffContentInterner interner;
  DiffSyntaxTree tree;
  compare::BuildDiffSyntaxTree(lines, *CSyntax(), interner, tree);
  std::size_t lists = 0;
  for (const auto& node : tree.nodes) {
    lists += node.is_list ? 1 : 0;
  }
  Expect(lists == 1, "only the balanced (x) should form a list");
  for (const auto& token : tree.tokens) {
    if (token.text == "{" || (token.text == "(" && token.line == 1)) {
      Expect(token.kind == DiffTokenKind::Punctuation,
             "an unclosed delimiter should be read as punctuation");
    }
  }
}

void TestContentIdsIgnoreLayout() {
  const std::string a = "f(a, b)";
  const std::string b = "f(\n  a,\n  b\n)";
  const std::vector<std::string_view> a_lines = Lines(a);
  const std::vector<std::string_view> b_lines = Lines(b);
  DiffContentInterner interner;
  DiffSyntaxTree left;
  DiffSyntaxTree right;
  compare::BuildDiffSyntaxTree(a_lines, *CSyntax(), interner, left);
  compare::BuildDiffSyntaxTree(b_lines, *CSyntax(), interner, right);
  Expect(left.nodes.size() == right.nodes.size(), "same shape on both sides");
  for (std::size_t index = 0; index < left.nodes.size(); ++index) {
    Expect(left.nodes[index].content_id == right.nodes[index].content_id,
           "re-wrapped subtrees should intern to the same content id");
  }
}

// The headline behaviour: wrapping code in a new block highlights only the new
// block's head and closing brace; the re-indented body is a layout change.
void TestWrappingInABlockMarksOnlyTheNewDelimiters() {
  const auto model = Build(
      "void f() {\n"
      "  step_one(a);\n"
      "  step_two(b);\n"
      "}\n",
      "void f() {\n"
      "  if (ready) {\n"
      "    step_one(a);\n"
      "    step_two(b);\n"
      "  }\n"
      "}\n");
  const CompareRow* head = FindRight(model, "  if (ready) {");
  Expect(head != nullptr && Novel(head->right_text, head->right_changed_spans) == "if (ready) {",
         "the new block head should be novel");
  const CompareRow* close = FindRight(model, "  }");
  Expect(close != nullptr && Novel(close->right_text, close->right_changed_spans) == "}",
         "the new closing brace should be novel");
  for (const char* body : {"    step_one(a);", "    step_two(b);"}) {
    const CompareRow* row = FindRight(model, body);
    Expect(row != nullptr && row->kind == CompareRowKind::Modified,
           "the re-indented body should pair with its old line");
    Expect(compare::LeftSideChange(*row) == CompareSideChange::LayoutOnly &&
               compare::RightSideChange(*row) == CompareSideChange::LayoutOnly,
           "the re-indented body should be a layout-only change");
  }
}

void TestRewrappedCallHasNoNovelTokens() {
  const auto model = Build("call(first, second, third);\n",
                           "call(\n    first,\n    second,\n    third);\n");
  Expect(model.hunks.size() == 1, "the re-wrap is still a textual change git sees");
  for (const auto& row : model.rows) {
    Expect(row.left_changed_spans.empty() && row.right_changed_spans.empty(),
           "a re-wrapped call should carry no novel token");
  }
  const CompareRow* first = FindLeft(model, "call(first, second, third);");
  Expect(first != nullptr && first->kind == CompareRowKind::Modified,
         "the one-line call should sit beside its re-wrapped head");
}

void TestChangedArgumentIsTheOnlyNovelToken() {
  const auto model = Build("total = compute(alpha, beta, gamma);\n",
                           "total = compute(alpha, delta, gamma);\n");
  const CompareRow& row = model.rows.front();
  Expect(Novel(row.left_text, row.left_changed_spans) == "beta" &&
             Novel(row.right_text, row.right_changed_spans) == "delta",
         "only the replaced argument should be novel");
}

void TestEditedCommentMarksOnlyChangedWords() {
  const auto model = Build("x = 1; // the quick brown fox\n", "x = 1; // the slow brown fox\n");
  const CompareRow& row = model.rows.front();
  Expect(Novel(row.left_text, row.left_changed_spans) == "quick" &&
             Novel(row.right_text, row.right_changed_spans) == "slow",
         "an edited comment should be diffed word by word");
}

void TestReindentedCommentIsLayoutOnly() {
  const auto model = Build("/* a long\n   comment */\nx;\n", "  /* a long\n     comment */\nx;\n");
  for (const auto& row : model.rows) {
    Expect(row.left_changed_spans.empty() && row.right_changed_spans.empty(),
           "a re-indented multi-line comment should carry no novel word");
  }
}

void TestFlatFallbackMatchesEqualTokensInOrder() {
  // Two lists a side with nothing equal at the top: the comparison cannot be
  // split, so it reaches the graph search -- which a one-vertex budget stops.
  const std::string a = "(a b) (c d)";
  const std::string b = "(a x) (y d)";
  const std::vector<std::string_view> a_lines = Lines(a);
  const std::vector<std::string_view> b_lines = Lines(b);
  DiffContentInterner interner;
  DiffSyntaxTree left;
  DiffSyntaxTree right;
  compare::BuildDiffSyntaxTree(a_lines, *CSyntax(), interner, left);
  compare::BuildDiffSyntaxTree(b_lines, *CSyntax(), interner, right);
  StructuralMatch match;
  compare::StructuralDiffStats stats;
  compare::MatchSyntaxTrees(left, right, 1, match, &stats);
  Expect(stats.fell_back, "a one-vertex budget must fall back");
  // Tokens: ( a b ) ( c d )  vs  ( a x ) ( y d ).
  for (const std::uint32_t index : {0u, 1u, 3u, 4u, 6u, 7u}) {
    Expect(match.lhs_partner[index] == index, "the flat diff should match the shared tokens");
  }
  Expect(match.lhs_partner[2] == kNoDiffNode && match.lhs_partner[5] == kNoDiffNode,
         "the flat diff should leave the replaced tokens novel");
}

// A gap of bare atoms is matched without a graph, and the answer is the LCS.
void TestAtomOnlyGapNeedsNoSearch() {
  const std::string a = "a b c d e";
  const std::string b = "a x c y e";
  const std::vector<std::string_view> a_lines = Lines(a);
  const std::vector<std::string_view> b_lines = Lines(b);
  DiffContentInterner interner;
  DiffSyntaxTree left;
  DiffSyntaxTree right;
  compare::BuildDiffSyntaxTree(a_lines, *CSyntax(), interner, left);
  compare::BuildDiffSyntaxTree(b_lines, *CSyntax(), interner, right);
  StructuralMatch match;
  compare::StructuralDiffStats stats;
  compare::MatchSyntaxTrees(left, right, compare::kStructuralDiffVertexBudget, match, &stats);
  Expect(stats.vertices == 0 && !stats.fell_back, "an atom-only gap should not be searched");
  Expect(match.lhs_partner[0] == 0 && match.lhs_partner[2] == 2 && match.lhs_partner[4] == 4 &&
             match.lhs_partner[1] == kNoDiffNode && match.lhs_partner[3] == kNoDiffNode,
         "an atom-only gap should match its longest common subsequence");
}

// Over random token soups: every match is symmetric, in order on both sides,
// and pairs equal texts (or two literals, for a replaced string/comment); and
// every hunk's rows cover each of its lines exactly once, in order.
void TestStructuralMatchInvariantsOnRandomInput() {
  std::mt19937 random(20261111);
  const std::vector<std::string> atoms = {"a", "b", "c", "f", "1", ",", ";", "+", "(", ")",
                                          "{", "}", "\"s\"", "\"t u\"", "// c", "\n", "  "};
  const auto generate = [&](std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
      text += atoms[random() % atoms.size()];
      text += ' ';
    }
    return text;
  };
  for (int round = 0; round < 200; ++round) {
    const std::string a = generate(5 + random() % 40);
    const std::string b = generate(5 + random() % 40);
    const std::vector<std::string_view> a_lines = Lines(a);
    const std::vector<std::string_view> b_lines = Lines(b);
    DiffContentInterner interner;
    DiffSyntaxTree left;
    DiffSyntaxTree right;
    compare::BuildDiffSyntaxTree(a_lines, *CSyntax(), interner, left);
    compare::BuildDiffSyntaxTree(b_lines, *CSyntax(), interner, right);
    StructuralMatch match;
    compare::MatchSyntaxTrees(left, right, compare::kStructuralDiffVertexBudget, match);
    std::uint32_t last_partner = 0;
    bool first = true;
    for (std::uint32_t index = 0; index < left.tokens.size(); ++index) {
      const std::uint32_t partner = match.lhs_partner[index];
      if (partner == kNoDiffNode) {
        continue;
      }
      Expect(match.rhs_partner[partner] == index, "matches must be symmetric");
      Expect(first || partner > last_partner, "matches must be in order on both sides");
      first = false;
      last_partner = partner;
      if (match.lhs_replaced[index] == 0) {
        Expect(left.tokens[index].text == right.tokens[partner].text,
               "an unchanged token must match an equal token");
      }
    }

    compare::HunkRefinement refinement;
    compare::RefineHunk(a_lines, b_lines, *CSyntax(), refinement);
    std::uint32_t next_left = 0;
    std::uint32_t next_right = 0;
    for (const compare::HunkRow& row : refinement.rows) {
      if (row.left != compare::kNoHunkLine) {
        Expect(row.left == next_left++, "rows must cover left lines once, in order");
      }
      if (row.right != compare::kNoHunkLine) {
        Expect(row.right == next_right++, "rows must cover right lines once, in order");
      }
    }
    Expect(next_left == a_lines.size() && next_right == b_lines.size(),
           "rows must cover every line of the hunk");
  }
}

// TD-2026-11-11-442: slider correction. Each of these has two placements of the
// novel tokens at the same cost; the reader expects one of them. The first three
// the search's tie-break already gets right -- the pass must keep them; the two
// after it the tie-break got wrong (found by probing random insertions with the
// pass on and off): novelty straddling a line break it does not need to.
void TestAppendedListItemIsTheNovelOne() {
  const auto model = Build("call(a, b);\n", "call(a, b, b);\n");
  const CompareRow& row = model.rows.front();
  Expect(Novel(row.right_text, row.right_changed_spans) == ", b",
         "the appended item and its separator are novel, not the middle item (got '" +
             Novel(row.right_text, row.right_changed_spans) + "')");
}

void TestNewBlockInsideABlockIsTheInnerPair() {
  const auto model = Build(
      "{\n"
      "  x;\n"
      "}\n",
      "{\n"
      "  {\n"
      "    x;\n"
      "  }\n"
      "}\n");
  const CompareRow* inner_open = FindRight(model, "  {");
  const CompareRow* inner_close = FindRight(model, "  }");
  Expect(inner_open != nullptr && Novel(inner_open->right_text, inner_open->right_changed_spans) == "{",
         "the inner, indented brace is the new one");
  Expect(inner_close != nullptr && Novel(inner_close->right_text, inner_close->right_changed_spans) == "}",
         "and its closing brace");
  for (const auto& row : model.rows) {
    if (row.right_text == "{" || row.right_text == "}") {
      Expect(row.right_changed_spans.empty(), "the outer pair, at column 0, is the old block");
    }
  }
}

void TestNewStatementBeforeAnEqualOneSlidesToWholeLines() {
  // Inserting `y = 1;` before an equal `y = 1;`: the novel run can be the first
  // or the second statement; whole lines read better than a run that starts
  // mid-line, and the later one wins a tie.
  const auto model = Build("f();\ny = 1;\ng();\n", "f();\ny = 1;\ny = 1;\ng();\n");
  int novel_rows = 0;
  for (const auto& row : model.rows) {
    if (!row.right_changed_spans.empty()) {
      ++novel_rows;
      Expect(Novel(row.right_text, row.right_changed_spans) == "y = 1;",
             "the novel run is one whole statement");
    }
  }
  Expect(novel_rows <= 1, "the inserted statement is novel on one line only");
}

void TestNovelRunDoesNotStraddleALineBreakItNeedNot() {
  // `)b();` inserted before an equal `)`: the search split it as `b();` at the
  // end of line 1 plus the `)` that starts line 2. Sliding over the equal `)`
  // keeps it on one line, which is what was typed.
  const auto model = Build("  )d, )[a]  if (x) {\n)\n", "  )d, )b();\n)[a]  if (x) {\n)\n");
  int novel_rows = 0;
  for (const auto& row : model.rows) {
    if (!row.right_changed_spans.empty()) {
      ++novel_rows;
      Expect(Novel(row.right_text, row.right_changed_spans) == ")b();",
             "the inserted tokens are one run at the end of line 1 (got '" +
                 Novel(row.right_text, row.right_changed_spans) + "')");
    }
  }
  Expect(novel_rows == 1, "the insertion is novel on one line");
}

void TestNovelCallSlidesOntoItsOwnLine() {
  // `f(a)}` inserted before an equal `f(a)`: novel `}` on line 1 and `f(a)` on
  // line 2 becomes `f(a)}`, the tail of line 1.
  const auto model = Build(")d, f(a)f(a))\n", ")d, f(a)}\nf(a)f(a))\n");
  const CompareRow* first = FindRight(model, ")d, f(a)}");
  const CompareRow* second = FindRight(model, "f(a)f(a))");
  Expect(first != nullptr && Novel(first->right_text, first->right_changed_spans) == "f(a)}",
         "the novel run is the tail of line 1");
  Expect(second != nullptr && second->right_changed_spans.empty(), "and line 2 is unchanged");
}

// TD-2026-11-11-441: raw strings, here-documents, and the state a region starts in.
std::shared_ptr<const DiffSyntax> WithRaw(const std::shared_ptr<const DiffSyntax>& base, std::uint8_t raw) {
  auto syntax = std::make_shared<DiffSyntax>(*base);
  syntax->raw_strings = raw;
  return std::shared_ptr<const DiffSyntax>(std::move(syntax));
}

std::shared_ptr<const DiffSyntax> CppRawSyntax() {
  static const auto kSyntax = WithRaw(CSyntax(), compare::kDiffRawCpp);
  return kSyntax;
}

std::shared_ptr<const DiffSyntax> RustSyntax() {
  static const auto kSyntax = WithRaw(CSyntax(), compare::kDiffRawRust);
  return kSyntax;
}

std::shared_ptr<const DiffSyntax> ShellSyntax() {
  static const auto kSyntax =
      WithRaw(PythonSyntax(), compare::kDiffHeredoc | compare::kDiffHeredocSpaced);
  return kSyntax;
}

void TestRawStringsAreOneToken() {
  Expect(Tokens("auto s = R\"x(a )\" b\n)y)x\";", CppRawSyntax()) ==
             std::vector<std::string>{"auto", "s", "=", "R\"x(a )\" b\n)y)x\"", ";"},
         "a C++ raw string runs to its own delimiter, across lines and quotes");
  Expect(Tokens("let s = r#\"has \"quote\"\"#;", RustSyntax()) ==
             std::vector<std::string>{"let", "s", "=", "r#\"has \"quote\"\"#", ";"},
         "a Rust raw string runs to its hashes");
  Expect(Tokens("let r#type = 1;", RustSyntax()) ==
             std::vector<std::string>{"let", "r", "#", "type", "=", "1", ";"},
         "a raw identifier is not a raw string");
  Expect(Tokens("fooR\"x\"", CppRawSyntax()) == std::vector<std::string>{"fooR", "\"x\""},
         "R in the middle of a word does not start a raw string");
  Expect(Tokens("cat <<'EOF' | sort\nb $x \"\nEOF\necho done", ShellSyntax()) ==
             std::vector<std::string>{"cat", "<<'EOF'", "|", "sort", "b $x \"\nEOF", "echo", "done"},
         "a here-document's body is one token, after the rest of its line");
  Expect(Tokens("x <<-END\n\tbody\n\tEND\ny", ShellSyntax()) ==
             std::vector<std::string>{"x", "<<-END", "\tbody\n\tEND", "y"},
         "<<- ends at the word after leading tabs");
  Expect(Tokens("echo $((1<<2))", ShellSyntax()) ==
             std::vector<std::string>{"echo", "$", "(", "(", "1", "<<", "2", ")", ")"},
         "a shift by a number is not a here-document");
}

void TestScanStateCarriesMultiLineTokens() {
  const auto state = [](const std::string& text, const std::shared_ptr<const DiffSyntax>& syntax) {
    const std::vector<std::string_view> lines = Lines(text);
    return compare::ScanDiffLexState(lines, lines.size(), *syntax, {});
  };
  using Kind = compare::DiffLexState::Kind;
  Expect(state("int a; /* opens\nstill", CSyntax()).kind == Kind::BlockComment, "inside a comment");
  Expect(state("int a; /* opens */ int b;", CSyntax()).code(), "a closed comment leaves code");
  Expect(state("s = \"/* not a comment\";", CSyntax()).code(), "a comment opener in a string");
  Expect(state("// /* not a comment", CSyntax()).code(), "a comment opener in a line comment");
  const std::string raw_text = "x = R\"tag(\nbody";
  const compare::DiffLexState raw = state(raw_text, CppRawSyntax());
  Expect(raw.kind == Kind::CppRawString && raw.delimiter == "tag", "inside a raw string, by its tag");
  Expect(state("s = '''\nbody", PythonSyntax()).kind == Kind::TripleString, "inside a triple quote");
  const std::string heredoc_text = "cat <<EOF";
  const compare::DiffLexState heredoc = state(heredoc_text, ShellSyntax());
  Expect(heredoc.kind == Kind::Heredoc && heredoc.delimiter == "EOF",
         "a here-document marker on the last line: its body starts the next region");
}

// The mechanism the refinement relies on: a region lexed from the state the
// scan reports for the text above it yields the tokens a lex of the whole text
// yields from that line on (the remainder of a token crossing into it first).
void TestRegionLexedFromScannedStateAgreesWithAWholeLex() {
  std::mt19937 random(20261113);
  const std::vector<std::string> pieces = {
      "a", "b1", "(", ")", "{", "}", ";", ",", "+", "/*", "*/", "//", "\"", "\"\"\"", "'",
      "`", "R\"(", ")\"", "R\"t(", ")t\"", "r#\"", "\"#", "<<EOF", "<<-EOF", "EOF", "\tEOF",
      "\\", "#", "  ", "\n", "\n", "\n"};
  const std::vector<std::shared_ptr<const DiffSyntax>> syntaxes = {
      CSyntax(), CppRawSyntax(), RustSyntax(),
      WithRaw(CSyntax(), compare::kDiffHeredoc | compare::kDiffHeredocSpaced)};
  const auto same_class = [](DiffTokenKind kind) {
    return kind == DiffTokenKind::Open || kind == DiffTokenKind::Close ? DiffTokenKind::Punctuation
                                                                       : kind;
  };
  for (int round = 0; round < 400; ++round) {
    std::string text;
    const int count = 4 + static_cast<int>(random() % 40);
    for (int index = 0; index < count; ++index) {
      text += pieces[random() % pieces.size()];
      if (random() % 3 == 0) {
        text += ' ';
      }
    }
    const DiffSyntax& syntax = *syntaxes[static_cast<std::size_t>(round) % syntaxes.size()];
    const std::vector<std::string_view> lines = Lines(text);
    DiffContentInterner interner;
    DiffSyntaxTree whole;
    compare::BuildDiffSyntaxTree(lines, syntax, interner, whole);
    for (std::size_t split = 1; split < lines.size(); ++split) {
      const auto below = std::span<const std::string_view>(lines).subspan(split);
      const compare::DiffLexState entry = compare::ScanDiffLexState(lines, split, syntax, {});
      DiffContentInterner region_interner;  // entries point into their tree
      DiffSyntaxTree region;
      compare::BuildDiffSyntaxTree(below, syntax, region_interner, region, entry);
      std::vector<std::string> expected;
      std::vector<std::string> actual;
      for (const auto& token : whole.tokens) {
        if (token.end_line < split) {
          continue;
        }
        const char* begin = token.line >= split ? token.text.data() : below.front().data();
        const std::string_view rest(
            begin, static_cast<std::size_t>(token.text.data() + token.text.size() - begin));
        if (rest.empty()) {
          continue;
        }
        expected.push_back(std::to_string(static_cast<int>(same_class(token.kind))) + ":" +
                           std::string(rest));
      }
      for (const auto& token : region.tokens) {
        actual.push_back(std::to_string(static_cast<int>(same_class(token.kind))) + ":" +
                         std::string(token.text));
      }
      Expect(expected == actual, "a region lexed from the scanned state must agree with the whole "
                                 "lex (round " + std::to_string(round) + ", split " +
                                 std::to_string(split) + "):\n" + text);
      if (expected != actual) {
        return;
      }
    }
  }
}

void TestEntryStateIsPartOfTheRefinementKey() {
  // The same hunk text, once inside a comment and once not: two refinements.
  CompareBuildOptions options;
  options.syntax = CSyntax();
  CompareModel model;
  (void)compare::BuildCompareModelInto(model, "/*\nkeep\nold words\nend\n", "/*\nkeep\nnew words\nend\n",
                                       options);
  const auto other_state = compare::BuildCompareModelInto(model, "x;\nkeep\nold words\nend\n",
                                                          "x;\nkeep\nnew words\nend\n", options);
  Expect(other_state.cached_hunks == 0,
         "a hunk whose first line is in another lexical state is not served from the cache");
  const auto same_state = compare::BuildCompareModelInto(model, "x;\nkeep\nold words\nend\n",
                                                         "x;\nkeep\nnew words\nend\n", options);
  Expect(same_state.cached_hunks == 1, "the same text in the same state is");
}

// TD-2026-11-11-443: code moved from one hunk to another.
std::string MiddleLines() {
  std::string text;
  for (int line = 0; line < 12; ++line) {
    text += "keep_" + std::to_string(line) + "();\n";
  }
  return text;
}

const std::string kHelper =
    "void helper() {\n"
    "  do_work(alpha, beta);\n"
    "  finish(gamma);\n"
    "}\n";

void TestMovedBlockIsMarkedOnBothSides() {
  const auto model = Build(kHelper + MiddleLines(), MiddleLines() + kHelper);
  int left_moved = 0;
  int right_moved = 0;
  for (const auto& row : model.rows) {
    left_moved += row.left_moved ? 1 : 0;
    right_moved += row.right_moved ? 1 : 0;
    if (row.left_moved) {
      Expect(compare::LeftSideChange(row) == CompareSideChange::Moved, "a moved line reads as moved");
    }
  }
  Expect(left_moved == 4 && right_moved == 4,
         "the four moved lines are marked where they left and where they landed (" +
             std::to_string(left_moved) + "/" + std::to_string(right_moved) + ")");
}

void TestReindentedMoveIsStillAMove() {
  std::string indented;
  for (const std::string_view line : Lines(kHelper)) {
    if (!line.empty()) {
      indented += "    " + std::string(line) + "\n";
    }
  }
  const auto model = Build(kHelper + MiddleLines(), MiddleLines() + "namespace n {\n" + indented + "}\n");
  int moved = 0;
  for (const auto& row : model.rows) {
    moved += row.right_moved ? 1 : 0;
  }
  Expect(moved == 4, "indentation does not stop a move (" + std::to_string(moved) + ")");
}

void TestTrivialOrEditedLinesAreNotMoves() {
  // A lone brace reappearing elsewhere carries too little text to be a move.
  const auto braces = Build("}\n" + MiddleLines(), MiddleLines() + "}\n");
  for (const auto& row : braces.rows) {
    Expect(!row.left_moved && !row.right_moved, "a lone brace is not moved code");
  }
  // Edited on the way: not the same lines.
  std::string edited = kHelper;
  edited.replace(edited.find("beta"), 4, "delta");
  const auto changed = Build(kHelper + MiddleLines(), MiddleLines() + edited);
  int moved = 0;
  for (const auto& row : changed.rows) {
    moved += row.right_moved ? 1 : 0;
  }
  Expect(moved == 0, "the runs around the edit are each under 20 alphanumerics: no move (" +
                         std::to_string(moved) + ")");
}

// The hunk rule, on rows built by hand: the same deleted and added run is a move
// across hunks and not within one (there the structural match already paired them).
void TestMovesAreBetweenHunksOnly() {
  const auto run = [](int deleted_hunk, int added_hunk) {
    CompareModel model;
    const char* lines[] = {"  do_work(alpha, beta);", "  finish(gamma, delta);"};
    for (int index = 0; index < 2; ++index) {
      CompareRow row;
      row.kind = CompareRowKind::Deleted;
      row.left_text = lines[index];
      row.left_line = index + 1;
      row.hunk = deleted_hunk;
      row.left_changed_spans.push_back({0, row.left_text.size()});
      model.rows.push_back(row);
    }
    for (int index = 0; index < 2; ++index) {
      CompareRow row;
      row.kind = CompareRowKind::Added;
      row.right_text = lines[index];
      row.right_line = index + 1;
      row.hunk = added_hunk;
      row.right_changed_spans.push_back({0, row.right_text.size()});
      model.rows.push_back(row);
    }
    compare::MarkMovedLines(model);
    return model.rows[0].left_moved && model.rows[1].left_moved && model.rows[2].right_moved &&
           model.rows[3].right_moved;
  };
  Expect(run(0, 1), "a run deleted in one hunk and added in another is moved");
  Expect(!run(0, 0), "the same run inside one hunk is not");
}

// An in-place rebuild refines only the hunks that changed, and its rows are the
// ones a fresh build produces.
void TestRebuildRefinesOnlyChangedHunks() {
  std::string left;
  std::string right;
  for (int block = 0; block < 6; ++block) {
    const std::string id = std::to_string(block);
    left += "keep_" + id + "();\nvalue_" + id + " = f(a, b);\n";
    right += "keep_" + id + "();\nvalue_" + id + " = f(a, c);\n";
  }
  CompareBuildOptions options;
  options.syntax = CSyntax();
  CompareModel model;
  const auto first = compare::BuildCompareModelProfiled(left, right, options);
  compare::BuildCompareModelInto(model, left, right, options);
  Expect(model.hunks.size() == 6, "fixture should have six hunks");

  std::string edited = right;
  edited.replace(edited.find("value_3 = f(a, c);"), 18, "value_3 = f(a, d);");
  compare::BuildCompareModelInto(model, left, edited, options);
  const auto fresh = compare::BuildCompareModelProfiled(left, edited, options);
  Expect(fresh.profile.cached_hunks == 0 && first.profile.cached_hunks == 0,
         "a fresh model has nothing cached");
  Expect(model.rows.size() == fresh.model.rows.size(), "a cached rebuild must keep the rows");
  for (std::size_t index = 0; index < model.rows.size(); ++index) {
    const CompareRow& a = model.rows[index];
    const CompareRow& b = fresh.model.rows[index];
    Expect(a.kind == b.kind && a.left_line == b.left_line && a.right_line == b.right_line &&
               Novel(a.left_text, a.left_changed_spans) ==
                   Novel(b.left_text, b.left_changed_spans) &&
               Novel(a.right_text, a.right_changed_spans) ==
                   Novel(b.right_text, b.right_changed_spans),
           "a cached rebuild must match a fresh build row for row");
  }

  // The next keystroke-like rebuild: five hunks unchanged, one edited.
  edited.replace(edited.find("value_3 = f(a, d);"), 18, "value_3 = f(a, e);");
  const auto profile = compare::BuildCompareModelInto(model, left, edited, options);
  Expect(profile.cached_hunks == 5, "a rebuild should reuse the five unchanged hunks");
  Expect(profile.structural_hunks + profile.flat_fallback_hunks == 1,
         "a rebuild should refine only the edited hunk");
}

// The cache answers only for the same text read with the same rules, and keeps
// only what the last build used.
void TestCacheIsKeyedOnTextAndSyntax() {
  const std::string a = "x = f(a, b);";
  const std::string b = "x = f(a, c);";
  const std::string c = "x = f(a, cc);";
  const std::vector<std::string_view> a_lines = Lines(a);
  const std::vector<std::string_view> b_lines = Lines(b);
  const std::vector<std::string_view> c_lines = Lines(c);
  compare::HunkRefinementCache cache;
  compare::HunkRefinementStats stats;
  cache.BeginBuild(CSyntax());
  (void)cache.Refine(a_lines, b_lines, &stats);
  (void)cache.Refine(a_lines, c_lines, &stats);
  cache.EndBuild();
  Expect(stats.cached_hunks == 0 && cache.size() == 2, "a first build refines every hunk");

  cache.BeginBuild(CSyntax());
  const compare::HunkRefinementView view = cache.Refine(a_lines, b_lines, &stats);
  Expect(stats.cached_hunks == 1, "the same hunk with the same rules should hit");
  Expect(view.rows().size() == 1 && view.LeftSpans(0).size() == 1 &&
             Novel(a_lines[0], {view.LeftSpans(0).begin(), view.LeftSpans(0).end()}) == "b",
         "a hit should reproduce the refinement");
  cache.EndBuild();
  Expect(cache.size() == 1, "a build keeps only the entries it used");

  cache.BeginBuild(PythonSyntax());
  (void)cache.Refine(a_lines, b_lines, &stats);
  cache.EndBuild();
  Expect(stats.cached_hunks == 1, "the same hunk read with other rules must miss");
}

void TestRefinementCanBeTurnedOff() {
  CompareBuildOptions options;
  options.refine_hunks = false;
  const auto model = BuildCompareModel("a(b)\n", "a(c)\n", options);
  Expect(model.hunks.size() == 1 && model.rows.front().kind == CompareRowKind::Modified,
         "hunks are the line diff's whatever the refinement");
  Expect(model.rows.front().left_changed_spans.empty(),
         "an unrefined build should carry no spans");
}

// Soak 1b3aae58e0b7 / 5226857eacc9 / 5fb20806dcf1 (the structural oracle, on diffs
// the monkey opened over lines of emoji): novel spans that ended inside a
// character -- the view highlights half a code point, and every byte column
// after it that a caret or a selection reads from the span is off. Every span a
// refinement reports starts and ends on a UTF-8 boundary, whatever the text.
void TestNovelSpansNeverSplitACharacter() {
  const std::string units[] = {"a", "x", " ", "(", ")", "\"", "//", "\xF0\x9F\x98\x80", "\xE4\xB8\xAD",
                               "e\xCC\x81", "\xC3\xA9", std::string("\x80", 1), "\xFF", "\t", "foo", "1"};
  std::mt19937 rng(7);
  const auto line = [&](std::size_t length) {
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
      out += units[rng() % std::size(units)];
    }
    return out;
  };
  const auto on_boundary = [](std::string_view text, std::size_t at) {
    return at == 0 || at >= text.size() || (static_cast<unsigned char>(text[at]) & 0xC0u) != 0x80u ||
           // A stray continuation byte is a unit of its own (the editor's tiling):
           // a span may begin or end at one only where no lead byte claims it.
           util::Utf8SequenceLength(text, [&] {
             std::size_t lead = at;
             while (lead > 0 && at - lead < 3 && (static_cast<unsigned char>(text[lead]) & 0xC0u) == 0x80u) {
               --lead;
             }
             return lead;
           }()) <= at - [&] {
             std::size_t lead = at;
             while (lead > 0 && at - lead < 3 && (static_cast<unsigned char>(text[lead]) & 0xC0u) == 0x80u) {
               --lead;
             }
             return lead;
           }();
  };
  for (int round = 0; round < 300; ++round) {
    std::string left;
    std::string right;
    for (int l = 0; l < 6; ++l) {
      const std::string base = line(1 + rng() % 40);
      left += base + "\n";
      right += (rng() % 3 == 0 ? line(1 + rng() % 70) : base + line(rng() % 4)) + "\n";
    }
    const CompareModel model = BuildCompareModel(left, right);
    for (const CompareRow& row : model.rows) {
      for (const auto& [text, spans] : {std::pair{row.left_text, &row.left_changed_spans},
                                        std::pair{row.right_text, &row.right_changed_spans}}) {
        for (const auto& span : *spans) {
          Expect(span.start < span.end && span.end <= text.size(),
                 "a novel span lies inside its line");
          Expect(on_boundary(text, span.start) && on_boundary(text, span.end),
                 "a novel span never starts or ends inside a character (round " + std::to_string(round) + ")");
        }
      }
    }
  }
}

}  // namespace

void RegisterStructuralDiffTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StructuralDiff/NovelSpansNeverSplitACharacter", TestNovelSpansNeverSplitACharacter);
  AddTest(tests, "StructuralDiff/LexerReadsCommentsStringsAndOperators",
          TestLexerReadsCommentsStringsAndOperators);
  AddTest(tests, "StructuralDiff/LexerApostropheInWordIsNotAQuote",
          TestLexerApostropheInWordIsNotAQuote);
  AddTest(tests, "StructuralDiff/LexerSpansLinesForBlockCommentsAndTripleQuotes",
          TestLexerSpansLinesForBlockCommentsAndTripleQuotes);
  AddTest(tests, "StructuralDiff/UnbalancedDelimitersBecomePunctuation",
          TestUnbalancedDelimitersBecomePunctuation);
  AddTest(tests, "StructuralDiff/ContentIdsIgnoreLayout", TestContentIdsIgnoreLayout);
  AddTest(tests, "StructuralDiff/WrappingInABlockMarksOnlyTheNewDelimiters",
          TestWrappingInABlockMarksOnlyTheNewDelimiters);
  AddTest(tests, "StructuralDiff/RewrappedCallHasNoNovelTokens",
          TestRewrappedCallHasNoNovelTokens);
  AddTest(tests, "StructuralDiff/ChangedArgumentIsTheOnlyNovelToken",
          TestChangedArgumentIsTheOnlyNovelToken);
  AddTest(tests, "StructuralDiff/EditedCommentMarksOnlyChangedWords",
          TestEditedCommentMarksOnlyChangedWords);
  AddTest(tests, "StructuralDiff/ReindentedCommentIsLayoutOnly",
          TestReindentedCommentIsLayoutOnly);
  AddTest(tests, "StructuralDiff/FlatFallbackMatchesEqualTokensInOrder",
          TestFlatFallbackMatchesEqualTokensInOrder);
  AddTest(tests, "StructuralDiff/AtomOnlyGapNeedsNoSearch", TestAtomOnlyGapNeedsNoSearch);
  AddTest(tests, "StructuralDiff/MatchInvariantsOnRandomInput",
          TestStructuralMatchInvariantsOnRandomInput);
  AddTest(tests, "StructuralDiff/RefinementCanBeTurnedOff", TestRefinementCanBeTurnedOff);
  AddTest(tests, "StructuralDiff/RebuildRefinesOnlyChangedHunks",
          TestRebuildRefinesOnlyChangedHunks);
  AddTest(tests, "StructuralDiff/CacheIsKeyedOnTextAndSyntax", TestCacheIsKeyedOnTextAndSyntax);
  AddTest(tests, "StructuralDiff/AppendedListItemIsTheNovelOne", TestAppendedListItemIsTheNovelOne);
  AddTest(tests, "StructuralDiff/NewBlockInsideABlockIsTheInnerPair",
          TestNewBlockInsideABlockIsTheInnerPair);
  AddTest(tests, "StructuralDiff/NewStatementBeforeAnEqualOneSlidesToWholeLines",
          TestNewStatementBeforeAnEqualOneSlidesToWholeLines);
  AddTest(tests, "StructuralDiff/NovelRunDoesNotStraddleALineBreakItNeedNot",
          TestNovelRunDoesNotStraddleALineBreakItNeedNot);
  AddTest(tests, "StructuralDiff/NovelCallSlidesOntoItsOwnLine", TestNovelCallSlidesOntoItsOwnLine);
  AddTest(tests, "StructuralDiff/RawStringsAreOneToken", TestRawStringsAreOneToken);
  AddTest(tests, "StructuralDiff/ScanStateCarriesMultiLineTokens", TestScanStateCarriesMultiLineTokens);
  AddTest(tests, "StructuralDiff/RegionLexedFromScannedStateAgreesWithAWholeLex",
          TestRegionLexedFromScannedStateAgreesWithAWholeLex);
  AddTest(tests, "StructuralDiff/EntryStateIsPartOfTheRefinementKey",
          TestEntryStateIsPartOfTheRefinementKey);
  AddTest(tests, "StructuralDiff/MovedBlockIsMarkedOnBothSides", TestMovedBlockIsMarkedOnBothSides);
  AddTest(tests, "StructuralDiff/ReindentedMoveIsStillAMove", TestReindentedMoveIsStillAMove);
  AddTest(tests, "StructuralDiff/TrivialOrEditedLinesAreNotMoves", TestTrivialOrEditedLinesAreNotMoves);
  AddTest(tests, "StructuralDiff/MovesAreBetweenHunksOnly", TestMovesAreBetweenHunksOnly);
}

}  // namespace microide::tests
