#include "TestSupport.h"

#include "FoldingReference.h"
#include "editor/FoldingModel.h"
#include "editor/TextBuffer.h"
#include "editor/TextViewport.h"
#include "util/PerformanceCounters.h"

#include <random>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::FoldRange;
using microide::editor::FoldSource;
using microide::editor::LineEditSpan;
using microide::editor::TextViewport;

FoldingModel::ComputeOptions DefaultCStyleOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}, {'(', ')'}, {'[', ']'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

FoldRange Find(const std::vector<FoldRange>& ranges, std::size_t opener_line) {
  for (const auto& r : ranges) {
    if (r.opener_line == opener_line) return r;
  }
  return FoldRange{};
}

std::string DescribeRanges(const std::vector<FoldRange>& ranges) {
  std::string out;
  for (const FoldRange& range : ranges) {
    out += "(" + std::to_string(range.opener_line) + "," + std::to_string(range.closer_line) +
           (range.source == FoldSource::Bracket ? ",B) " : ",I) ");
  }
  return out;
}

void ExpectMatchesReference(const FoldingModel& model, const std::vector<std::string>& lines,
                            const FoldingModel::ComputeOptions& options,
                            const std::string& context) {
  const std::vector<FoldRange> reference = ReferenceFolds(lines, options);
  std::vector<FoldRange> expected;
  for (const FoldRange& range : reference) {
    if (range.opener_line >= model.resolved_first_line() &&
        range.opener_line <= model.resolved_last_line()) {
      expected.push_back(range);
    }
  }
  std::vector<FoldRange> actual;
  for (const FoldRange& range : model.resolved_ranges()) {
    if (range.opener_line >= model.resolved_first_line() &&
        range.opener_line <= model.resolved_last_line()) {
      actual.push_back(range);
    }
  }
  const bool same =
      expected.size() == actual.size() &&
      std::equal(expected.begin(), expected.end(), actual.begin(),
                 [](const FoldRange& a, const FoldRange& b) {
                   return a.opener_line == b.opener_line && a.closer_line == b.closer_line &&
                          a.source == b.source;
                 });
  Expect(same, context + ": model folds must match the reference computation.\n  expected: " +
                   DescribeRanges(expected) + "\n  actual:   " + DescribeRanges(actual));
}

void TestFoldStaleWithoutMarkDirtyAfterSameLineCountEdit() {
  std::vector<std::string> lines = {
      "void f() {",
      "  if (x) {",
      "    a;",
      "  }",
      "  b;",
      "}",
  };
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "initial resolve should finish");
  Expect(Find(model.resolved_ranges(), 1).closer_line == 3,
         "structure A: the inner bracket fold opens at line 1 and closes at line 3");

  lines = {
      "void f() {",
      "  b;",
      "  if (x) {",
      "    a;",
      "  }",
      "}",
  };
  Expect(model.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, LineEditSpan{}),
         "resolve returns after the same-line-count content change");
  Expect(Find(model.resolved_ranges(), 1).closer_line == 3,
         "without a reported edit the fold model is STALE: it still reports the pre-edit fold");

  model.MarkDirty();
  Expect(model.Compute(lines, options), "resolve after MarkDirty should finish");
  Expect(Find(model.resolved_ranges(), 1).closer_line == 0,
         "after the rescan the stale inner fold at line 1 is gone");
  Expect(Find(model.resolved_ranges(), 2).closer_line == 4,
         "after the rescan the inner fold reflects structure B (opens at 2, closes at 4)");
}

void TestAnEditSpanTheDocumentContradictsRebuildsTheIndentCache() {
  std::vector<std::string> lines;
  for (int i = 0; i < 10; ++i) {
    lines.push_back(i % 3 == 0 ? "block {" : "  item;");
  }
  FoldingModel model;
  const FoldingModel::ComputeOptions options;
  Expect(model.Compute(lines, options), "the first resolve finishes");

  lines.resize(5);
  LineEditSpan span;
  span.NoteSplice(6, 4, 1);
  Expect(model.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, span),
         "a span the document contradicts is a rebuild, not a crash");
  model.MarkDirty();
  FoldingModel fresh;
  Expect(fresh.Compute(lines, options) && model.Compute(lines, options),
         "both resolve the shrunk document");
  Expect(model.resolved_ranges().size() == fresh.resolved_ranges().size(),
         "and the rebuilt model agrees with a fresh one");
}

void TestBracketFoldEmitsOpenerAndCloser() {
  const std::vector<std::string> lines = {
      "void f() {",
      "  if (x) {",
      "    return 1;",
      "  }",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "compute should complete on small input");
  const auto& ranges = model.resolved_ranges();
  Expect(ranges.size() == 2,
         "should produce two bracket fold ranges (outer + inner)");
  const auto outer = Find(ranges, 0);
  Expect(outer.closer_line == 4 && outer.source == FoldSource::Bracket,
         "outer { should fold to its closer at line 4");
  const auto inner = Find(ranges, 1);
  Expect(inner.closer_line == 3 && inner.source == FoldSource::Bracket,
         "inner { should fold to its closer at line 3");
  ExpectMatchesReference(model, lines, DefaultCStyleOptions(), "nested braces");
}

