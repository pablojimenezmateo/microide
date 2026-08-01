#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceTabState.h"

#include <limits>
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
  for (const editor::FoldRange& range : model.ranges()) {
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

// Partial `ComputeWithBudget` still yields actionable ranges; viewport honors collapsed hiding
// for resolved folds while `complete()==false` (§5.12 / §5.13 partial fallback).
void TestViewportPartialBudgetCollapsedFoldStillOmitsHiddenRows() {
  const std::vector<std::string> lines = {
      "void f() {",
      "  a;",
      "}",
      "void g() {",
      "  b;",
      "}",
  };

  FoldingModel model;
  Expect(!model.ComputeWithBudget(lines, DefaultCStyleOptions(), /*max_lines=*/3),
         "tight line budget should leave the model incomplete");
  Expect(!model.complete(), "partial fallback should report incomplete compute");
  Expect(!model.ranges().empty(),
         "partial bracket scan should still emit folds within the visited prefix");

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
  Expect(!editor_state.folding_model->ranges().empty(),
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

  for (const auto& range : editor_state.folding_model->ranges()) {
    Expect(range.source != microide::editor::FoldSource::Bracket,
           "markdown prose parentheticals must not emit bracket fold ranges");
  }
}

void TestViewportEditExposesFoldAnchorLineForIncrementalRefresh() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\ndd\n", "/tmp/fold-anchor.cpp");
  viewport.MoveCursorTo(2, 0);
  viewport.InsertText("Z");

  Expect(viewport.ConsumeFoldEditAnchorLine() == 2u,
         "fold anchor should match the first edited logical line");
  Expect(viewport.ConsumeFoldEditAnchorLine() == std::numeric_limits<std::size_t>::max(),
         "fold anchor should reset to the idle sentinel after consume");
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
