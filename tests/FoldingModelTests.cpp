#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::FoldRange;
using microide::editor::FoldSource;
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
  const auto& ranges = model.ranges();
  Expect(ranges.size() == 2,
         "should produce two bracket fold ranges (outer + inner)");
  const auto outer = Find(ranges, 0);
  Expect(outer.closer_line == 4 && outer.source == FoldSource::Bracket,
         "outer { should fold to its closer at line 4");
  const auto inner = Find(ranges, 1);
  Expect(inner.closer_line == 3 && inner.source == FoldSource::Bracket,
         "inner { should fold to its closer at line 3");
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
  options.bracket_pairs = {};  // language has no brackets (Python-ish)
  FoldingModel model;
  Expect(model.Compute(lines, options),
         "indent compute should complete on small input");
  Expect(model.ranges().size() == 1,
         "should emit a single indent-source fold for the indented block");
  Expect(model.ranges()[0].opener_line == 0 &&
             model.ranges()[0].closer_line == 2 &&
             model.ranges()[0].source == FoldSource::Indent,
         "indent-source fold should span the indented block");
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
  Expect(model.ranges().size() == 1,
         "duplicate opener should be deduplicated to a single range");
  Expect(model.ranges()[0].source == FoldSource::Bracket,
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
  // Prime the token cache so the folding model
  // consumes the same syntax classification as the live viewport path.
  (void)viewport.HighlightedLineTokens(0);
  (void)viewport.HighlightedLineTokens(1);
  (void)viewport.HighlightedLineTokens(2);
  (void)viewport.HighlightedLineTokens(3);
  (void)viewport.HighlightedLineTokens(4);

  FoldingModel model;
  Expect(model.ComputeWithBudget(viewport.lines(), DefaultCStyleOptions(), /*max_lines=*/0,
                                 std::numeric_limits<std::size_t>::max(),
                                 std::numeric_limits<std::size_t>::max(), &viewport),
         "syntax-aware fold compute should complete");
  Expect(model.ranges().size() == 1,
         "string/comment brackets should not create or interfere with fold ranges");
  const auto function_fold = Find(model.ranges(), 0);
  Expect(function_fold.closer_line == 2 && function_fold.source == FoldSource::Bracket,
         "only the real function body braces should define the fold");
}

void TestComputeWithBudgetFlagsPartial() {
  std::vector<std::string> lines;
  lines.reserve(200);
  for (int i = 0; i < 200; ++i) {
    lines.push_back("line");
  }
  FoldingModel model;
  Expect(!model.ComputeWithBudget(lines, DefaultCStyleOptions(), /*max_lines=*/10),
         "small budget should mark compute as partial");
  Expect(!model.complete(),
         "partial compute should report complete()==false");
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

void TestIncrementalBracketScanReusesPrefixAndCollapseState() {
  std::vector<std::string> lines = {
      "void outer() {",  // 0
      "  void inner() {",  // 1
      "    body();",       // 2
      "  }",               // 3
      "}",                 // 4
      "// tail",           // 5
      "void after() {",    // 6
      "}",                 // 7
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "initial compute should finish for incremental fixture");
  Expect(model.Collapse(0), "outer fold should accept collapse");
  Expect(model.IsCollapsedAtOpener(0), "outer fold should be collapsed");

  Expect(model.ComputeWithBudget(lines, DefaultCStyleOptions(), /*max_lines=*/0, /*resume=*/5),
         "incremental refresh should complete");
  const auto outer = Find(model.ranges(), 0);
  Expect(outer.closer_line == 4 && outer.source == FoldSource::Bracket,
         "incremental path should preserve the outer bracket span");
  Expect(model.IsCollapsedAtOpener(0),
         "collapse state should remap across incremental bracket recompute");
}

void TestEnsureFoldsForVisibleRangeBoundsInitialResolve() {
  std::vector<std::string> lines;
  lines.reserve(256);
  for (int i = 0; i < 128; ++i) {
    lines.push_back("top filler");
  }
  lines.push_back("void far() {");
  lines.push_back("  body();");
  lines.push_back("}");
  for (int i = 0; i < 64; ++i) {
    lines.push_back("tail filler");
  }

  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/0,
                                          /*visible_end_line=*/15,
                                          /*max_lines=*/2000),
         "visible-range resolve should finish for a small first viewport");
  Expect(!model.FoldStartingAt(128).has_value(),
         "first viewport resolve should not walk far fold openers outside the visible prefix");
}

void TestEnsureFoldsForVisibleRangeExtendsOnScroll() {
  std::vector<std::string> lines;
  lines.reserve(256);
  for (int i = 0; i < 128; ++i) {
    lines.push_back("top filler");
  }
  lines.push_back("void far() {");
  lines.push_back("  body();");
  lines.push_back("}");
  for (int i = 0; i < 64; ++i) {
    lines.push_back("tail filler");
  }

  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/120,
                                          /*visible_end_line=*/132,
                                          /*max_lines=*/2000),
         "visible-range resolve should finish for the scrolled viewport");
  const auto far = model.FoldStartingAt(128);
  Expect(far.has_value(), "scrolled viewport should extend the resolved fold prefix");
  Expect(far->closer_line == 130 && far->source == FoldSource::Bracket,
         "extended visible-range resolve should discover the distant bracket fold");
}

}  // namespace

void RegisterFoldingModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorFolding/Bracket/EmitsOpenerAndCloser",
          TestBracketFoldEmitsOpenerAndCloser);
  AddTest(tests, "EditorFolding/Bracket/InnermostFoldContainingNested",
          TestInnermostFoldContainingPicksDeepestOpener);
  AddTest(tests, "EditorFolding/Indent/FallsBackWhenNoBrackets",
          TestIndentFoldFallsBackWhenNoBrackets);
  AddTest(tests, "EditorFolding/Mixed/BracketWinsOnDuplicateOpener",
          TestBracketWinsOnDuplicateOpenerLine);
  AddTest(tests, "EditorFolding/Bracket/SkipsStringAndCommentRegions",
          TestBracketFoldSkipsStringAndCommentRegions);
  AddTest(tests, "EditorFolding/Budget/PartialCompute",
          TestComputeWithBudgetFlagsPartial);
  AddTest(tests, "EditorFolding/State/ToggleHidesInterior",
          TestToggleFoldHidesInteriorLines);
  AddTest(tests, "EditorFolding/Incremental/ReusesPrefixAndCollapse",
          TestIncrementalBracketScanReusesPrefixAndCollapseState);
  AddTest(tests, "EditorFolding/VisibleRange/BoundsInitialResolve",
          TestEnsureFoldsForVisibleRangeBoundsInitialResolve);
  AddTest(tests, "EditorFolding/VisibleRange/ExtendsOnScroll",
          TestEnsureFoldsForVisibleRangeExtendsOnScroll);
}

}  // namespace microide::tests