void TestInnermostFoldContainingPicksDeepestOpener() {
  const std::vector<std::string> lines = {
      "void f() {",
      "  if (x) {",
      "    return 1;",
      "  }",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");
  auto inner = model.InnermostFoldContaining(2);
  Expect(inner.has_value() && inner->opener_line == 1 && inner->closer_line == 3,
         "body line should sit in the inner bracket fold");
  inner = model.InnermostFoldContaining(0);
  Expect(inner.has_value() && inner->opener_line == 0,
         "caret on outer opener should pick the outer-most range that still contains the line");
}

void TestIndentFoldFallsBackWhenNoBrackets() {
  const std::vector<std::string> lines = {
      "if cond:",
      "    print('hi')",
      "    print('there')",
      "next_thing()",
  };
  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.bracket_pairs = {};
  FoldingModel model;
  Expect(model.Compute(lines, options),
         "indent compute should complete on small input");
  Expect(model.resolved_ranges().size() == 1,
         "should emit a single indent-source fold for the indented block");
  Expect(model.resolved_ranges()[0].opener_line == 0 &&
             model.resolved_ranges()[0].closer_line == 2 &&
             model.resolved_ranges()[0].source == FoldSource::Indent,
         "indent-source fold should span the indented block");
  ExpectMatchesReference(model, lines, options, "indent-only block");
}

void TestIndentFoldsEmittedWhenScanWindowReachesBudget() {
  std::vector<std::string> lines;
  lines.reserve(2100);
  lines.push_back("if cond:");
  lines.push_back("    body_one()");
  lines.push_back("    body_two()");
  lines.push_back("next_thing()");
  for (int i = 0; i < 2096; ++i) {
    lines.push_back("filler()");
  }

  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.bracket_pairs = {};

  FoldingModel model;
  Expect(model.Refresh(lines, options, 0, 20, 2000),
         "visible-range resolve should finish");
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == 2 && fold->source == FoldSource::Indent,
         "indent fold near the top must be emitted even on a file past the budget");
}

void TestBracketWinsOnDuplicateOpenerLine() {
  const std::vector<std::string> lines = {
      "if (a) {",
      "  body;",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "mixed compute should complete");
  Expect(model.resolved_ranges().size() == 1,
         "duplicate opener should be deduplicated to a single range");
  Expect(model.resolved_ranges()[0].source == FoldSource::Bracket,
         "bracket fold should win over indent fold on the same opener line");
}

void TestBracketFoldSkipsStringAndCommentRegions() {
  TextViewport viewport;
  viewport.LoadContent(
      "void f() {\n"
      "  body();\n"
      "}\n"
      "const char* s = \"{\";\n"
      "// }\n",
      "/tmp/fold-syntax.cpp");
  for (std::size_t line = 0; line < 5; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }

  FoldingModel model;
  Expect(model.Refresh(viewport.lines().Snapshot(), DefaultCStyleOptions(), 0,
                       FoldingModel::kAllLines, 0, LineEditSpan::FullRebuild(),
                       &viewport),
         "syntax-aware fold compute should complete");
  Expect(model.resolved_ranges().size() == 1,
         "string/comment brackets should not create or interfere with fold ranges");
  const auto function_fold = Find(model.resolved_ranges(), 0);
  Expect(function_fold.closer_line == 2 && function_fold.source == FoldSource::Bracket,
         "only the real function body braces should define the fold");
}

void TestBudgetedRefreshFlagsPartial() {
  std::vector<std::string> lines;
  lines.reserve(4000);
  lines.push_back("void f() {");
  for (int i = 0; i < 3998; ++i) {
    lines.push_back("  step();");
  }
  lines.push_back("}");
  FoldingModel model;
  Expect(!model.Refresh(lines, DefaultCStyleOptions(), 0, 10,
                        10),
         "a ten-line budget cannot resolve a fold closing 4000 lines below");
  Expect(!model.complete(), "partial resolve should report complete()==false");
  Expect(!model.IsWindowResolved(0, 10),
         "a partial resolve must not claim its window, or the gate never comes back");
}

void TestBudgetedRefreshConvergesGeometrically() {
  std::vector<std::string> lines;
  lines.reserve(40002);
  lines.push_back("namespace n {");
  for (int i = 0; i < 40000; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");

  lines[5] = "  void inner() {";
  lines[6000] = "  }";

  FoldingModel model;
  const auto options = DefaultCStyleOptions();
  int refreshes = 0;
  while (!model.Refresh(lines, options, 0, 40,
                        2000, LineEditSpan{})) {
    ++refreshes;
    Expect(refreshes < 40, "budgeted refresh must converge");
  }
  Expect(refreshes <= 8,
         "a doubling budget must converge in O(log n) refreshes, not O(n / budget)");
  const auto fold = model.FoldStartingAt(5);
  Expect(fold.has_value() && fold->closer_line == 6000,
         "the resolve that completes must carry the folds inside its reach");
}

void TestForwardResolveCapDefersAVeryLongFold() {
  const std::size_t body = FoldingModel::kMaxForwardResolveLines * 2;
  std::vector<std::string> lines;
  lines.reserve(body + 2);
  lines.push_back("namespace n {");
  for (std::size_t i = 0; i < body; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");

  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Refresh(lines, options, 0, 40, 0,
                       LineEditSpan{}),
         "hitting the forward cap is an answer, not a partial resolve");
  Expect(!model.FoldStartingAt(0).has_value(),
         "a fold longer than the forward cap must not be chased from the top of the file");
  Expect(model.IsWindowResolved(0, 40),
         "and the gate must not keep asking for a window the cap already answered");

  const std::size_t near_closer = lines.size() - 40;
  Expect(model.Refresh(lines, options, near_closer, lines.size() - 1, 0,
                       LineEditSpan{}),
         "a window near the closer should resolve");
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == lines.size() - 1,
         "once the closer is within reach the fold resolves as an ancestor of the window");
}

