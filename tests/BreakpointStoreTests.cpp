#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "editor/EditTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::AppliedEdit;
using microide::editor::Breakpoint;
using microide::editor::BreakpointStore;
using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::VerifiedBreakpoint;

// Build an AppliedEdit that replaced [start_line,start_col .. end_line,end_col)
// with `replacement`.
AppliedEdit MakeEdit(std::size_t start_line, std::size_t start_col, std::size_t end_line,
                     std::size_t end_col, std::string replacement) {
  return AppliedEdit{SelectionRange{TextPosition{start_line, start_col},
                                    TextPosition{end_line, end_col}},
                     std::move(replacement)};
}

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

// The verification race fix: a setBreakpoints response is positional to the
// REQUEST, so the caller passes each result's requested line. Matching by that
// line (not by current-store index) means a stale response — sent before the user
// toggled another breakpoint in the same file — still lands on the right lines.
void TestBreakpointStoreApplyVerificationMatchesByLineNotIndex() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  // Current store: 0-based lines 20 and 30 (the user removed line 10 a moment ago).
  store.Toggle(path, 20);
  store.Toggle(path, 30);

  // A stale response for the OLD [10,20,30] request (1-based lines 11,21,31). The
  // first result is for the now-removed line; index-matching would have shifted it
  // onto line 20.
  store.ApplyVerification(
      path, {
                VerifiedBreakpoint{.id = 1, .verified = false, .line = 11, .message = "gone"},
                VerifiedBreakpoint{.id = 2, .verified = true, .line = 21},
                VerifiedBreakpoint{.id = 3, .verified = true, .line = 31},
            });

  const auto* bps = store.FindByPath(path);  // sorted ascending: [20, 30]
  Expect(bps != nullptr && bps->size() == 2, "two breakpoints expected");
  Expect((*bps)[0].line == 20 && (*bps)[0].verified && (*bps)[0].adapter_id == 2,
         "line 20 should match by requested line (id 2), not by index");
  Expect((*bps)[0].verify_message.empty(),
         "the removed-line result must not bleed onto a neighbouring breakpoint");
  Expect((*bps)[1].line == 30 && (*bps)[1].verified && (*bps)[1].adapter_id == 3,
         "line 30 should match by requested line (id 3)");
}

// Verification against a large sorted breakpoint set must land each result on the
// breakpoint at its requested line (the binary search replaces the O(n) scan).
void TestBreakpointStoreApplyVerificationManyLines() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/big.py";
  constexpr std::size_t kCount = 4000;
  for (std::size_t i = 0; i < kCount; ++i) {
    store.Toggle(path, i * 2);  // even 0-based lines 0,2,4,...
  }
  // Verify a scattered subset. VerifiedBreakpoint.line is the 1-based requested line.
  store.ApplyVerification(
      path, {
                VerifiedBreakpoint{.id = 1, .verified = true, .line = 1},        // 0-based 0
                VerifiedBreakpoint{.id = 2, .verified = true, .line = 4001},     // 0-based 4000
                VerifiedBreakpoint{.id = 3, .verified = false, .line = 7999,
                                   .message = "no code"},                        // 0-based 7998
                VerifiedBreakpoint{.id = 4, .verified = true, .line = 5000},     // 0-based 4999: odd, absent
            });
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == kCount, "all breakpoints retained");
  const auto find = [&](std::size_t line) -> const Breakpoint* {
    for (const auto& bp : *bps) {
      if (bp.line == line) {
        return &bp;
      }
    }
    return nullptr;
  };
  Expect(find(0)->verified && find(0)->adapter_id == 1, "line 0 verifies by requested line");
  Expect(find(4000)->verified && find(4000)->adapter_id == 2, "line 4000 verifies by requested line");
  Expect(!find(7998)->verified && find(7998)->verify_message == "no code",
         "line 7998 reflects its rejection");
  // The odd requested line 4999 has no breakpoint; it must not clobber a neighbour.
  Expect(find(4998)->adapter_id == 0 && find(5000)->adapter_id == 0,
         "an absent requested line does not bleed onto neighbours");
}

