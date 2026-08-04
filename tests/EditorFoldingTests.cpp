#include "TestSupport.h"

#include "FoldingReference.h"
#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/state/WorkspaceTabState.h"

#include <functional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::TextViewport;
using microide::workspace::EnsureFoldingModelFresh;
using microide::workspace::LanguageContract;
using microide::workspace::TabEntry;

FoldingModel::ComputeOptions DefaultCStyleOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}, {'(', ')'}, {'[', ']'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

std::string JoinLinesWithTrailingNewline(const std::vector<std::string>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      out.push_back('\n');
    }
    out += lines[i];
  }
  out.push_back('\n');
  return out;
}

// Viewport-specific complement to `EditorEssentials/Folding/RefreshDisabledExpandsAndClears`:
// clearing the attached model immediately restores full logical-row visibility.
void TestViewportDetachFoldingModelRestoresVisualRows() {
  TextViewport viewport;
  viewport.LoadContent("void f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/editor-fold-detach.cpp");

  FoldingModel model;
  Expect(model.Compute(viewport.lines().Snapshot(), DefaultCStyleOptions()),
         "fold compute should succeed for detach fixture");
  Expect(model.Collapse(0), "outer fold should collapse");
  viewport.SetFoldingModel(&model);

  const int collapsed_visual_rows = viewport.VisualRowCount();
  Expect(collapsed_visual_rows < static_cast<int>(viewport.line_count()),
         "collapsed fold should shrink visible-row count vs logical lines");

  viewport.SetFoldingModel(nullptr);
  Expect(viewport.VisualRowCount() == static_cast<int>(viewport.line_count()),
         "detaching the folding model should expose every logical line again");
}

// The memoised per-block prefix state is what lets a keystroke skip re-deriving
// which brackets are open above it. It is invalidated from the edited block
// onwards, and getting that wrong produces WRONG folds, not a crash — so drive a
// document through a full recompute onto entirely different content and then a
// localized edit at the same line, and diff against the cache-free reference.
void TestFoldingPrefixStateDoesNotSurviveAFullRecompute() {
  // Line 7 sits two brackets deep here...
  std::vector<std::string> deep;
  for (int block = 0; block < 6; ++block) {
    deep.push_back("void f" + std::to_string(block) + "() {");
    deep.push_back("  if (x) {");
    deep.push_back("    body();");
    deep.push_back("  }");
    deep.push_back("}");
  }
  // ...and one bracket deep here, opened at a DIFFERENT line. That difference is
  // the whole point: if both documents happened to have the same bracket open at
  // line 7, reusing the wrong state would emit the same ranges by luck and prove
  // nothing.
  std::vector<std::string> flat(deep.size(), "  body();");
  flat[0] = "void a() {";
  flat[1] = "  x();";
  flat[2] = "}";
  flat[3] = "void b() {";
  flat[flat.size() - 1] = "}";

  const auto options = DefaultCStyleOptions();
  FoldingModel model;
  Expect(model.Compute(deep, options), "seed compute on the deep document");
  deep[7] = "    body(); more();";
  model.Refresh(deep, options, 0, FoldingModel::kAllLines, /*max_lines=*/0,
                editor::LineEditSpan::SuffixReplacedFrom(7));

  // A full recompute on entirely different content — what a document load does.
  Expect(model.Compute(flat, options), "full recompute on the flat document");

  // Now edit at the same line on the new content.
  flat[7] = "  body(); more();";
  model.Refresh(flat, options, 0, FoldingModel::kAllLines, /*max_lines=*/0,
                editor::LineEditSpan::SuffixReplacedFrom(7));

  const std::vector<editor::FoldRange> reference = ReferenceFolds(flat, options);
  Expect(model.resolved_ranges().size() == reference.size(),
         "an edit after a full recompute must not reuse the previous document's state");
  for (std::size_t i = 0; i < reference.size() && i < model.resolved_ranges().size(); ++i) {
    Expect(model.resolved_ranges()[i].opener_line == reference[i].opener_line &&
               model.resolved_ranges()[i].closer_line == reference[i].closer_line,
           "fold ranges after a cross-document edit must match the reference computation");
  }
}