void TestToggleFoldHidesInteriorLines() {
  const std::vector<std::string> lines = {
      "void f() {",
      "  body1;",
      "  body2;",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "compute should complete for toggle fixture");
  Expect(model.ToggleFold(0),
         "toggling the outer { should succeed");
  Expect(model.IsLineHidden(1) && model.IsLineHidden(2) && model.IsLineHidden(3),
         "collapsed folds should hide every line after the opener through the closer");
  Expect(!model.IsLineHidden(0),
         "opener line itself should not be hidden");
}

void TestIncrementalRefreshMatchesReference() {
  std::vector<std::string> lines;
  for (int block = 0; block < 12; ++block) {
    lines.push_back("void f" + std::to_string(block) + "() {");
    lines.push_back("  if (x) {");
    lines.push_back("    body();");
    lines.push_back("  }");
    lines.push_back("}");
  }

  FoldingModel incremental;
  const auto options = DefaultCStyleOptions();
  Expect(incremental.Compute(lines, options), "seed compute should complete");
  Expect(incremental.Collapse(0), "outer fold should accept collapse");

  const std::vector<std::pair<std::size_t, std::string>> edits = {
      {42, "    body(); more();"},
      {42, "    body();"},
      {14, "}  // f2"},
      {12, "    body(); x();"},
      {44, "}  // f8"},
      {22, "    body(); y();"},
      {22, "    body();"},
      {24, "}  // f4"},
      {57, "  if (z) {"},
  };

  for (const auto& [line, text] : edits) {
    lines[line] = text;
    LineEditSpan edited;
    edited.NoteSplice(line, 1, 1);
    incremental.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, edited);
    ExpectMatchesReference(incremental, lines, options,
                           "after in-place edit at line " + std::to_string(line));
  }
  Expect(incremental.IsCollapsedAtOpener(0),
         "the collapsed fold must survive edits that do not touch it");
}

void TestRandomisedSplicesMatchReference() {
  std::mt19937 rng(20260804);
  std::vector<std::string> lines;
  for (int block = 0; block < 200; ++block) {
    lines.push_back("void f" + std::to_string(block) + "() {");
    lines.push_back("  if (x) {");
    lines.push_back("    body();");
    lines.push_back("  }");
    lines.push_back("");
    lines.push_back("}");
  }

  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "seed compute should complete");

  const std::vector<std::string> insertable = {
      "void extra() {", "  while (y) {", "    step();", "  }", "}", "// comment", "", "  }",
  };
  for (int step = 0; step < 60; ++step) {
    const std::size_t start =
        std::uniform_int_distribution<std::size_t>(0, lines.size() - 1)(rng);
    const std::size_t removed = std::uniform_int_distribution<std::size_t>(
        0, std::min<std::size_t>(3, lines.size() - start))(rng);
    const std::size_t inserted = std::uniform_int_distribution<std::size_t>(0, 3)(rng);
    std::vector<std::string> replacement;
    for (std::size_t i = 0; i < inserted; ++i) {
      replacement.push_back(
          insertable[std::uniform_int_distribution<std::size_t>(0, insertable.size() - 1)(rng)]);
    }
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
                lines.begin() + static_cast<std::ptrdiff_t>(start + removed));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start), replacement.begin(),
                 replacement.end());
    if (lines.empty()) {
      lines.push_back("}");
    }

    LineEditSpan span;
    span.NoteSplice(start, removed, inserted);
    model.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, span);
    ExpectMatchesReference(model, lines, options, "randomised splice step " + std::to_string(step));
  }
}

void TestDeletingLinesAboveCachedBlocksKeepsTheirEventsInStep() {
  std::vector<std::string> lines;
  for (int function = 0; lines.size() < 2200; ++function) {
    lines.push_back("void f" + std::to_string(function) + "() {");
    lines.push_back("  if (x) {");
    lines.push_back("    body(a[0], (b));");
    lines.push_back("  }");
    lines.push_back("}");
  }
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "the seed resolve covers the whole document");

  const std::size_t start = 10;
  const std::size_t removed = 200;
  lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
              lines.begin() + static_cast<std::ptrdiff_t>(start + removed));
  LineEditSpan span;
  span.NoteSplice(start, removed, 0);
  Expect(model.Refresh(lines, options, 0, 40, 0, span),
         "the resolve after the deletion completes");
  ExpectMatchesReference(model, lines, options, "at the top, after deleting 200 lines there");

  const std::size_t top = lines.size() - 60;
  Expect(model.Refresh(lines, options, top, lines.size() - 1, 0, LineEditSpan{}),
         "the resolve at the end of the document completes");
  ExpectMatchesReference(model, lines, options, "in the last block, after deleting 200 lines above it");

  lines[top + 7] = "  while (y) { step(); }";
  LineEditSpan edited;
  edited.NoteSplice(top + 7, 1, 1);
  Expect(model.Refresh(lines, options, top, lines.size() - 1, 0, edited),
         "the resolve after an edit in the last block completes");
  ExpectMatchesReference(model, lines, options, "after an edit in the last block");
}

