// The merge surface's row space: with `editor.wrap` on, the three panes scroll
// through ON-SCREEN rows while every downstream consumer (conflict bands, accept
// buttons, hit tests, the overview lane) still speaks conflict LINE numbers.
// `MergeVisualConflicts` is the projection between the two, and it had no tests.
//
// Its cache is the part worth pinning: it is warmed from const geometry paths
// that run every frame, so a key that misses a change leaves the bands drawn at
// stale rows -- which looks like a hit-test bug three layers away.

#include "TestSupport.h"

#include <algorithm>
#include <string>
#include <vector>

#include "workspace/MergeWrapRows.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::tests {
namespace {

using microide::workspace::MergeTabState;
using microide::workspace::MergeTrackedConflict;

// Backing storage for the model's string_views, which the model does not own.
struct MergeFixture {
  std::vector<std::string> incoming_storage;
  std::vector<std::string> current_storage;
  MergeTabState tab;

  void Build(std::vector<std::string> incoming, std::vector<std::string> current) {
    incoming_storage = std::move(incoming);
    current_storage = std::move(current);
    tab.model.incoming_lines.clear();
    tab.model.current_lines.clear();
    for (const std::string& line : incoming_storage) tab.model.incoming_lines.push_back(line);
    for (const std::string& line : current_storage) tab.model.current_lines.push_back(line);
    ++tab.model_revision;
  }
};

MergeTrackedConflict Conflict(std::size_t incoming_start,
                              std::size_t incoming_end,
                              std::size_t current_start,
                              std::size_t current_end) {
  MergeTrackedConflict conflict;
  conflict.incoming_start_line = incoming_start;
  conflict.incoming_end_line = incoming_end;
  conflict.current_start_line = current_start;
  conflict.current_end_line = current_end;
  return conflict;
}

// Wrap off: the conflicts already ARE rows, so the projection hands back the
// original vector with no copy and no cache.
void TestWrapOffReturnsTheConflictsUnchanged() {
  MergeFixture fixture;
  fixture.Build({"a", "b", "c"}, {"a", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/false, 80);

  const auto projected = microide::workspace::MergeVisualConflicts(fixture.tab);
  Expect(projected.data() == fixture.tab.conflicts.data(),
         "wrap off must hand back the conflicts themselves, not a copy");
  Expect(projected[0].incoming_start_line == 1 && projected[0].incoming_end_line == 2,
         "wrap off leaves the line numbers alone");
}

// Wrap on: a line that occupies several rows pushes every later conflict down by
// the rows it added, and a range ending at the document end covers the last
// line's final row rather than stopping short.
void TestWrapOnProjectsConflictsIntoRowSpace() {
  MergeFixture fixture;
  // At eight columns "aaaaaaaaaaaa" takes two rows; the short lines take one.
  fixture.Build({"aaaaaaaaaaaa", "b", "c"}, {"aaaaaaaaaaaa", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  fixture.tab.conflicts.push_back(Conflict(2, 3, 2, 3));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/true, 8);

  const auto projected = microide::workspace::MergeVisualConflicts(fixture.tab);
  Expect(projected.data() != fixture.tab.conflicts.data(), "wrap on projects into its own vector");
  Expect(projected[0].incoming_start_line == 2,
         "line 1 starts on row 2, after the two rows line 0 took, got " +
             std::to_string(projected[0].incoming_start_line));
  Expect(projected[0].incoming_end_line == 3, "the half-open end follows to row 3");
  Expect(projected[1].incoming_end_line == microide::workspace::MergeSourceVisualRowCount(fixture.tab),
         "a range ending at the document end covers the last line's final row");
}

// The projection follows a pane-width change. It is safe for the key to carry
// the width only through the row COUNTS because a wrap row count is monotone in
// width -- every row budget (first row `W`, continuation `W - min(indent, W/2)`)
// is non-decreasing in `W`, so an unchanged total means every line kept its own
// row count and no line moved. That argument is what this test stands on; if a
// future wrap rule breaks the monotonicity, this is where it shows up.
void TestConflictProjectionFollowsAPaneWidthChange() {
  MergeFixture fixture;
  fixture.Build({"aaaaaaaaaaaa", "bbbb", "c"}, {"aaaaaaaaaaaa", "bbbb", "C"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));

  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/true, 8);
  const std::size_t narrow_row =
      microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line;
  Expect(narrow_row == 2, "at width 8 the long first line pushes line 1 to row 2, got " +
                              std::to_string(narrow_row));

  // Nothing about the model changed -- only the pane width.
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/true, 16);
  const std::size_t wide_row =
      microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line;
  Expect(wide_row == 1, "at width 16 the first line fits on one row, so line 1 is row 1, got " +
                            std::to_string(wide_row));
}

// The cache is warmed from const geometry paths that run every frame, so it has
// to be a real cache -- a second call with nothing moved must not reproject.
// Poking the cached vector and asking again is the only way to see that from
// outside: a rebuild would overwrite the poke.
void TestConflictProjectionIsCachedBetweenIdenticalCalls() {
  MergeFixture fixture;
  fixture.Build({"aaaaaaaaaaaa", "b", "c"}, {"aaaaaaaaaaaa", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/true, 8);

  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 2,
         "the first call projects");
  fixture.tab.visual_conflicts[0].incoming_start_line = 999;
  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 999,
         "an unchanged second call is served from the cache, not reprojected");

  // ...and a model change does invalidate it.
  ++fixture.tab.model_revision;
  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 2,
         "a model revision bump reprojects");
}

// A conflict whose lines run past the end of either buffer -- a stale entry
// between a result edit and the model rebuild that follows it -- must clamp
// rather than index out of range.
void TestConflictPastTheBufferEndClampsToTheLastRow() {
  MergeFixture fixture;
  fixture.Build({"a", "b"}, {"a", "B"});
  fixture.tab.conflicts.push_back(Conflict(50, 60, 50, 60));
  fixture.tab.conflicts.back().start_line = 50;
  fixture.tab.conflicts.back().end_line = 60;
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, /*soft_wrap=*/true, 8);

  const auto projected = microide::workspace::MergeVisualConflicts(fixture.tab);
  const std::size_t source_rows = microide::workspace::MergeSourceVisualRowCount(fixture.tab);
  Expect(projected[0].incoming_start_line <= source_rows &&
             projected[0].incoming_end_line == source_rows,
         "an out-of-range source range clamps into the pane's rows");
  Expect(projected[0].end_line <=
             static_cast<std::size_t>(
                 std::max(0, fixture.tab.result_viewport.VisualRowCount())) +
                 1,
         "an out-of-range result range clamps into the result pane's rows");
}

}  // namespace

void RegisterMergeWrapRowsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MergeWrapRows/WrapOffReturnsTheConflictsUnchanged",
          TestWrapOffReturnsTheConflictsUnchanged);
  AddTest(tests, "MergeWrapRows/WrapOnProjectsConflictsIntoRowSpace",
          TestWrapOnProjectsConflictsIntoRowSpace);
  AddTest(tests, "MergeWrapRows/ConflictProjectionFollowsAPaneWidthChange",
          TestConflictProjectionFollowsAPaneWidthChange);
  AddTest(tests, "MergeWrapRows/ConflictProjectionIsCachedBetweenIdenticalCalls",
          TestConflictProjectionIsCachedBetweenIdenticalCalls);
  AddTest(tests, "MergeWrapRows/ConflictPastTheBufferEndClampsToTheLastRow",
          TestConflictPastTheBufferEndClampsToTheLastRow);
}

}  // namespace microide::tests