// The indent scan treats "blank or indent only" as a property of the measured
// indent (a sentinel value) rather than re-scanning the line's bytes. Those are
// the same predicate — MeasureIndent returns the sentinel exactly when every byte
// is a space or a tab — but "exactly" is worth a test, because the interesting
// input is the one that is neither empty nor code: a line of pure whitespace.
//
// A whitespace-only line must not open a fold, and must not terminate the block
// it sits inside (it is skipped, not treated as a dedent).
void TestFoldingWhitespaceOnlyLinesAreNeitherOpenersNorDedents() {
  const std::vector<std::string> lines = {
      "def outer():",   // 0: opener
      "    first()",    // 1: body
      "      \t  ",      // 2: whitespace only, indented deeper than the body
      "    second()",   // 3: body again — the block must not have ended at line 2
      "",               // 4: empty line
      "    third()",    // 5: still the same block
      "done()",         // 6: the real dedent
  };

  FoldingModel model;
  editor::FoldingModel::ComputeOptions options;
  options.tab_size = 4;
  options.use_indent_source = true;  // no bracket pairs: indent folds only
  Expect(model.Compute(lines, options), "indent-only fold compute should complete");

  bool found_outer = false;
  for (const editor::FoldRange& range : model.resolved_ranges()) {
    Expect(range.opener_line != 2 && range.opener_line != 4,
           "a whitespace-only line must never be a fold opener");
    if (range.opener_line == 0) {
      found_outer = true;
      Expect(range.closer_line == 5,
             "whitespace-only lines inside a block must not end it early — the block runs "
             "to the last indented line before the real dedent");
    }
  }
  Expect(found_outer, "the indented block under line 0 should produce a fold");
}

// Attaching or detaching a fold model changes which lines are visible, never how
// wide any line is, so it must not wipe the width-derived layout caches. It used
// to call InvalidateVisualColumnCache(), and the very next statement
// (ClampScrollState -> MaxVisualColumns) then rebuilt the per-line width table
// from scratch: a full O(lines) walk, ~10 ms of shell-thread stall on a 50k-line
// buffer, paid the first time a fold scan resolved any range.
//
// Pin it by cache behavior, not by timing: the visible-line layout cache must
// still serve the same keys after the model is attached and after it is detached.
void TestViewportAttachFoldingModelKeepsWidthCaches() {
  TextViewport viewport;
  viewport.LoadContent("void f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/editor-fold-cache.cpp");

  const auto warm_and_count_hits = [&viewport]() {
    viewport.ResetCacheStats();
    for (std::size_t line = 0; line < viewport.line_count(); ++line) {
      (void)viewport.VisibleLineLayoutRef(line);
    }
    return viewport.CacheStats().visible_line_hits;
  };

  (void)warm_and_count_hits();
  const std::size_t warm_hits = warm_and_count_hits();
  Expect(warm_hits == viewport.line_count(),
         "a warmed visible-line cache must serve every repeat query — otherwise this test "
         "cannot tell a preserved cache from a wiped one");

  FoldingModel model;
  Expect(model.Compute(viewport.lines().Snapshot(), DefaultCStyleOptions()),
         "fold compute should succeed for the cache fixture");
  viewport.SetFoldingModel(&model);
  Expect(warm_and_count_hits() == viewport.line_count(),
         "attaching a fold model must not invalidate the visible-line layout cache: folds "
         "change which lines are visible, not how wide any line is");

  viewport.SetFoldingModel(nullptr);
  Expect(warm_and_count_hits() == viewport.line_count(),
         "detaching a fold model must not invalidate the visible-line layout cache either");
}

// A partial (budget-limited) resolve still yields actionable ranges; the viewport
// honors collapsed hiding for resolved folds while `complete() == false`
// (§5.12 / §5.13 partial fallback).
void TestViewportPartialBudgetCollapsedFoldStillOmitsHiddenRows() {
  std::vector<std::string> lines = {
      "void f() {",
      "  a;",
      "}",
  };
  for (int i = 0; i < 400; ++i) {
    lines.push_back("void g" + std::to_string(i) + "() {");
    lines.push_back("  b;");
    lines.push_back("}");
  }

  FoldingModel model;
  Expect(!model.Refresh(lines, DefaultCStyleOptions(), /*first_line=*/0, /*last_line=*/2,
                        /*max_lines=*/8),
         "a tight line budget should leave the model incomplete");
  Expect(!model.complete(), "partial fallback should report incomplete resolve");
  Expect(!model.resolved_ranges().empty(),
         "a partial resolve should still emit the folds it did reach");

  TextViewport viewport;
  viewport.LoadContent(JoinLinesWithTrailingNewline(lines), "/tmp/editor-fold-partial.cpp");
  Expect(model.Collapse(0), "first resolved fold should accept collapse");
  viewport.SetFoldingModel(&model);

  Expect(viewport.VisualRowCount() < static_cast<int>(viewport.line_count()),
         "collapsed resolved fold should still omit interior logical rows in the viewport");
}