// The async DAP `breakpoint` event updates verification after the initial
// response: matched by the adapter id assigned at setBreakpoints time.
void TestBreakpointStoreApplyBreakpointEvent() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 20);  // 0-based line 20 → 1-based 21
  store.ApplyVerification(path, {VerifiedBreakpoint{.id = 7, .verified = false, .line = 21,
                                                    .message = "pending bind"}});
  Expect(!(*store.FindByPath(path))[0].verified, "starts unverified");

  // The adapter later binds the breakpoint and emits a `breakpoint` event (id 7).
  store.ApplyBreakpointEvent(path, VerifiedBreakpoint{.id = 7, .verified = true, .line = 21});
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && (*bps)[0].verified && (*bps)[0].verify_message.empty(),
         "the breakpoint event should flip the breakpoint to verified");

  // With no source path on the event, the id still locates it across files.
  store.ApplyBreakpointEvent({}, VerifiedBreakpoint{.id = 7, .verified = false, .line = 21,
                                                    .message = "invalidated"});
  Expect(!(*store.FindByPath(path))[0].verified &&
             (*store.FindByPath(path))[0].verify_message == "invalidated",
         "a path-less event should match by adapter id across all files");
}

// Regression: a `breakpoint` event whose id belongs to a *function* breakpoint
// (gdb resolves `break foo` to a concrete source line and emits an event carrying
// that line) must not clobber a line breakpoint that happens to sit at the same
// resolved line. The line fallback only binds still-unbound breakpoints.
void TestBreakpointStoreEventForeignIdDoesNotClobberBoundBreakpoint() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.cpp";
  store.Toggle(path, 9);  // 0-based line 9 → 1-based 10
  // Bind it to adapter id 5 (as setBreakpoints would).
  store.ApplyVerification(path, {VerifiedBreakpoint{.id = 5, .verified = true, .line = 10}});
  Expect((*store.FindByPath(path))[0].adapter_id == 5, "line breakpoint is bound to id 5");

  // A function breakpoint (id 99) resolves to the same line and emits an event.
  store.ApplyBreakpointEvent(
      path, VerifiedBreakpoint{.id = 99, .verified = false, .line = 10, .message = "func bp"});

  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == 1, "the line breakpoint still exists");
  Expect((*bps)[0].adapter_id == 5,
         "a foreign-id event must not rewrite the bound breakpoint's adapter id");
  Expect((*bps)[0].verified && (*bps)[0].verify_message.empty(),
         "a foreign-id event must not overwrite the bound breakpoint's verify state");

  // But an event carrying the still-unbound breakpoint's real id still binds it.
  store.Toggle(path, 20);  // a fresh, unbound breakpoint at 1-based line 21
  store.ApplyBreakpointEvent(path,
                             VerifiedBreakpoint{.id = 42, .verified = true, .line = 21});
  const auto* after = store.FindByPath(path);
  const auto bound = std::find_if(after->begin(), after->end(),
                                  [](const auto& bp) { return bp.line == 20; });
  Expect(bound != after->end() && bound->adapter_id == 42 && bound->verified,
         "the line fallback still binds an as-yet-unbound breakpoint");
}

void TestBreakpointStoreToggleEnabled() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  Expect(!store.ToggleEnabled(path, 5), "toggling a non-existent breakpoint is a no-op");
  store.Toggle(path, 5);
  Expect((*store.FindByPath(path))[0].enabled, "a new breakpoint is enabled");
  Expect(store.ToggleEnabled(path, 5), "toggling an existing breakpoint returns true");
  Expect(!(*store.FindByPath(path))[0].enabled, "ToggleEnabled disables an enabled breakpoint");
  Expect(store.ToggleEnabled(path, 5) && (*store.FindByPath(path))[0].enabled,
         "toggling again re-enables it");
  Expect(store.HasBreakpoint(path, 5), "ToggleEnabled never removes the breakpoint");
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