void TestWindowedResolveMatchesWholeDocumentInsideItsWindow() {
  std::vector<std::string> lines;
  lines.push_back("namespace outer {");
  for (int block = 0; block < 400; ++block) {
    lines.push_back("  void f" + std::to_string(block) + "() {");
    lines.push_back("    if (x) {");
    lines.push_back("      body();");
    lines.push_back("    }");
    lines.push_back("  }");
  }
  lines.push_back("}");

  const auto options = DefaultCStyleOptions();
  FoldingModel windowed;
  FoldingModel whole;
  Expect(whole.Compute(lines, options), "whole-document resolve should complete");

  for (std::size_t top : {std::size_t{0}, std::size_t{137}, std::size_t{900},
                          std::size_t{1500}, lines.size() - 5}) {
    const std::size_t bottom = std::min(top + 40, lines.size() - 1);
    Expect(windowed.Refresh(lines, options, top, bottom, 0, LineEditSpan{}),
           "windowed resolve should complete");
    for (std::size_t line = top; line <= bottom; ++line) {
      const auto expected = whole.FoldStartingAt(line);
      const auto actual = windowed.FoldStartingAt(line);
      Expect(expected.has_value() == actual.has_value() &&
                 (!expected || (expected->closer_line == actual->closer_line &&
                                expected->source == actual->source)),
             "a windowed resolve must agree with the whole-document resolve on line " +
                 std::to_string(line));
    }
    std::vector<FoldRange> expected_ancestors;
    std::vector<FoldRange> actual_ancestors;
    whole.AppendFoldsContaining(top, &expected_ancestors);
    windowed.AppendFoldsContaining(top, &actual_ancestors);
    Expect(expected_ancestors.size() == actual_ancestors.size(),
           "a windowed resolve must find the same ancestors of its first line (sticky scroll)");
    for (std::size_t i = 0; i < expected_ancestors.size() && i < actual_ancestors.size(); ++i) {
      Expect(expected_ancestors[i].opener_line == actual_ancestors[i].opener_line &&
                 expected_ancestors[i].closer_line == actual_ancestors[i].closer_line,
             "windowed ancestors must match the whole-document ancestors");
    }
  }
}

void TestCollapsedFoldSurvivesInsertionAbove() {
  std::vector<std::string> before = {
      "void a() {",
      "}",
      "void b() {",
      "  body();",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(before, DefaultCStyleOptions()), "initial compute should finish");
  Expect(model.Collapse(2), "the b() fold should accept collapse");
  Expect(model.IsCollapsedAtOpener(2), "the b() fold should be collapsed");

  std::vector<std::string> after = {
      "// inserted 1",
      "// inserted 2",
      "void a() {",
      "}",
      "void b() {",
      "  body();",
      "}",
  };
  LineEditSpan inserted_at_top;
  inserted_at_top.NoteSplice(0, 0, 2);
  Expect(model.Refresh(after, DefaultCStyleOptions(), 0, FoldingModel::kAllLines,
                       0, inserted_at_top),
         "recompute after the insertion should finish");
  Expect(!model.IsCollapsedAtOpener(2), "nothing is collapsed at the old opener line any more");
  Expect(model.IsCollapsedAtOpener(4),
         "the collapse follows the fold to its shifted opener line");
  Expect(model.IsLineHidden(5), "the shifted collapsed fold still hides its body");
}

void TestCollapsedFoldDroppedWhenTheEditDissolvesIt() {
  std::vector<std::string> lines = {
      "void a() {",
      "  body();",
      "}",
  };
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "initial compute should finish");
  Expect(model.Collapse(0), "the fold should collapse");
  Expect(model.IsLineHidden(1), "the collapsed fold hides its body");

  lines[0] = "void a()";
  LineEditSpan edited;
  edited.NoteSplice(0, 1, 1);
  Expect(model.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, edited),
         "resolve after the brace-removing edit should finish");
  Expect(model.IsCollapsedAtOpener(0),
         "a fold that still opens on the same line keeps its collapse");
  Expect(model.collapsed_ranges()[0].closer_line == 1,
         "and the collapse adopts the fold's new extent instead of hiding stale rows");

  lines[0] = "";
  LineEditSpan blanked;
  blanked.NoteSplice(0, 1, 1);
  Expect(model.Refresh(lines, options, 0, FoldingModel::kAllLines, 0, blanked),
         "resolve after the dissolving edit should finish");
  Expect(!model.has_any_collapsed_fold(),
         "a collapse whose fold no longer exists in the resolved window must be dropped");
  Expect(!model.IsLineHidden(1), "and nothing may stay hidden behind it");
}