// Host-side incremental folding consults `ConsumeFoldEditAnchorLine()` after edits (see
// `WorkspaceFoldingRefresh`); assert the viewport publishes the minimum invalidated line.
LanguageContract MakeCStyleFoldContract() {
  LanguageContract contract;
  contract.language_id = "cpp";
  contract.bracket_pairs.push_back({"{", "}"});
  contract.bracket_pairs.push_back({"(", ")"});
  contract.bracket_pairs.push_back({"[", "]"});
  return contract;
}

TabEntry::EditorTabState MakeFoldingEditorTab() {
  return TabEntry::EditorTabState{};
}

void TestFoldingModelPointerStableAcrossTabVectorReallocation() {
  std::vector<TabEntry> tabs;
  tabs.reserve(1);

  TabEntry first_tab;
  first_tab.kind = TabEntry::Kind::Editor;
  first_tab.editor_state = MakeFoldingEditorTab();
  auto& editor_state = *first_tab.editor_state;
  auto& viewport = editor_state.viewport;
  viewport.LoadContent("void f() {\n  body();\n}\n", "/tmp/fold-stable.cpp");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);
  const auto contract = MakeCStyleFoldContract();
  EnsureFoldingModelFresh(editor_state, viewport, &contract, 4, true, viewport.visible_lines());
  Expect(!editor_state.folding_model->resolved_ranges().empty(),
         "fixture should expose at least one fold range");
  Expect(editor_state.folding_model->Collapse(0),
         "fixture should collapse the top-level fold");
  const void* model_ptr_before = editor_state.folding_model.get();
  viewport.SetFoldingModel(editor_state.folding_model.get());
  const int collapsed_visual_rows = viewport.VisualRowCount();
  Expect(collapsed_visual_rows < static_cast<int>(viewport.line_count()),
         "collapsed fold should hide interior logical rows before reallocation");

  tabs.push_back(std::move(first_tab));
  for (int i = 0; i < 32; ++i) {
    TabEntry filler;
    filler.kind = TabEntry::Kind::Editor;
    filler.editor_state = MakeFoldingEditorTab();
    tabs.push_back(std::move(filler));
  }

  auto& stored = *tabs.front().editor_state;
  Expect(stored.folding_model.get() == model_ptr_before,
         "heap folding model address must stay stable across tab vector reallocation");
  Expect(stored.folding_model->IsLineHidden(1),
         "collapsed fold visibility must remain valid after tab vector reallocation");
  // Moving the editor tab (vector reallocation) moves the contained TextViewport,
  // which intentionally drops its non-owning folding-model binding (see
  // TestTextViewportMoveClearsFoldingModelBinding). The render loop rebinds every
  // frame via EnsureActiveFoldingModelFresh; emulate that and confirm the stable
  // heap model still drives the collapsed layout.
  auto& rebound_viewport = stored.viewport;
  rebound_viewport.SetFoldingModel(stored.folding_model.get());
  Expect(rebound_viewport.VisualRowCount() == collapsed_visual_rows,
         "rebinding the stable folding model after reallocation must restore collapsed layout");
}

void TestTextViewportCopyClearsFoldingModelBinding() {
  TextViewport original;
  original.LoadContent("void f() {\n  a();\n}\n", "/tmp/fold-copy.cpp");
  FoldingModel model;
  Expect(model.Compute(original.lines().Snapshot(), DefaultCStyleOptions()),
         "fold compute should succeed for copy fixture");
  Expect(model.Collapse(0), "outer fold should collapse");
  original.SetFoldingModel(&model);
  const int collapsed_visual_rows = original.VisualRowCount();

  const TextViewport copy = original;
  Expect(copy.folding_revision() == 0,
         "copied viewport must not retain a non-owning folding model pointer");
  Expect(copy.VisualRowCount() == static_cast<int>(copy.line_count()),
         "copied viewport must show every logical row without a folding rebind");
  Expect(original.VisualRowCount() == collapsed_visual_rows,
         "source viewport folding binding should remain intact after copy");
}

