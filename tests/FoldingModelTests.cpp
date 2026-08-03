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

// Regression: EnsureFoldsForVisibleRange early-returns on !dirty_ once a file is fully
// resolved, IGNORING the content_revision. So a document edit that moves a fold boundary
// WITHOUT changing the line count (a formatter re-indent, a completion / code action /
// plugin edit that re-nests brackets) leaves STALE fold ranges unless the mutating path
// marks the model dirty. This documents why ApplyLspWorkspaceEdit, the completion-accept
// path, and Undo/Redo must all call MarkDirty().
void TestFoldStaleWithoutMarkDirtyAfterSameLineCountEdit() {
  std::vector<std::string> lines = {
      "void f() {",  // 0
      "  if (x) {",  // 1  inner opener (structure A)
      "    a;",      // 2
      "  }",         // 3  inner closer (A)
      "  b;",        // 4
      "}",           // 5  outer closer
  };
  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, options, 0, 5, 100),
         "initial resolve should finish");
  Expect(Find(model.ranges(), 1).closer_line == 3,
         "structure A: the inner bracket fold opens at line 1 and closes at line 3");

  // Mutate to structure B (SAME line count): the inner block slides down one line.
  lines = {
      "void f() {",  // 0
      "  b;",        // 1
      "  if (x) {",  // 2  inner opener (structure B)
      "    a;",      // 3
      "  }",         // 4  inner closer (B)
      "}",           // 5
  };
  // No MarkDirty: the early-return keeps the STALE ranges — the exact phantom-fold
  // hazard the apply-path fixes guard against.
  Expect(model.EnsureFoldsForVisibleRange(lines, options, 0, 5, 100),
         "resolve returns (early) after the same-line-count content change");
  Expect(Find(model.ranges(), 1).closer_line == 3,
         "without MarkDirty the fold model is STALE: it still reports the pre-edit inner fold");

  // MarkDirty forces a rescan against the new content.
  model.MarkDirty();
  Expect(model.EnsureFoldsForVisibleRange(lines, options, 0, 5, 100),
         "resolve after MarkDirty should finish");
  Expect(Find(model.ranges(), 1).closer_line == 0,
         "after MarkDirty the stale inner fold at line 1 is gone");
  Expect(Find(model.ranges(), 2).closer_line == 4,
         "after MarkDirty the inner fold reflects structure B (opens at line 2, closes at line 4)");
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

