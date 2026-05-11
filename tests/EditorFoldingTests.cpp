#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"

#include <limits>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::TextViewport;

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
  Expect(model.Compute(viewport.lines(), DefaultCStyleOptions()),
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
  AddTest(tests, "EditorFolding/Viewport/DetachModelRestoresVisualRows",
          TestViewportDetachFoldingModelRestoresVisualRows);
  AddTest(tests, "EditorFolding/Viewport/PartialBudgetCollapseOmitsHiddenRows",
          TestViewportPartialBudgetCollapsedFoldStillOmitsHiddenRows);
  AddTest(tests, "EditorFolding/Viewport/EditExposesFoldAnchorForIncrementalRefresh",
          TestViewportEditExposesFoldAnchorLineForIncrementalRefresh);
}

}  // namespace microide::tests