void TestCollapsedFoldOutsideWindowIsKept() {
  std::vector<std::string> lines;
  lines.push_back("void top() {");
  lines.push_back("  body();");
  lines.push_back("}");
  for (int i = 0; i < 4000; ++i) {
    lines.push_back("// filler");
  }
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "initial compute should finish");
  Expect(model.Collapse(0), "the top fold should collapse");

  Expect(model.Refresh(lines, options, 3500, 3540, 0, LineEditSpan{}),
         "a far window should resolve");
  Expect(model.IsCollapsedAtOpener(0),
         "a collapse outside the resolved window must survive the resolve");
  Expect(model.IsLineHidden(1), "and must keep hiding its body");
}

void TestEnsureFoldsResolvesVisibleOpenerWithDistantCloser() {
  std::vector<std::string> lines;
  lines.reserve(128);
  for (int i = 0; i < 5; ++i) {
    lines.push_back("// preamble");
  }
  lines.push_back("void method() {");
  for (int i = 0; i < 100; ++i) {
    lines.push_back("  step();");
  }
  lines.push_back("}");

  FoldingModel model;
  Expect(model.Refresh(lines, DefaultCStyleOptions(), 0, 20,
                       2000),
         "visible-range resolve should finish");
  const auto fold = model.FoldStartingAt(5);
  Expect(fold.has_value() && fold->closer_line == 106 && fold->source == FoldSource::Bracket,
         "visible opener with closer far below the viewport must fold on the first frame");
}

void TestDistantCloserResolvesWithoutScrollingToIt() {
  std::vector<std::string> lines;
  lines.reserve(6200);
  lines.push_back("namespace n {");
  for (int i = 0; i < 6000; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");
  for (int i = 0; i < 100; ++i) {
    lines.push_back("// tail");
  }

  FoldingModel model;
  const auto options = DefaultCStyleOptions();
  int guard = 0;
  while (!model.Refresh(lines, options, 0, 20, 2000,
                        LineEditSpan{})) {
    Expect(++guard < 40, "budgeted refresh must converge");
  }
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == 6001,
         "an opener at the top of a large file must resolve its far closer without scrolling");
}

std::vector<std::string> BuildFixtureWithThreeDisjointFolds() {
  std::vector<std::string> lines;
  lines.reserve(2048);
  for (int i = 0; i < 100; ++i) lines.push_back("// pre filler");
  lines.push_back("void before() {");
  for (int i = 0; i < 30; ++i) lines.push_back("  before_body();");
  lines.push_back("}");
  for (int i = 0; i < 500; ++i) lines.push_back("// mid filler");
  lines.push_back("void inside() {");
  for (int i = 0; i < 40; ++i) lines.push_back("  inside_body();");
  lines.push_back("}");
  for (int i = 0; i < 500; ++i) lines.push_back("// gap filler");
  lines.push_back("void after() {");
  for (int i = 0; i < 20; ++i) lines.push_back("  after_body();");
  lines.push_back("}");
  for (int i = 0; i < 200; ++i) lines.push_back("// tail filler");
  return lines;
}

void TestCollapsedFoldsBeforeAndAfterViewportDoNotHideViewportRows() {
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  Expect(model.Collapse(100), "should collapse the before-viewport fold opener");
  Expect(model.Collapse(1174), "should collapse the after-viewport fold opener");

  Expect(!model.IsLineHidden(632),
         "viewport opener line must not be hidden by an out-of-range collapsed fold");
  Expect(!model.IsLineHidden(650),
         "viewport body line must not be hidden by an out-of-range collapsed fold");
  Expect(!model.IsLineHidden(672),
         "viewport last body line must not be hidden by an out-of-range collapsed fold");

  Expect(model.IsLineHidden(115),
         "line inside collapsed before-viewport fold body must be hidden");
  Expect(model.IsLineHidden(1180),
         "line inside collapsed after-viewport fold body must be hidden");

  Expect(!model.IsLineHidden(100),
         "opener of collapsed fold must remain visible (consumed visual row)");
  Expect(!model.IsLineHidden(1174),
         "opener of collapsed fold must remain visible (consumed visual row)");
}

void TestCollapsedFoldInsideViewportHidesInteriorRows() {
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  Expect(model.Collapse(632), "should collapse the inside-viewport fold opener");

  Expect(model.IsLineHidden(640),
         "interior body line of collapsed in-viewport fold must be hidden");
  Expect(model.IsLineHidden(672),
         "interior closer-adjacent line of collapsed in-viewport fold must be hidden");
  Expect(!model.IsLineHidden(632),
         "opener of collapsed in-viewport fold remains visible");
  Expect(!model.IsLineHidden(631),
         "line immediately before the fold opener must remain visible");
  Expect(!model.IsLineHidden(674),
         "line immediately after the fold closer must remain visible");
}

void TestIndexedLookupsRunAfterFoldToggleInvalidation() {
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  Expect(!model.IsLineHidden(632), "interior is visible before collapse");
  Expect(!model.IsLineHidden(640), "interior is visible before collapse");

  Expect(model.Collapse(632), "collapse must succeed for a known opener");
  Expect(model.IsLineHidden(640),
         "indexed cache must report interior hidden immediately after collapse");

  Expect(model.Expand(632), "expand must succeed after collapse");
  Expect(!model.IsLineHidden(640),
         "indexed cache must report interior visible immediately after expand");

  Expect(model.ToggleFold(632), "toggle should re-collapse");
  Expect(model.IsLineHidden(640),
         "indexed cache must reflect the toggled collapsed state");
}