void TestTextViewportMoveClearsFoldingModelBinding() {
  TextViewport original;
  original.LoadContent("void f() {\n  a();\n}\n", "/tmp/fold-move.cpp");
  FoldingModel model;
  Expect(model.Compute(original.lines().Snapshot(), DefaultCStyleOptions()),
         "fold compute should succeed for move fixture");
  Expect(model.Collapse(0), "outer fold should collapse");
  original.SetFoldingModel(&model);
  Expect(original.VisualRowCount() < static_cast<int>(original.line_count()),
         "move fixture should start from a collapsed fold layout");

  TextViewport moved = std::move(original);
  Expect(moved.folding_revision() == 0,
         "moved-to viewport must not retain a non-owning folding model pointer");
  Expect(moved.VisualRowCount() == static_cast<int>(moved.line_count()),
         "moved-to viewport must show every logical row without a folding rebind");
  Expect(original.folding_revision() == 0,
         "moved-from viewport must clear its folding model pointer");
  Expect(original.VisualRowCount() == static_cast<int>(original.line_count()),
         "moved-from viewport must not keep collapsed-row layout state");
}

// Regression: markdown prose freely breaks parentheticals across lines. Those
// must NOT produce bracket fold markers (the markdown contract ships no
// fold-driving bracket pairs). Indent-based folds are still allowed.
void TestMarkdownProseParensDoNotFold() {
  TabEntry::EditorTabState editor_state = MakeFoldingEditorTab();
  auto& viewport = editor_state.viewport;
  // Each paragraph opens a paren that closes on the following line — exactly the
  // open-questions.md shape that produced bogus fold markers.
  viewport.LoadContent(
      "## Heading\n"
      "Which capabilities are subscription-only (Planner / Expert Modeler /\n"
      "Operations / Decision Owner) across tiers?\n"
      "When resolved, record the decision (ADR for engineering, a business\n"
      "page for the rest) and append to the log.\n",
      "/tmp/fold-markdown-prose.md");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);

  LanguageContract contract;
  contract.language_id = "markdown";  // mirrors the default contract: no bracket pairs.

  EnsureFoldingModelFresh(editor_state, viewport, &contract, 4, true, viewport.visible_lines());

  for (const auto& range : editor_state.folding_model->resolved_ranges()) {
    Expect(range.source != microide::editor::FoldSource::Bracket,
           "markdown prose parentheticals must not emit bracket fold ranges");
  }
}

// End-to-end guard for the incremental fold caches. The per-line indent and
// bracket caches are spliced against the LineEditSpan the viewport reports, so a
// wrong splice shows up as wrong FOLDS, not a crash -- and the existing resume
// test only ever rewrites a line in place, which exercises the splice's easy case
// (removed == inserted) and never moves a cached entry.
//
// Drive real viewport edits that change the line count in both directions
// (newline splits, backspace joins, multi-line paste, selection delete) and diff
// the incrementally maintained model against a from-scratch compute after every
// one. The span comes from the real edit path, so this covers the whole chain:
// edit -> reported splice -> cache resync -> event match.
void TestFoldingIncrementalCachesSurviveLineCountChanges() {
  std::vector<std::string> seed;
  for (int block = 0; block < 10; ++block) {
    seed.push_back("void f" + std::to_string(block) + "() {");
    seed.push_back("  if (x) {");
    seed.push_back("    body();");
    seed.push_back("  }");
    seed.push_back("}");
  }

  TextViewport viewport;
  viewport.LoadContent(JoinLinesWithTrailingNewline(seed), "/tmp/fold-splice.cpp");
  const auto options = DefaultCStyleOptions();

  FoldingModel incremental;
  Expect(incremental.Refresh(viewport.lines().Snapshot(), options, 0, FoldingModel::kAllLines,
                             /*max_lines=*/0, viewport.ConsumeFoldEditSpan()),
         "seed compute should complete");

  // Each step mutates the buffer in a way that moves the lines below it.
  const std::vector<std::pair<const char*, std::function<void(TextViewport&)>>> steps = {
      {"split a mid-file body line",
       [](TextViewport& v) { v.MoveCursorTo(22, 8); v.InsertNewline(); }},
      {"join it back",
       [](TextViewport& v) { v.MoveCursorTo(23, 0); v.Backspace(); }},
      {"paste a nested block above the middle",
       [](TextViewport& v) {
         v.MoveCursorTo(10, 0);
         v.InsertText("void extra() {\n  while (y) {\n    step();\n  }\n}\n");
       }},
      {"open a brace near the top",
       [](TextViewport& v) { v.MoveCursorTo(2, 0); v.InsertText("  {\n"); }},
      {"delete a whole line low in the file",
       [](TextViewport& v) {
         v.MoveCursorTo(40, 0);
         v.MoveCursorTo(41, 0, /*extend_selection=*/true);
         v.InsertText("");  // replaces the selected range with nothing
       }},
      {"insert a closer that rebalances the top brace",
       [](TextViewport& v) { v.MoveCursorTo(6, 0); v.InsertText("  }\n"); }},
      {"edit a line in place after all the shifting",
       [](TextViewport& v) { v.MoveCursorTo(30, 0); v.InsertText("// note "); }},
  };

  for (const auto& [label, apply] : steps) {
    apply(viewport);
    const std::vector<std::string> snapshot = viewport.lines().Snapshot();
    incremental.Refresh(snapshot, options, 0, FoldingModel::kAllLines, /*max_lines=*/0,
                        viewport.ConsumeFoldEditSpan());

    // The cache-free reference, not a second FoldingModel: a model-vs-model diff
    // cannot see a derivation bug that both sides share.
    const std::vector<editor::FoldRange> reference = ReferenceFolds(snapshot, options);

    const std::string context = std::string("after ") + label;
    Expect(incremental.resolved_ranges().size() == reference.size(),
           context + ": incremental fold count must match the reference computation");
    for (std::size_t i = 0; i < reference.size() && i < incremental.resolved_ranges().size(); ++i) {
      Expect(incremental.resolved_ranges()[i].opener_line == reference[i].opener_line &&
                 incremental.resolved_ranges()[i].closer_line == reference[i].closer_line &&
                 incremental.resolved_ranges()[i].source == reference[i].source,
             context + ": a spliced per-line cache must yield identical fold ranges");
    }
  }
}