void TestBreakpointStoreModifierSetters() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/mod.py";

  // Setting a modifier on a bare line materializes the breakpoint (so the
  // context menu's "Set Condition…" doubles as "Add Conditional Breakpoint").
  store.SetCondition(path, 12, "i > 5");
  Expect(store.HasBreakpoint(path, 12), "setting a condition creates the breakpoint");
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == 1 && (*bps)[0].condition == std::optional<std::string>("i > 5"),
         "condition is stored on the breakpoint");

  store.SetHitCondition(path, 12, ">10");
  store.SetLogMessage(path, 12, "value={x}");
  const auto* after = store.FindByPath(path);
  Expect(after != nullptr && (*after)[0].hit_condition == std::optional<std::string>(">10") &&
             (*after)[0].log_message == std::optional<std::string>("value={x}"),
         "hit condition + log message are stored");

  // A nullopt clears the field (an empty prompt commit / "Clear Condition"). The
  // MATLAB-style menu clears only the condition; hit-count + log message persist.
  const std::uint64_t before_clear = store.revision();
  store.SetCondition(path, 12, std::nullopt);
  const auto* cleared = store.FindByPath(path);
  Expect(!(*cleared)[0].condition.has_value(), "nullopt clears the condition");
  Expect((*cleared)[0].hit_condition == std::optional<std::string>(">10") &&
             (*cleared)[0].log_message == std::optional<std::string>("value={x}"),
         "clearing the condition keeps the hit-count and log-message modifiers");
  Expect(store.revision() != before_clear, "clearing a modifier bumps the revision");

  // A no-op assignment does not bump the revision.
  const std::uint64_t before_noop = store.revision();
  store.SetHitCondition(path, 12, ">10");
  Expect(store.revision() == before_noop, "an unchanged modifier does not bump the revision");
}

void TestBreakpointStoreShiftInsertLineAboveMovesDown() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 10);
  // Insert a blank line at line 2 (a newline before line 2's content).
  Expect(store.ShiftForAppliedEdit(path, MakeEdit(2, 0, 2, 0, "\n")),
         "inserting a line above a breakpoint should shift it");
  Expect(store.HasBreakpoint(path, 11) && !store.HasBreakpoint(path, 10),
         "the breakpoint should move from line 10 to 11");
}

void TestBreakpointStoreShiftDeleteLineAboveMovesUp() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 10);
  // Delete line 2 entirely: replace [2,0 .. 3,0) with nothing.
  Expect(store.ShiftForAppliedEdit(path, MakeEdit(2, 0, 3, 0, "")),
         "deleting a line above a breakpoint should shift it");
  Expect(store.HasBreakpoint(path, 9) && !store.HasBreakpoint(path, 10),
         "the breakpoint should move from line 10 to 9");
}

void TestBreakpointStoreShiftEditBelowLeavesItPut() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 5);
  Expect(!store.ShiftForAppliedEdit(path, MakeEdit(20, 0, 20, 0, "\n")),
         "an edit below the breakpoint should not move it (returns false)");
  Expect(store.HasBreakpoint(path, 5), "the breakpoint stays on line 5");
}

void TestBreakpointStoreShiftInlineEditIsNoop() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 5);
  // Replace two chars within line 5 with one char: no net line change.
  Expect(!store.ShiftForAppliedEdit(path, MakeEdit(5, 2, 5, 4, "x")),
         "an in-line edit with no line delta should be a no-op");
  Expect(store.HasBreakpoint(path, 5), "the breakpoint stays on line 5");
}

