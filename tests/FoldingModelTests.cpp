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

// §2.4 — Indexed fold-lookup regression coverage. Builds a synthetic large
// file with three disjoint fold ranges (before, inside, after a notional
// viewport) and exercises the revision-keyed lookup cache via the public
// `IsLineHidden`, `FoldStartingAt`, `IsCollapsedAtOpener`, and
// `InnermostFoldContaining` accessors.
std::vector<std::string> BuildFixtureWithThreeDisjointFolds() {
  std::vector<std::string> lines;
  lines.reserve(2048);
  for (int i = 0; i < 100; ++i) lines.push_back("// pre filler");
  lines.push_back("void before() {");      // 100
  for (int i = 0; i < 30; ++i) lines.push_back("  before_body();");
  lines.push_back("}");                    // 131
  for (int i = 0; i < 500; ++i) lines.push_back("// mid filler");  // 132..631
  lines.push_back("void inside() {");      // 632
  for (int i = 0; i < 40; ++i) lines.push_back("  inside_body();");
  lines.push_back("}");                    // 673
  for (int i = 0; i < 500; ++i) lines.push_back("// gap filler");  // 674..1173
  lines.push_back("void after() {");       // 1174
  for (int i = 0; i < 20; ++i) lines.push_back("  after_body();");
  lines.push_back("}");                    // 1195
  for (int i = 0; i < 200; ++i) lines.push_back("// tail filler");
  return lines;
}

void TestCollapsedFoldsBeforeAndAfterViewportDoNotHideViewportRows() {
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  Expect(model.Collapse(100), "should collapse the before-viewport fold opener");
  Expect(model.Collapse(1174), "should collapse the after-viewport fold opener");

  // Notional viewport spans lines 632..672 (the body of `inside()`).
  Expect(!model.IsLineHidden(632),
         "viewport opener line must not be hidden by an out-of-range collapsed fold");
  Expect(!model.IsLineHidden(650),
         "viewport body line must not be hidden by an out-of-range collapsed fold");
  Expect(!model.IsLineHidden(672),
         "viewport last body line must not be hidden by an out-of-range collapsed fold");

  // Lines genuinely inside the collapsed ranges must still report hidden.
  Expect(model.IsLineHidden(115),
         "line inside collapsed before-viewport fold body must be hidden");
  Expect(model.IsLineHidden(1180),
         "line inside collapsed after-viewport fold body must be hidden");

  // Opener lines themselves remain visible (the design treats them as the one
  // consumed visual row for a collapsed fold).
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
  // Lines just outside the fold range remain visible.
  Expect(!model.IsLineHidden(631),
         "line immediately before the fold opener must remain visible");
  Expect(!model.IsLineHidden(674),
         "line immediately after the fold closer must remain visible");
}

void TestIndexedLookupsRunAfterFoldToggleInvalidation() {
  // The indexed cache is keyed on `revision_`, which increments on every
  // `ToggleFold`/`Collapse`/`Expand`. After a toggle, IsLineHidden must reflect
  // the new collapsed-flag state.
  const std::vector<std::string> lines = BuildFixtureWithThreeDisjointFolds();
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "compute should complete");

  // Pre-toggle: opener visible, interior visible.
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
  // Build a deeply nested fold structure so `InnermostFoldContaining` must
  // walk back across openers to find the parent that still contains a target
  // line. The indexed prefix-max closer table should still return the right
  // innermost fold.
  std::vector<std::string> lines;
  lines.push_back("void outer() {");          // 0
  lines.push_back("  void mid_a() {");        // 1
  for (int i = 0; i < 20; ++i) lines.push_back("    body();");
  lines.push_back("  }");                     // 22
  lines.push_back("  void mid_b() {");        // 23
  for (int i = 0; i < 10; ++i) lines.push_back("    deeper();");
  lines.push_back("    void inner() {");      // 34
  for (int i = 0; i < 10; ++i) lines.push_back("      leaf();");
  lines.push_back("    }");                   // 45
  lines.push_back("  }");                     // 46
  lines.push_back("}");                       // 47

  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()),
         "compute should complete for nested fixture");

  // Line 40 is inside `inner()`, which is inside `mid_b()`, which is inside
  // `outer()`. The innermost containing fold must be `inner()` (opener 34).
  const auto innermost = model.InnermostFoldContaining(40);
  Expect(innermost.has_value() && innermost->opener_line == 34,
         "sticky-scroll parent lookup at leaf line must resolve to the innermost opener");

  // Line 28 is inside `mid_b()` but BEFORE `inner()` opens. It must resolve to
  // `mid_b()` (opener 23), not `inner()`.
  const auto mid = model.InnermostFoldContaining(28);
  Expect(mid.has_value() && mid->opener_line == 23,
         "lookup before the inner opener must resolve to the surrounding mid fold");

  // Line 22 is the closer of `mid_a()` — the innermost fold containing line 22
  // is `mid_a()` itself (the closer line is inclusive).
  const auto closer = model.InnermostFoldContaining(22);
  Expect(closer.has_value() && closer->opener_line == 1,
         "closer line must resolve to its own fold via the indexed lookup");

  // Line 23 is the opener of `mid_b()`. It is contained by `outer()` and
  // by `mid_b()` itself; the innermost is `mid_b()` (opener_line == 23).
  const auto opener_self = model.InnermostFoldContaining(23);
  Expect(opener_self.has_value() && opener_self->opener_line == 23,
         "opener line must resolve to its own fold (inclusive of opener)");
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

  // Sanity: looking up a line beyond the document does not crash and reports
  // negative results.
  Expect(!model.FoldStartingAt(lines.size() + 100).has_value(),
         "FoldStartingAt for an out-of-range line must return nullopt");
  Expect(!model.IsCollapsedAtOpener(lines.size() + 100),
         "IsCollapsedAtOpener for an out-of-range line must return false");
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
}

}  // namespace microide::tests