void TestViewportEditExposesFoldAnchorLineForIncrementalRefresh() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\ndd\n", "/tmp/fold-anchor.cpp");
  // A load replaces every line, so it reports a full rebuild -- the fold model's
  // per-line caches hold the *previous* document until something resyncs them.
  // Consume it the way the first frame after a load does, so what follows
  // measures the edit alone.
  Expect(viewport.ConsumeFoldEditSpan().begin() == 0u,
         "loading a document should invalidate the whole fold span");
  viewport.MoveCursorTo(2, 0);
  viewport.InsertText("Z");

  const editor::LineEditSpan span = viewport.ConsumeFoldEditSpan();
  Expect(span.begin() == 2u, "fold edit span should start at the first edited logical line");
  Expect(span.ResolvedCurrentEnd(4) == 3u,
         "a one-line in-place insert should report a one-line window, not the whole tail");
  Expect(viewport.ConsumeFoldEditSpan().empty(),
         "the fold edit span should be empty again after consume");
}

}  // namespace

void RegisterEditorFoldingTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorFolding/TabState/ModelPointerStableAcrossVectorReallocation",
          TestFoldingModelPointerStableAcrossTabVectorReallocation);
  AddTest(tests, "EditorFolding/Viewport/CopyClearsFoldingModelBinding",
          TestTextViewportCopyClearsFoldingModelBinding);
  AddTest(tests, "EditorFolding/Viewport/MoveClearsFoldingModelBinding",
          TestTextViewportMoveClearsFoldingModelBinding);
  AddTest(tests, "EditorFolding/Viewport/DetachModelRestoresVisualRows",
          TestViewportDetachFoldingModelRestoresVisualRows);
  AddTest(tests, "EditorFolding/PrefixStateDoesNotSurviveAFullRecompute",
          TestFoldingPrefixStateDoesNotSurviveAFullRecompute);
  AddTest(tests, "EditorFolding/IncrementalCachesSurviveLineCountChanges",
          TestFoldingIncrementalCachesSurviveLineCountChanges);
  AddTest(tests, "EditorFolding/WhitespaceOnlyLinesAreNeitherOpenersNorDedents",
          TestFoldingWhitespaceOnlyLinesAreNeitherOpenersNorDedents);
  AddTest(tests, "EditorFolding/Viewport/AttachModelKeepsWidthCaches",
          TestViewportAttachFoldingModelKeepsWidthCaches);
  AddTest(tests, "EditorFolding/Viewport/PartialBudgetCollapseOmitsHiddenRows",
          TestViewportPartialBudgetCollapsedFoldStillOmitsHiddenRows);
  AddTest(tests, "EditorFolding/Viewport/EditExposesFoldAnchorForIncrementalRefresh",
          TestViewportEditExposesFoldAnchorLineForIncrementalRefresh);
  AddTest(tests, "EditorFolding/Markdown/ProseParensDoNotFold",
          TestMarkdownProseParensDoNotFold);
}

}  // namespace microide::tests