// Regression: indent-source folds must still be emitted when the scan window
// reaches the compute budget. The emission pass previously shared the
// measurement loop's budget counter, which the measurement loop always
// exhausted first (callers pass work_budget == max(max_lines, scan_end)), so
// every indent fold was silently dropped on files whose scan window hit the
// 2000-line budget — indentation-only languages (Python/YAML) then showed no
// fold markers at all.
void TestIndentFoldsEmittedWhenScanWindowReachesBudget() {
  std::vector<std::string> lines;
  lines.reserve(2100);
  lines.push_back("if cond:");        // indent opener at line 0
  lines.push_back("    body_one()");  // line 1
  lines.push_back("    body_two()");  // line 2 (closer)
  lines.push_back("next_thing()");    // line 3, dedent
  for (int i = 0; i < 2096; ++i) {
    lines.push_back("filler()");  // no folds, pushes line_count past the budget
  }

  FoldingModel::ComputeOptions options = DefaultCStyleOptions();
  options.bracket_pairs = {};  // Python-ish: no bracket fold to mask the indent fold

  FoldingModel model;
  // Visible at the very top; scan_end resolves to max(target_end, budget) == 2000
  // == max_lines, the exact condition that previously starved the emission loop.
  Expect(model.EnsureFoldsForVisibleRange(lines, options,
                                          /*visible_start_line=*/0,
                                          /*visible_end_line=*/20,
                                          /*max_lines=*/2000),
         "visible-range resolve should finish");
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == 2 &&
             fold->source == FoldSource::Indent,
         "indent fold near the top must be emitted even when scan_end == budget");
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
  Expect(model.ComputeWithBudget(viewport.lines().Snapshot(), DefaultCStyleOptions(), /*max_lines=*/0,
                                 LineEditSpan::FullRebuild(),
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

  Expect(model.ComputeWithBudget(lines, DefaultCStyleOptions(), /*max_lines=*/0,
                                 LineEditSpan::SuffixReplacedFrom(5)),
         "incremental refresh should complete");
  const auto outer = Find(model.ranges(), 0);
  Expect(outer.closer_line == 4 && outer.source == FoldSource::Bracket,
         "incremental path should preserve the outer bracket span");
  Expect(model.IsCollapsedAtOpener(0),
         "collapse state should remap across incremental bracket recompute");
}

// Regression: a collapsed fold survives a line-count-changing edit ABOVE it. The
// recompute matched previous collapsed openers by ABSOLUTE line, so inserting lines
// above shifted the opener, failed the match, and dropped the collapse (the fold
// visually re-expanded). The remap now shifts previous collapsed openers/closers by
// the net edit delta at/after the edit anchor before matching (VS Code shift-preserves).
void TestCollapsedFoldSurvivesInsertionAbove() {
  std::vector<std::string> before = {
      "void a() {",  // 0
      "}",           // 1
      "void b() {",  // 2  <- collapse this fold
      "  body();",   // 3
      "}",           // 4
  };
  FoldingModel model;
  Expect(model.Compute(before, DefaultCStyleOptions()), "initial compute should finish");
  Expect(model.Collapse(2), "the b() fold should accept collapse");
  Expect(model.IsCollapsedAtOpener(2), "the b() fold should be collapsed");

  // Insert two lines at the very top; the b() opener shifts from line 2 to line 4.
  std::vector<std::string> after = {
      "// inserted 1",  // 0
      "// inserted 2",  // 1
      "void a() {",     // 2
      "}",              // 3
      "void b() {",     // 4  <- opener shifted down by 2
      "  body();",      // 5
      "}",              // 6
  };
  // Two lines inserted at the top; the net +2 line delta is inferred from the
  // change in line count since the previous compute.
  LineEditSpan inserted_at_top;
  inserted_at_top.NoteSplice(/*start=*/0, /*removed=*/0, /*inserted=*/2);
  Expect(model.ComputeWithBudget(after, DefaultCStyleOptions(), /*max_lines=*/0, inserted_at_top),
         "recompute after the insertion should finish");
  Expect(!model.IsCollapsedAtOpener(2), "nothing is collapsed at the old opener line any more");
  Expect(model.IsCollapsedAtOpener(4),
         "the collapse follows the fold to its shifted opener line");
}

void TestEnsureFoldsForVisibleRangeResolvesWithinBudgetWindow() {
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
  // The whole 195-line file fits inside the 2000-line per-frame budget, so a
  // fold opener below the look-ahead must still resolve on the first viewport
  // (its closer is well within the budget window).
  const auto far = model.FoldStartingAt(128);
  Expect(far.has_value() && far->closer_line == 130 && far->source == FoldSource::Bracket,
         "first viewport resolve should walk the whole budget window, not just the look-ahead");
}

// Regression for the reported bug: a method whose opener is on a visible line
// but whose closer sits many lines below the viewport bottom must still get a
// fold range on the first frame (closer within budget).
void TestEnsureFoldsForVisibleRangeResolvesVisibleOpenerWithDistantCloser() {
  std::vector<std::string> lines;
  lines.reserve(128);
  for (int i = 0; i < 5; ++i) {
    lines.push_back("// preamble");
  }
  lines.push_back("void method() {");  // opener at line 5
  for (int i = 0; i < 100; ++i) {
    lines.push_back("  step();");
  }
  lines.push_back("}");  // closer at line 106

  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/0,
                                          /*visible_end_line=*/20,
                                          /*max_lines=*/2000),
         "visible-range resolve should finish");
  const auto fold = model.FoldStartingAt(5);
  Expect(fold.has_value() && fold->closer_line == 106 && fold->source == FoldSource::Bracket,
         "visible opener with closer far below the viewport must fold on the first frame");
}

// The budget guard must still bound work on huge files: an opener whose closer
// is beyond the budget from the scan origin should NOT resolve on the first
// frame, but must resolve once the viewport scrolls near it.
void TestEnsureFoldsForVisibleRangeBudgetGuardOnHugeFile() {
  std::vector<std::string> lines;
  lines.reserve(5200);
  lines.push_back("namespace n {");  // opener at line 0
  for (int i = 0; i < 5000; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");  // closer at line 5001
  for (int i = 0; i < 100; ++i) {
    lines.push_back("// tail");
  }

  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/0,
                                          /*visible_end_line=*/20,
                                          /*max_lines=*/2000),
         "visible-range resolve should finish for the first viewport");
  Expect(!model.FoldStartingAt(0).has_value(),
         "closer beyond the budget window must not resolve on the first frame (perf guard)");

  // Scroll near the closer; the extended scan resolves it.
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/4980,
                                          /*visible_end_line=*/5001,
                                          /*max_lines=*/2000),
         "scrolled visible-range resolve should finish");
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == 5001,
         "scrolling near the closer must resolve the far opener");
}