void TestIndexedStickyScrollParentLookupAcrossNestedFolds() {
  std::vector<std::string> lines;
  lines.push_back("void outer() {");
  lines.push_back("  void mid_a() {");
  for (int i = 0; i < 20; ++i) lines.push_back("    body();");
  lines.push_back("  }");
  lines.push_back("  void mid_b() {");
  for (int i = 0; i < 10; ++i) lines.push_back("    deeper();");
  lines.push_back("    void inner() {");
  for (int i = 0; i < 10; ++i) lines.push_back("      leaf();");
  lines.push_back("    }");
  lines.push_back("  }");
  lines.push_back("}");

  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "compute should complete for nested fixture");

  const auto innermost = model.InnermostFoldContaining(40);
  Expect(innermost.has_value() && innermost->opener_line == 34,
         "sticky-scroll parent lookup at leaf line must resolve to the innermost opener");

  const auto mid = model.InnermostFoldContaining(28);
  Expect(mid.has_value() && mid->opener_line == 23,
         "lookup before the inner opener must resolve to the surrounding mid fold");

  const auto closer = model.InnermostFoldContaining(22);
  Expect(closer.has_value() && closer->opener_line == 1,
         "closer line must resolve to its own fold via the indexed lookup");

  const auto opener_self = model.InnermostFoldContaining(23);
  Expect(opener_self.has_value() && opener_self->opener_line == 23,
         "opener line must resolve to its own fold (inclusive of opener)");

  std::vector<FoldRange> ancestors;
  model.AppendFoldsContaining(40, &ancestors);
  Expect(ancestors.size() == 3 && ancestors[0].opener_line == 0 &&
             ancestors[1].opener_line == 23 && ancestors[2].opener_line == 34,
         "AppendFoldsContaining must return every enclosing fold, outermost first");
}

void TestIndexedFoldStartingAtAndIsCollapsedAtOpenerStaySynchronized() {
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  Expect(model.FoldStartingAt(100).has_value(),
         "FoldStartingAt must find the known opener");
  Expect(!model.FoldStartingAt(101).has_value(),
         "FoldStartingAt must return nothing for a non-opener line");
  Expect(!model.IsCollapsedAtOpener(100),
         "IsCollapsedAtOpener defaults to false for a fresh model");
  Expect(model.Collapse(100), "collapse should succeed");
  Expect(model.IsCollapsedAtOpener(100),
         "IsCollapsedAtOpener must reflect the collapsed flag");

  Expect(!model.FoldStartingAt(lines.size() + 100).has_value(),
         "FoldStartingAt for an out-of-range line must return nullopt");
  Expect(!model.IsCollapsedAtOpener(lines.size() + 100),
         "IsCollapsedAtOpener for an out-of-range line must return false");
}

void TestHasAnyCollapsedFoldTracksCounter() {
  const std::vector<std::string> lines = {
      "void f() {",
      "  void g() {",
      "    body();",
      "  }",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");
  Expect(!model.has_any_collapsed_fold(),
         "fresh model with no Collapse() calls has no collapsed fold");
  Expect(model.Collapse(0), "outer fold should collapse");
  Expect(model.has_any_collapsed_fold(),
         "Collapse must update the has-any-collapsed probe");
  Expect(model.Collapse(1), "inner fold should also collapse");
  Expect(model.has_any_collapsed_fold(),
         "second Collapse must keep the probe positive");
  Expect(model.Expand(0), "outer fold should expand");
  Expect(model.has_any_collapsed_fold(),
         "after one Expand, the inner fold still keeps the probe positive");
  Expect(model.Expand(1), "inner fold should expand");
  Expect(!model.has_any_collapsed_fold(),
         "all Expand() should bring the probe back to false");
  Expect(model.ToggleFold(0), "toggle should flip outer fold");
  Expect(model.has_any_collapsed_fold(), "toggle to collapsed sets the probe");
  Expect(model.ToggleFold(0), "toggle should flip outer fold back");
  Expect(!model.has_any_collapsed_fold(), "toggle to expanded clears the probe");
  Expect(model.CollapseAllResolved(), "CollapseAllResolved must collapse every resolved fold");
  Expect(model.has_any_collapsed_fold(), "CollapseAllResolved must set the probe");
  Expect(model.collapsed_ranges().size() == 2, "both folds must be collapsed");
  model.ExpandAll();
  Expect(!model.has_any_collapsed_fold(), "ExpandAll must reset the probe");
  model.Clear();
  Expect(!model.has_any_collapsed_fold(), "Clear must reset the probe");
}

void TestLayoutRevisionOnlyTracksTheCollapsedSet() {
  std::vector<std::string> lines;
  lines.push_back("void top() {");
  lines.push_back("  body();");
  lines.push_back("}");
  for (int i = 0; i < 3000; ++i) {
    lines.push_back("// filler");
  }
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(lines, options), "compute should complete");

  const std::size_t layout_before = model.layout_revision();
  const std::size_t revision_before = model.revision();
  Expect(model.Refresh(lines, options, 1500, 1540, 0, LineEditSpan{}),
         "a scrolled resolve should complete");
  Expect(model.revision() != revision_before,
         "resolving a different window must bump the general revision");
  Expect(model.layout_revision() == layout_before,
         "resolving a different window must NOT bump the layout revision");

  Expect(model.Refresh(lines, options, 0, 40, 0, LineEditSpan{}),
         "scrolling back should complete");
  Expect(model.Collapse(0), "collapse should succeed");
  Expect(model.layout_revision() != layout_before,
         "changing the collapsed set must bump the layout revision");
}

