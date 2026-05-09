#include "TestSupport.h"

#include "editor/FoldingModel.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::FoldRange;
using microide::editor::FoldSource;

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

}  // namespace

void RegisterFoldingModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorFolding/Bracket/EmitsOpenerAndCloser",
          TestBracketFoldEmitsOpenerAndCloser);
  AddTest(tests, "EditorFolding/Indent/FallsBackWhenNoBrackets",
          TestIndentFoldFallsBackWhenNoBrackets);
  AddTest(tests, "EditorFolding/Mixed/BracketWinsOnDuplicateOpener",
          TestBracketWinsOnDuplicateOpenerLine);
  AddTest(tests, "EditorFolding/Budget/PartialCompute",
          TestComputeWithBudgetFlagsPartial);
  AddTest(tests, "EditorFolding/State/ToggleHidesInterior",
          TestToggleFoldHidesInteriorLines);
}

}  // namespace microide::tests