// Extending the resolved prefix must grow it GEOMETRICALLY, not by one look-ahead
// window at a time.
//
// ComputeWithBudget always rescans `[0, scan_end)` from the start -- its
// incremental path serves a localized edit, not a forward extension -- so an
// extension policy that only reaches the current viewport makes scrolling a large
// file quadratic: each of the O(n / window) extensions re-walks the whole prefix.
// Scrolling a 50k-line buffer measured 108 extensions averaging 2.35 ms, all on
// the shell thread, and showed up as a p95/max blowout (252/330 ms vs a 152 ms
// p50) rather than in the median -- the shape of a scroll hitch.
//
// Walk a viewport down a large document and count recomputes via revision().
void TestVisibleRangeExtensionGrowsResolvedPrefixGeometrically() {
  constexpr std::size_t kBodyLines = 32000;
  std::vector<std::string> lines;
  lines.reserve(kBodyLines + 2);
  lines.push_back("namespace n {");
  for (std::size_t i = 0; i < kBodyLines; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");

  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  constexpr std::size_t kBudget = 2000;
  constexpr std::size_t kWindow = 40;

  const std::size_t before = model.revision();
  for (std::size_t top = 0; top + kWindow < lines.size(); top += kWindow) {
    Expect(model.EnsureFoldsForVisibleRange(lines, options, top, top + kWindow, kBudget),
           "each scrolled resolve should finish");
  }
  const std::size_t recomputes = model.revision() - before;

  // One-window-at-a-time extension needs ~(lines - budget) / lookahead recomputes;
  // doubling from the 2000-line budget to 32002 lines needs 5. Leave headroom for
  // the look-ahead-driven first pass without leaving room for a linear policy.
  Expect(recomputes <= 12,
         "scrolling a large document must extend the fold prefix geometrically, "
         "not once per look-ahead window");

  // Geometric growth must not cost coverage: the far closer still resolves.
  const auto fold = model.FoldStartingAt(0);
  Expect(fold.has_value() && fold->closer_line == kBodyLines + 1,
         "the outer fold spanning the whole document must resolve by the last viewport");
}

// The workspace refresh gate skips recompute only when the fold model is fresh AND
// the visible range is already resolved (IsVisibleRangeResolved). On a file larger
// than the compute budget the first pass resolves only a prefix, so the predicate
// must report the far range as UNresolved — otherwise the gate would early-return on
// content-freshness alone and folds below the budget would never appear on scroll.
void TestIsVisibleRangeResolvedTracksBudgetedPrefix() {
  std::vector<std::string> lines;
  lines.reserve(5200);
  lines.push_back("namespace n {");
  for (int i = 0; i < 5000; ++i) {
    lines.push_back("  int v = 0;");
  }
  lines.push_back("}");
  for (int i = 0; i < 100; ++i) {
    lines.push_back("// tail");
  }

  FoldingModel model;
  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/0,
                                          /*visible_end_line=*/20,
                                          /*max_lines=*/2000),
         "first budgeted resolve should finish");
  Expect(model.IsVisibleRangeResolved(20),
         "the resolved prefix must report the top visible range as resolved");
  Expect(!model.IsVisibleRangeResolved(5001),
         "a range beyond the budgeted prefix must report as UNresolved so the gate keeps scanning");

  Expect(model.EnsureFoldsForVisibleRange(lines, DefaultCStyleOptions(),
                                          /*visible_start_line=*/4980,
                                          /*visible_end_line=*/5001,
                                          /*max_lines=*/2000),
         "scrolled resolve should finish");
  Expect(model.IsVisibleRangeResolved(5001),
         "after scrolling and extending the scan, the far range must report as resolved");
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

void TestHasAnyCollapsedFoldTracksCounter() {
  // 2026-05-15 perf deep-dive round 2 Finding 5: has_any_collapsed_fold() must be O(1) and stay in
  // sync with the public Collapse / Expand / ToggleFold / CollapseAll / ExpandAll mutators. The
  // wrapped-row layout build relies on this probe per edit; reverting to a linear scan would
  // reintroduce the per-edit O(fold_count) work the rewrite removed.
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
         "Collapse must update the has-any-collapsed counter");
  Expect(model.Collapse(1), "inner fold should also collapse");
  Expect(model.has_any_collapsed_fold(),
         "second Collapse must keep the counter positive");
  Expect(model.Expand(0), "outer fold should expand");
  Expect(model.has_any_collapsed_fold(),
         "after one Expand, the inner fold still keeps the counter positive");
  Expect(model.Expand(1), "inner fold should expand");
  Expect(!model.has_any_collapsed_fold(),
         "all Expand() should bring the counter back to zero");
  // ToggleFold: collapsed -> expanded round-trip
  Expect(model.ToggleFold(0), "toggle should flip outer fold");
  Expect(model.has_any_collapsed_fold(), "toggle to collapsed bumps counter");
  Expect(model.ToggleFold(0), "toggle should flip outer fold back");
  Expect(!model.has_any_collapsed_fold(), "toggle to expanded zeroes counter");
  // CollapseAll / ExpandAll
  model.CollapseAll();
  Expect(model.has_any_collapsed_fold(), "CollapseAll must set counter");
  model.ExpandAll();
  Expect(!model.has_any_collapsed_fold(), "ExpandAll must reset counter");
  model.Clear();
  Expect(!model.has_any_collapsed_fold(), "Clear must reset counter");
}

