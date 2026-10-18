#include "TestSupport.h"

#include "workspace/state/EditorNavigationHistory.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::EditorNavigationHistory;

const auto kAlwaysExists = [](const std::filesystem::path&) { return true; };

std::string Describe(const std::optional<EditorNavigationHistory::Location>& location) {
  return location.has_value() ? location->path.string() + ":" + std::to_string(location->line)
                              : std::string("nothing");
}

// VS Code's rule: another file, or more than ten lines away, is a new place; a
// smaller move updates the current one, so going back returns to where you last
// were in a file.
void TestSmallMovesUpdateTheCurrentEntry() {
  EditorNavigationHistory history;
  history.Observe("/p/a.cpp", 0, 0);
  history.Observe("/p/a.cpp", 4, 2);
  history.Observe("/p/a.cpp", 9, 0);
  Expect(history.entries().size() == 1, "moves within ten lines should not add entries");
  history.Observe("/p/a.cpp", 30, 0);
  history.Observe("/p/b.cpp", 30, 0);
  Expect(history.entries().size() == 3, "a far jump and another file should each add one");
  const auto back = history.Step(-1, kAlwaysExists);
  Expect(back.has_value() && back->path == "/p/a.cpp" && back->line == 30,
         "back should land on a.cpp:30, got " + Describe(back));
  const auto first = history.Step(-1, kAlwaysExists);
  Expect(first.has_value() && first->line == 9 && first->path == "/p/a.cpp",
         "back again should land where the caret last was near the start (9), got " +
             Describe(first));
  Expect(!history.Step(-1, kAlwaysExists).has_value(), "there is nothing before the first entry");
}

void TestANewPlaceDropsTheForwardHistory() {
  EditorNavigationHistory history;
  history.Observe("/p/a.cpp", 0, 0);
  history.Observe("/p/b.cpp", 0, 0);
  history.Observe("/p/c.cpp", 0, 0);
  (void)history.Step(-1, kAlwaysExists);
  (void)history.Step(-1, kAlwaysExists);
  Expect(history.CanGoForward(), "after going back there is somewhere to go forward to");
  history.Observe("/p/a.cpp", 0, 0);  // where Step put the caret: not a new place
  Expect(history.CanGoForward(), "landing where the step went keeps the forward history");
  history.Observe("/p/d.cpp", 0, 0);
  Expect(!history.CanGoForward() && history.entries().size() == 2,
         "a new place drops everything ahead of the cursor");
}

void TestStepSkipsFilesThatAreGone() {
  EditorNavigationHistory history;
  history.Observe("/p/a.cpp", 0, 0);
  history.Observe("/p/gone.cpp", 0, 0);
  history.Observe("/p/c.cpp", 0, 0);
  const auto exists = [](const std::filesystem::path& path) { return path != "/p/gone.cpp"; };
  const auto back = history.Step(-1, exists);
  Expect(back.has_value() && back->path == "/p/a.cpp",
         "back should skip the deleted file, got " + Describe(back));
  Expect(history.entries().size() == 2, "the deleted file's entry should be dropped");
  const auto forward = history.Step(1, exists);
  Expect(forward.has_value() && forward->path == "/p/c.cpp",
         "forward should come back to c.cpp, got " + Describe(forward));
}

void TestHistoryIsBounded() {
  EditorNavigationHistory history;
  for (std::size_t i = 0; i < EditorNavigationHistory::kMaxEntries + 10; ++i) {
    history.Observe("/p/a.cpp", i * 100, 0);
  }
  Expect(history.entries().size() == EditorNavigationHistory::kMaxEntries,
         "the history should keep at most kMaxEntries places");
  Expect(history.entries().back().line == (EditorNavigationHistory::kMaxEntries + 9) * 100,
         "the newest place should be kept");
}

}  // namespace

void RegisterEditorNavigationHistoryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorNavigationHistory/SmallMovesUpdateTheCurrentEntry",
          TestSmallMovesUpdateTheCurrentEntry);
  AddTest(tests, "EditorNavigationHistory/ANewPlaceDropsTheForwardHistory",
          TestANewPlaceDropsTheForwardHistory);
  AddTest(tests, "EditorNavigationHistory/StepSkipsFilesThatAreGone", TestStepSkipsFilesThatAreGone);
  AddTest(tests, "EditorNavigationHistory/HistoryIsBounded", TestHistoryIsBounded);
}

}  // namespace microide::tests
