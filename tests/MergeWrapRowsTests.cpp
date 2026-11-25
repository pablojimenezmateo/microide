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

void TestWrapOffReturnsTheConflictsUnchanged() {
  MergeFixture fixture;
  fixture.Build({"a", "b", "c"}, {"a", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, false, 80);

  const auto projected = microide::workspace::MergeVisualConflicts(fixture.tab);
  Expect(projected.data() == fixture.tab.conflicts.data(),
         "wrap off must hand back the conflicts themselves, not a copy");
  Expect(projected[0].incoming_start_line == 1 && projected[0].incoming_end_line == 2,
         "wrap off leaves the line numbers alone");
}

void TestWrapOnProjectsConflictsIntoRowSpace() {
  MergeFixture fixture;
  fixture.Build({"aaaaaaaaaaaa", "b", "c"}, {"aaaaaaaaaaaa", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  fixture.tab.conflicts.push_back(Conflict(2, 3, 2, 3));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, true, 8);

  const auto projected = microide::workspace::MergeVisualConflicts(fixture.tab);
  Expect(projected.data() != fixture.tab.conflicts.data(), "wrap on projects into its own vector");
  Expect(projected[0].incoming_start_line == 2,
         "line 1 starts on row 2, after the two rows line 0 took, got " +
             std::to_string(projected[0].incoming_start_line));
  Expect(projected[0].incoming_end_line == 3, "the half-open end follows to row 3");
  Expect(projected[1].incoming_end_line == microide::workspace::MergeSourceVisualRowCount(fixture.tab),
         "a range ending at the document end covers the last line's final row");
}

void TestConflictProjectionFollowsAPaneWidthChange() {
  MergeFixture fixture;
  fixture.Build({"aaaaaaaaaaaa", "bbbb", "c"}, {"aaaaaaaaaaaa", "bbbb", "C"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));

  microide::workspace::EnsureMergeWrapLayout(fixture.tab, true, 8);
  const std::size_t narrow_row =
      microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line;
  Expect(narrow_row == 2, "at width 8 the long first line pushes line 1 to row 2, got " +
                              std::to_string(narrow_row));

  microide::workspace::EnsureMergeWrapLayout(fixture.tab, true, 16);
  const std::size_t wide_row =
      microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line;
  Expect(wide_row == 1, "at width 16 the first line fits on one row, so line 1 is row 1, got " +
                            std::to_string(wide_row));
}

void TestConflictProjectionIsCachedBetweenIdenticalCalls() {
  MergeFixture fixture;
  fixture.Build({"aaaaaaaaaaaa", "b", "c"}, {"aaaaaaaaaaaa", "B", "c"});
  fixture.tab.conflicts.push_back(Conflict(1, 2, 1, 2));
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, true, 8);

  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 2,
         "the first call projects");
  fixture.tab.visual_conflicts[0].incoming_start_line = 999;
  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 999,
         "an unchanged second call is served from the cache, not reprojected");

  ++fixture.tab.model_revision;
  Expect(microide::workspace::MergeVisualConflicts(fixture.tab)[0].incoming_start_line == 2,
         "a model revision bump reprojects");
}

void TestConflictPastTheBufferEndClampsToTheLastRow() {
  MergeFixture fixture;
  fixture.Build({"a", "b"}, {"a", "B"});
  fixture.tab.conflicts.push_back(Conflict(50, 60, 50, 60));
  fixture.tab.conflicts.back().start_line = 50;
  fixture.tab.conflicts.back().end_line = 60;
  microide::workspace::EnsureMergeWrapLayout(fixture.tab, true, 8);

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

}

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

}