void TestRemapCollapsedFlagsPreservesCollapsedAcrossRecompute() {
  // 2026-05-15 perf deep-dive round 2 Finding 6: the O(N²) remap was rewritten to an indexed O(N)
  // loop that only walks the previously-collapsed openers. This test asserts the remap still
  // preserves collapsed state across a recompute where ranges are unchanged.
  const std::vector<std::string> lines = {
      "void f() {",
      "  void g() {",
      "    body();",
      "  }",
      "}",
  };
  FoldingModel model;
  Expect(model.Compute(lines, DefaultCStyleOptions()), "first compute should complete");
  Expect(model.Collapse(1), "inner fold collapses");
  Expect(model.has_any_collapsed_fold(), "inner fold is collapsed pre-recompute");
  // Recompute the same buffer; the inner fold must still be collapsed after the remap.
  Expect(model.Compute(lines, DefaultCStyleOptions()), "recompute should complete");
  Expect(model.has_any_collapsed_fold(),
         "remap must preserve the collapsed inner fold across a recompute");
  Expect(model.IsCollapsedAtOpener(1),
         "the specific opener that was collapsed must remain collapsed");

  // When nothing was collapsed, the recompute fast-path skips RemapCollapsedFlags entirely.
  FoldingModel fresh_model;
  Expect(fresh_model.Compute(lines, DefaultCStyleOptions()),
         "fresh recompute should still complete with the no-collapsed fast path");
  Expect(!fresh_model.has_any_collapsed_fold(),
         "fresh recompute with no prior Collapse should produce no collapsed flags");
}

}  // namespace

void RegisterFoldingModelTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "EditorFolding/Budget/PartialCompute",
          TestComputeWithBudgetFlagsPartial);
  AddTest(tests, "EditorFolding/State/ToggleHidesInterior",
          TestToggleFoldHidesInteriorLines);
  AddTest(tests, "EditorFolding/Incremental/ReusesPrefixAndCollapse",
          TestIncrementalBracketScanReusesPrefixAndCollapseState);
  AddTest(tests, "EditorFolding/Incremental/CollapsedFoldSurvivesInsertionAbove",
          TestCollapsedFoldSurvivesInsertionAbove);
  AddTest(tests, "EditorFolding/VisibleRange/ResolvesWithinBudgetWindow",
          TestEnsureFoldsForVisibleRangeResolvesWithinBudgetWindow);
  AddTest(tests, "EditorFolding/VisibleRange/ResolvesVisibleOpenerWithDistantCloser",
          TestEnsureFoldsForVisibleRangeResolvesVisibleOpenerWithDistantCloser);
  AddTest(tests, "EditorFolding/VisibleRange/BudgetGuardOnHugeFile",
          TestEnsureFoldsForVisibleRangeBudgetGuardOnHugeFile);
  AddTest(tests, "EditorFolding/VisibleRange/ExtensionGrowsResolvedPrefixGeometrically",
          TestVisibleRangeExtensionGrowsResolvedPrefixGeometrically);
  AddTest(tests, "EditorFolding/VisibleRange/IsVisibleRangeResolvedTracksBudgetedPrefix",
          TestIsVisibleRangeResolvedTracksBudgetedPrefix);
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
  AddTest(tests, "EditorFolding/HasAnyCollapsedFoldTracksCounter",
          TestHasAnyCollapsedFoldTracksCounter);
  AddTest(tests, "EditorFolding/RemapCollapsedFlagsPreservesCollapsedAcrossRecompute",
          TestRemapCollapsedFlagsPreservesCollapsedAcrossRecompute);
  AddTest(tests, "EditorFolding/StaleWithoutMarkDirtyAfterSameLineCountEdit",
          TestFoldStaleWithoutMarkDirtyAfterSameLineCountEdit);
}

}  // namespace microide::tests
