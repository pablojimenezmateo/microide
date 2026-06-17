#include "TestSupport.h"

#include "editor/BreakpointStore.h"

#include <filesystem>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::Breakpoint;
using microide::editor::BreakpointStore;
using microide::editor::VerifiedBreakpoint;

void TestBreakpointStoreToggleAddsAndRemoves() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";

  Expect(store.Toggle(path, 10), "first toggle should add a breakpoint");
  Expect(store.HasBreakpoint(path, 10), "breakpoint should be present after add");
  Expect(!store.Toggle(path, 10), "second toggle should remove the breakpoint");
  Expect(!store.HasBreakpoint(path, 10), "breakpoint should be gone after remove");
  Expect(store.FindByPath(path) == nullptr, "empty file should drop from the store");
}

void TestBreakpointStoreKeepsLinesSorted() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/a.py";
  store.Toggle(path, 30);
  store.Toggle(path, 5);
  store.Toggle(path, 17);

  const auto* breakpoints = store.FindByPath(path);
  Expect(breakpoints != nullptr && breakpoints->size() == 3, "three breakpoints expected");
  Expect((*breakpoints)[0].line == 5 && (*breakpoints)[1].line == 17 &&
             (*breakpoints)[2].line == 30,
         "breakpoints should be sorted ascending by line");
}

void TestBreakpointStorePathNormalizationMatches() {
  BreakpointStore store;
  store.Toggle("/tmp/project/./sub/../main.py", 3);
  Expect(store.HasBreakpoint("/tmp/project/main.py", 3),
         "normalized path spellings should resolve to the same breakpoint");
}

void TestBreakpointStoreSnapshotAllIsDeterministic() {
  BreakpointStore store;
  store.Toggle("/tmp/b.py", 1);
  store.Toggle("/tmp/a.py", 2);
  store.Toggle("/tmp/a.py", 4);

  const auto snapshot = store.SnapshotAll();
  Expect(snapshot.size() == 2, "two files expected in snapshot");
  Expect(snapshot[0].path.generic_string() == "/tmp/a.py", "snapshot should sort files by path");
  Expect(snapshot[0].breakpoints.size() == 2, "a.py should carry two breakpoints");
  Expect(snapshot[1].path.generic_string() == "/tmp/b.py", "b.py should follow a.py");
}

void TestBreakpointStoreApplyVerificationByIndex() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 5);
  store.Toggle(path, 9);

  store.ApplyVerification(path, {
                                    VerifiedBreakpoint{.id = 1, .verified = true, .line = 6},
                                    VerifiedBreakpoint{.id = 2, .verified = false, .line = 10,
                                                       .message = "no code on line"},
                                });
  const auto* breakpoints = store.FindByPath(path);
  Expect(breakpoints != nullptr && breakpoints->size() == 2, "two breakpoints expected");
  Expect((*breakpoints)[0].verified && (*breakpoints)[0].adapter_id == 1,
         "first breakpoint should be verified with adapter id 1");
  Expect(!(*breakpoints)[1].verified && (*breakpoints)[1].verify_message == "no code on line",
         "second breakpoint should carry the rejection message");

  store.ResetVerification();
  Expect(!(*store.FindByPath(path))[0].verified, "ResetVerification should clear verified flags");
}

void TestBreakpointStoreReplaceAllResetsTransientState() {
  BreakpointStore store;
  std::vector<BreakpointStore::FileBreakpoints> files;
  std::vector<Breakpoint> bps;
  bps.push_back(Breakpoint{.line = 7, .verified = true, .adapter_id = 42});
  bps.push_back(Breakpoint{.line = 2});
  files.push_back(BreakpointStore::FileBreakpoints{.path = "/tmp/x.py", .breakpoints = bps});
  store.ReplaceAll(std::move(files));

  const auto* breakpoints = store.FindByPath("/tmp/x.py");
  Expect(breakpoints != nullptr && breakpoints->size() == 2, "two breakpoints expected");
  Expect((*breakpoints)[0].line == 2 && (*breakpoints)[1].line == 7,
         "ReplaceAll should re-sort by line");
  Expect(!(*breakpoints)[1].verified && (*breakpoints)[1].adapter_id == 0,
         "ReplaceAll should drop persisted transient verification state");
}

void TestBreakpointStoreRevisionBumps() {
  BreakpointStore store;
  const std::uint64_t before = store.revision();
  store.Toggle("/tmp/r.py", 1);
  Expect(store.revision() != before, "mutation should bump the revision");
}

}  // namespace

void RegisterBreakpointStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BreakpointStore/ToggleAddsAndRemoves", TestBreakpointStoreToggleAddsAndRemoves);
  AddTest(tests, "BreakpointStore/KeepsLinesSorted", TestBreakpointStoreKeepsLinesSorted);
  AddTest(tests, "BreakpointStore/PathNormalizationMatches",
          TestBreakpointStorePathNormalizationMatches);
  AddTest(tests, "BreakpointStore/SnapshotAllIsDeterministic",
          TestBreakpointStoreSnapshotAllIsDeterministic);
  AddTest(tests, "BreakpointStore/ApplyVerificationByIndex",
          TestBreakpointStoreApplyVerificationByIndex);
  AddTest(tests, "BreakpointStore/ReplaceAllResetsTransientState",
          TestBreakpointStoreReplaceAllResetsTransientState);
  AddTest(tests, "BreakpointStore/RevisionBumps", TestBreakpointStoreRevisionBumps);
}

}  // namespace microide::tests