void TestOverLongLineContributesNoBrackets() {
  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.use_indent_source = false;

  const auto document_with_opener_of_length = [](std::size_t bytes) {
    std::string opener = "{";
    opener.append(bytes - opener.size(), 'x');
    return std::vector<std::string>{
        std::move(opener), "  body", "}", "{", "  ordinary", "}",
    };
  };

  {
    std::vector<std::string> lines =
        document_with_opener_of_length(FoldingModel::kMaxBracketScanLineBytes);
    FoldingModel model;
    Expect(model.Compute(lines, options), "resolving an at-cap line should complete");
    Expect(Find(model.resolved_ranges(), 0).closer_line == 2,
           "a line exactly at the bracket cap still folds: " +
               DescribeRanges(model.resolved_ranges()));
    Expect(Find(model.resolved_ranges(), 3).closer_line == 5,
           "the ordinary fold below it resolves too");
  }

  {
    std::vector<std::string> lines =
        document_with_opener_of_length(FoldingModel::kMaxBracketScanLineBytes + 1);
    FoldingModel model;
    Expect(model.Compute(lines, options), "resolving an over-cap line should complete");
    Expect(!model.FoldStartingAt(0).has_value(),
           "one byte past the cap the line contributes no brackets: " +
               DescribeRanges(model.resolved_ranges()));
    Expect(Find(model.resolved_ranges(), 3).closer_line == 5,
           "a normal line in the same document keeps folding: " +
               DescribeRanges(model.resolved_ranges()));
  }
}

void TestOverLongLineIsSkippedWithoutReadingIt() {
  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.use_indent_source = false;

  microide::editor::TextBuffer buffer;
  std::string over_cap = "{";
  over_cap.append(FoldingModel::kMaxBracketScanLineBytes, 'x');
  buffer.ResetFromText(over_cap + "\n  body\n}\n");
  buffer.ReplaceTextRange(0, over_cap.size() / 2, 0, over_cap.size() / 2, "MID");

  FoldingModel model;
  util::ResetPerformanceCounters();
  Expect(model.Compute(buffer, options), "resolving an over-cap line should complete");
  Expect(!model.FoldStartingAt(0).has_value(), "the over-cap line still contributes no brackets");
  const std::uint64_t copied =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(copied == 0,
         "a line skipped for length must not be read first (copied " + std::to_string(copied) +
             " bytes)");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorFoldBracketLinesSkippedTooLong) > 0,
         "the skip counter must have moved -- otherwise the zero above proves nothing");

  (void)buffer.LineView(0);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes) >
             FoldingModel::kMaxBracketScanLineBytes,
         "reading the spanning over-cap line whole still copies it (control)");
  util::ResetPerformanceCounters();

  options.use_indent_source = true;
  FoldingModel indent_model;
  util::ResetPerformanceCounters();
  Expect(indent_model.Compute(buffer, options), "the indent pass should complete");
  const std::uint64_t indent_copied =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(indent_copied == 0,
         "measuring a line's indent must not read the rest of it (copied " +
             std::to_string(indent_copied) + " bytes)");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorFoldIndentLinesMeasured) > 0,
         "the indent scan must have run -- otherwise the zero above proves nothing");
  util::ResetPerformanceCounters();
}

void TestDeepIndentCrossingTheScanChunkMatchesTheReference() {
  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.bracket_pairs = {};

  {
    options.tab_size = 3;
    const std::vector<std::string> lines = {
        "root:",
        std::string(257, ' ') + "\ta",
        std::string(259, ' ') + "b",
        "end",
    };
    FoldingModel model;
    Expect(model.Compute(lines, options), "indent compute should complete");
    ExpectMatchesReference(model, lines, options, "one-column split across the scan chunk");
  }

  for (const std::size_t tab_size : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
    options.tab_size = tab_size;
    for (const std::size_t leading : {std::size_t{250}, std::size_t{255}, std::size_t{256},
                                      std::size_t{257}, std::size_t{600}}) {
      for (const std::string_view tail : {"\t", "\t\t\t", " \t ", ""}) {
        const std::string indent = "\t " + std::string(leading, ' ') + std::string(tail);
        const std::vector<std::string> lines = {
            "outer:",
            indent + "body",
            indent + "\tdeeper",
            std::string(leading + 8, ' '),
            indent + "body again",
            "next",
        };
        FoldingModel model;
        Expect(model.Compute(lines, options), "indent compute should complete");
        ExpectMatchesReference(model, lines, options,
                               "tab_size " + std::to_string(tab_size) + ", indent of " +
                                   std::to_string(leading) + " spaces + '" + std::string(tail) +
                                   "'");
      }
    }
  }
}

}

void RegisterFoldingModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorFolding/Indent/DeepIndentCrossingTheScanChunkMatchesTheReference",
          TestDeepIndentCrossingTheScanChunkMatchesTheReference);
  AddTest(tests, "EditorFolding/Bracket/OverLongLineIsSkippedWithoutReadingIt",
          TestOverLongLineIsSkippedWithoutReadingIt);
  AddTest(tests, "EditorFolding/Bracket/OverLongLineContributesNoBrackets",
          TestOverLongLineContributesNoBrackets);
  AddTest(tests, "EditorFolding/Bracket/EmitsOpenerAndCloser",
          TestBracketFoldEmitsOpenerAndCloser);
  AddTest(tests, "EditorFolding/Bracket/InnermostFoldContainingNested",
          TestInnermostFoldContainingPicksDeepestOpener);
  AddTest(tests, "EditorFolding/Indent/FallsBackWhenNoBrackets",
          TestIndentFoldFallsBackWhenNoBrackets);
  AddTest(tests, "EditorFolding/Indent/EmittedWhenScanWindowReachesBudget",
          TestIndentFoldsEmittedWhenScanWindowReachesBudget);
  AddTest(tests, "EditorFolding/Mixed/BracketWinsOnDuplicateOpener",
          TestBracketWinsOnDuplicateOpenerLine);
  AddTest(tests, "EditorFolding/Bracket/SkipsStringAndCommentRegions",
          TestBracketFoldSkipsStringAndCommentRegions);
  AddTest(tests, "EditorFolding/Budget/PartialResolve", TestBudgetedRefreshFlagsPartial);
  AddTest(tests, "EditorFolding/Budget/ConvergesGeometrically",
          TestBudgetedRefreshConvergesGeometrically);
  AddTest(tests, "EditorFolding/State/ToggleHidesInterior",
          TestToggleFoldHidesInteriorLines);
  AddTest(tests, "EditorFolding/Incremental/MatchesReference",
          TestIncrementalRefreshMatchesReference);
  AddTest(tests, "EditorFolding/Incremental/RandomisedSplicesMatchReference",
          TestRandomisedSplicesMatchReference);
  AddTest(tests, "EditorFolding/Incremental/DeletingLinesAboveCachedBlocksKeepsTheirEventsInStep",
          TestDeletingLinesAboveCachedBlocksKeepsTheirEventsInStep);
  AddTest(tests, "EditorFolding/Window/MatchesWholeDocumentInsideWindow",
          TestWindowedResolveMatchesWholeDocumentInsideItsWindow);
  AddTest(tests, "EditorFolding/Incremental/CollapsedFoldSurvivesInsertionAbove",
          TestCollapsedFoldSurvivesInsertionAbove);
  AddTest(tests, "EditorFolding/Collapsed/DroppedWhenEditDissolvesIt",
          TestCollapsedFoldDroppedWhenTheEditDissolvesIt);
  AddTest(tests, "EditorFolding/Collapsed/OutsideWindowIsKept",
          TestCollapsedFoldOutsideWindowIsKept);
  AddTest(tests, "EditorFolding/VisibleRange/ResolvesVisibleOpenerWithDistantCloser",
          TestEnsureFoldsResolvesVisibleOpenerWithDistantCloser);
  AddTest(tests, "EditorFolding/VisibleRange/DistantCloserResolvesWithoutScrolling",
          TestDistantCloserResolvesWithoutScrollingToIt);
  AddTest(tests, "EditorFolding/VisibleRange/ForwardResolveCapDefersVeryLongFold",
          TestForwardResolveCapDefersAVeryLongFold);
  AddTest(tests, "EditorFolding/IndexedLookup/CollapsedBeforeAndAfterViewportDoNotHideViewport",
          TestCollapsedFoldsBeforeAndAfterViewportDoNotHideViewportRows);
  AddTest(tests, "EditorFolding/IndexedLookup/CollapsedInsideViewportHidesInterior",
          TestCollapsedFoldInsideViewportHidesInteriorRows);
  AddTest(tests, "EditorFolding/IndexedLookup/ToggleInvalidation",
          TestIndexedLookupsRunAfterFoldToggleInvalidation);
  AddTest(tests, "EditorFolding/IndexedLookup/StickyScrollParentLookup",
          TestIndexedStickyScrollParentLookupAcrossNestedFolds);
  AddTest(tests, "EditorFolding/IndexedLookup/OpenerAndCollapsedFlagSync",
          TestIndexedFoldStartingAtAndIsCollapsedAtOpenerStaySynchronized);
  AddTest(tests, "EditorFolding/HasAnyCollapsedFoldTracksCounter",
          TestHasAnyCollapsedFoldTracksCounter);
  AddTest(tests, "EditorFolding/LayoutRevisionOnlyTracksCollapsedSet",
          TestLayoutRevisionOnlyTracksTheCollapsedSet);
  AddTest(tests, "EditorFolding/StaleWithoutMarkDirtyAfterSameLineCountEdit",
          TestFoldStaleWithoutMarkDirtyAfterSameLineCountEdit);
  AddTest(tests, "EditorFolding/AnEditSpanTheDocumentContradictsRebuildsTheIndentCache",
          TestAnEditSpanTheDocumentContradictsRebuildsTheIndentCache);
}

}
