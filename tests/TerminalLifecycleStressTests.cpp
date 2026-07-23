#include "TestSupport.h"

#include "terminal/TerminalSession.h"

#if defined(__unix__) || defined(__APPLE__)
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#endif

// Terminal lifecycle platform stress suite (TD-2026-07-17-015): real PTY-backed
// sessions driven through the teardown paths the placeholder-terminal shell
// tests never exercise — stop during heavy output, stop on the alternate
// screen, stop racing (and after) child exit, many concurrent sessions torn
// down while streaming, and rapid start/stop reuse of one session object.
// Closing a terminal tab in the shell just destroys its TerminalTabState, whose
// TerminalSession destructor runs the same Stop() path, so these session-level
// cases cover the tab-close-during-output/exit lifecycle directly. The child
// shutdown ladder (SIGHUP -> SIGTERM -> SIGKILL) is bounded (~325ms worst
// case), so every assertion here is a bounded wait, never a hang. The suite's
// real teeth come from the sanitizer runs: a teardown race or fd/thread leak
// on these paths surfaces under ASAN/TSAN.

namespace microide::tests {
namespace {

#if defined(__unix__) || defined(__APPLE__)

using microide::terminal::TerminalSession;

constexpr std::string_view kShell = "/bin/sh";

// An unkillable-by-HUP-free flood: spins printf forever until signalled.
constexpr std::string_view kFloodCommand =
    "while :; do printf 'terminal-stress-line-of-output\\n'; done";

bool WaitForOutputGrowth(TerminalSession& session, std::size_t beyond_lines) {
  return WaitUntil([&] { return session.LineCount() > beyond_lines; }, std::chrono::seconds(5),
                   std::chrono::milliseconds(2));
}

bool SnapshotContains(TerminalSession& session, std::string_view needle) {
  for (const auto& line : session.SnapshotLines()) {
    std::string text;
    for (const auto& cell : line.cells) {
      text.append(cell.DisplayText());
    }
    if (text.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Stop() while the child is mid-flood: the reader thread is parked in read()/
// poll() with more output always pending, so this exercises the wake-pipe
// interrupt + child shutdown ladder under load.
void TestTerminalStressStopDuringHeavyOutput() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(temp.path(), kFloodCommand, kShell)) {
    return;  // no PTY available in this environment; nothing to assert
  }
  Expect(WaitForOutputGrowth(session, 50),
         "a flooding child should stream output through the PTY");
  session.Stop();
  Expect(!session.running(), "Stop during heavy output should leave the session stopped");
}

// Stop() while the child holds the alternate screen (a full-screen app being
// closed): teardown must not depend on the child restoring the primary screen.
void TestTerminalStressStopDuringAlternateScreen() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(temp.path(), "printf '\\033[?1049h'; while :; do sleep 1; done", kShell)) {
    return;
  }
  Expect(WaitUntil([&] { return session.using_alternate_screen(); }, std::chrono::seconds(5),
                   std::chrono::milliseconds(2)),
         "the child should switch the session onto the alternate screen");
  session.Stop();
  Expect(!session.running(), "Stop during alternate screen should leave the session stopped");
  Expect(!session.using_alternate_screen(),
         "Stop should reset the alternate-screen mode for the next Start");
}

// Child exits on its own first (the close-during-exit half): the session must
// notice, emit the exit marker, and a subsequent Stop() must reap the
// already-dead child without hanging.
void TestTerminalStressStopAfterChildExit() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(temp.path(), "printf 'stress-done\\n'; exit 0", kShell)) {
    return;
  }
  Expect(WaitUntil([&] { return !session.running(); }, std::chrono::seconds(5),
                   std::chrono::milliseconds(2)),
         "the session should observe the child's exit");
  Expect(WaitUntil([&] { return SnapshotContains(session, "[process exited]"); },
                   std::chrono::seconds(5), std::chrono::milliseconds(2)),
         "a self-exiting child should leave the process-exit marker");
  session.Stop();  // reap path for an already-dead child; bounded
  Expect(!session.running(), "Stop after child exit should stay stopped");
}

// Stop() racing a child that exits immediately: repeated so the race lands on
// both sides (child dead before Stop / Stop first). Alternates with Start on
// the same session object, which itself calls Stop() — the open/close loop.
void TestTerminalStressOpenCloseLoopRacesChildExit() {
  TemporaryDirectory temp;
  TerminalSession session;
  for (int i = 0; i < 8; ++i) {
    if (!session.Start(temp.path(), i % 2 == 0 ? std::string_view("true") : kFloodCommand,
                       kShell)) {
      return;
    }
    if (i % 2 == 0) {
      // The previous iteration flooded the grid; Start() resets it synchronously,
      // and this iteration's child ("true") emits nothing, so any flood line seen
      // here leaked across the restart.
      Expect(!SnapshotContains(session, "terminal-stress-line"),
             "Start should reset the grid (no stale lines from the previous run)");
    } else {
      // Give the flood a moment to stream so half the iterations stop mid-output.
      Expect(WaitForOutputGrowth(session, 1), "flood iterations should produce output");
    }
    session.Stop();
    Expect(!session.running(), "each open/close iteration should end stopped");
  }
}

// Many concurrent flooding sessions torn down together: half via explicit
// Stop(), half straight through the destructor (the shell's tab-close path,
// which destroys TerminalTabState without a prior Stop call).
void TestTerminalStressMultiTerminalShutdown() {
  TemporaryDirectory temp;
  constexpr std::size_t kSessions = 6;
  std::vector<std::unique_ptr<TerminalSession>> sessions;
  for (std::size_t i = 0; i < kSessions; ++i) {
    auto session = std::make_unique<TerminalSession>();
    if (!session->Start(temp.path(), kFloodCommand, kShell)) {
      return;  // no PTY: skip (earlier sessions are cleaned up by unique_ptr)
    }
    sessions.push_back(std::move(session));
  }
  for (auto& session : sessions) {
    Expect(WaitForOutputGrowth(*session, 10),
           "every concurrent session should stream output");
  }
  // Explicitly stop the first half while the rest keep flooding...
  for (std::size_t i = 0; i < kSessions / 2; ++i) {
    sessions[i]->Stop();
    Expect(!sessions[i]->running(), "explicitly stopped sessions should be stopped");
  }
  // ...then destroy everything (destructor teardown for the still-running half).
  sessions.clear();
}

#endif  // defined(__unix__) || defined(__APPLE__)

}  // namespace

void RegisterTerminalLifecycleStressTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "TerminalLifecycleStress/StopDuringHeavyOutput",
          TestTerminalStressStopDuringHeavyOutput);
  AddTest(tests, "TerminalLifecycleStress/StopDuringAlternateScreen",
          TestTerminalStressStopDuringAlternateScreen);
  AddTest(tests, "TerminalLifecycleStress/StopAfterChildExit",
          TestTerminalStressStopAfterChildExit);
  AddTest(tests, "TerminalLifecycleStress/OpenCloseLoopRacesChildExit",
          TestTerminalStressOpenCloseLoopRacesChildExit);
  AddTest(tests, "TerminalLifecycleStress/MultiTerminalShutdown",
          TestTerminalStressMultiTerminalShutdown);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
