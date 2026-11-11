#include "TestSupport.h"

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/DiffSyntax.h"
#include "compare/HunkRefinement.h"
#include "compare/StructuralDiff.h"

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
  cache.BeginBuild();
  (void)cache.Refine(a_lines, b_lines, CSyntax(), &stats);
  (void)cache.Refine(a_lines, b_lines, CSyntax(), &stats);
  Expect(stats.cached_hunks == 1, "the same hunk with the same rules should hit");
  (void)cache.Refine(a_lines, b_lines, PythonSyntax(), &stats);
  Expect(stats.cached_hunks == 1, "the same hunk read with other rules must miss");
  (void)cache.Refine(a_lines, c_lines, CSyntax(), &stats);
  Expect(stats.cached_hunks == 1, "a different hunk must miss");
  cache.EndBuild();
  Expect(cache.size() == 3, "a build keeps the entries it used");
  cache.BeginBuild();
  (void)cache.Refine(a_lines, b_lines, CSyntax(), &stats);
  cache.EndBuild();
  Expect(stats.cached_hunks == 2 && cache.size() == 1,
         "a build drops the entries it did not use");
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

}  // namespace

void RegisterStructuralDiffTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