void TestBreakpointStoreShiftDeletedLineSlidesToEditEnd() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 5);
  store.Toggle(path, 9);
  // Delete lines 4..6 (replace [4,0 .. 7,0) with nothing). The breakpoint on the
  // interior line 5 slides to the edit's end line (4); the one at 9 shifts up by 3.
  Expect(store.ShiftForAppliedEdit(path, MakeEdit(4, 0, 7, 0, "")),
         "a multi-line delete spanning a breakpoint should shift the set");
  Expect(store.HasBreakpoint(path, 4), "the breakpoint on a removed line slides to the edit end");
  Expect(store.HasBreakpoint(path, 6) && !store.HasBreakpoint(path, 9),
         "the breakpoint below the delete shifts up by the removed line count");
}

void TestBreakpointStoreShiftCollisionDedupes() {
  BreakpointStore store;
  const std::filesystem::path path = "/tmp/project/main.py";
  store.Toggle(path, 4);
  store.Toggle(path, 5);
  // Delete line 4 (replace [4,0 .. 5,0)): line 4 stays put, line 5 slides onto 4.
  Expect(store.ShiftForAppliedEdit(path, MakeEdit(4, 0, 5, 0, "")),
         "the shift should report a change");
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == 1 && (*bps)[0].line == 4,
         "two breakpoints colliding on one line dedupe to a single breakpoint");
}

}  // namespace

void RegisterBreakpointStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BreakpointStore/ShiftInsertLineAboveMovesDown",
          TestBreakpointStoreShiftInsertLineAboveMovesDown);
  AddTest(tests, "BreakpointStore/ShiftDeleteLineAboveMovesUp",
          TestBreakpointStoreShiftDeleteLineAboveMovesUp);
  AddTest(tests, "BreakpointStore/ShiftEditBelowLeavesItPut",
          TestBreakpointStoreShiftEditBelowLeavesItPut);
  AddTest(tests, "BreakpointStore/ShiftInlineEditIsNoop",
          TestBreakpointStoreShiftInlineEditIsNoop);
  AddTest(tests, "BreakpointStore/ShiftDeletedLineSlidesToEditEnd",
          TestBreakpointStoreShiftDeletedLineSlidesToEditEnd);
  AddTest(tests, "BreakpointStore/ShiftCollisionDedupes",
          TestBreakpointStoreShiftCollisionDedupes);
  AddTest(tests, "BreakpointStore/ModifierSetters", TestBreakpointStoreModifierSetters);
  AddTest(tests, "BreakpointStore/ToggleAddsAndRemoves", TestBreakpointStoreToggleAddsAndRemoves);
  AddTest(tests, "BreakpointStore/KeepsLinesSorted", TestBreakpointStoreKeepsLinesSorted);
  AddTest(tests, "BreakpointStore/PathNormalizationMatches",
          TestBreakpointStorePathNormalizationMatches);
  AddTest(tests, "BreakpointStore/SnapshotAllIsDeterministic",
          TestBreakpointStoreSnapshotAllIsDeterministic);
  AddTest(tests, "BreakpointStore/ApplyVerificationByIndex",
          TestBreakpointStoreApplyVerificationByIndex);
  AddTest(tests, "BreakpointStore/ApplyVerificationMatchesByLineNotIndex",
          TestBreakpointStoreApplyVerificationMatchesByLineNotIndex);
  AddTest(tests, "BreakpointStore/ApplyBreakpointEvent", TestBreakpointStoreApplyBreakpointEvent);
  AddTest(tests, "BreakpointStore/EventForeignIdDoesNotClobberBoundBreakpoint",
          TestBreakpointStoreEventForeignIdDoesNotClobberBoundBreakpoint);
  AddTest(tests, "BreakpointStore/ToggleEnabled", TestBreakpointStoreToggleEnabled);
  AddTest(tests, "BreakpointStore/ReplaceAllResetsTransientState",
          TestBreakpointStoreReplaceAllResetsTransientState);
  AddTest(tests, "BreakpointStore/RevisionBumps", TestBreakpointStoreRevisionBumps);
}

}  // namespace microide::tests
