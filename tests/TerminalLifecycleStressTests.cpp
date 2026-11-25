#include "TestSupport.h"

#include "platform/ProcessLauncher.h"

#include "terminal/TerminalSession.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::terminal::TerminalSession;

constexpr std::string_view kShell = "/bin/sh";

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

void TestTerminalStressStopDuringHeavyOutput() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(microide::platform::LocalProcessLauncher(), temp.path(), kFloodCommand, kShell)) {
    return;
  }
  Expect(WaitForOutputGrowth(session, 50),
         "a flooding child should stream output through the PTY");
  session.Stop();
  Expect(!session.running(), "Stop during heavy output should leave the session stopped");
}

void TestTerminalStressStopDuringAlternateScreen() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(microide::platform::LocalProcessLauncher(), temp.path(), "printf '\\033[?1049h'; while :; do sleep 1; done", kShell)) {
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

void TestTerminalEnvironmentNamesThisTerminal() {
  TemporaryDirectory temp;
  ScopedEnvVar inherited_vte("VTE_VERSION", "7000");
  ScopedEnvVar inherited_program("TERM_PROGRAM", "vscode");
  TerminalSession session;
  if (!session.StartArgv(microide::platform::LocalProcessLauncher(), temp.path(),
                         "printf 'ENV[%s|%s|%s|%s]\\n' \"$TERM_PROGRAM\" \"${VTE_VERSION-unset}\" "
                         "\"$FORCE_HYPERLINK\" \"$MICROIDE_PROBE\"",
                         {std::string(kShell)}, {}, {"MICROIDE_PROBE=probe-value", "FORCE_HYPERLINK=2"})) {
    return;
  }
  Expect(WaitUntil([&] { return SnapshotContains(session, "ENV[microide|unset|2|probe-value]"); },
                   std::chrono::seconds(5), std::chrono::milliseconds(2)),
         "TERM_PROGRAM is microide, an inherited VTE_VERSION is dropped, and the launch "
         "environment is set over the defaults");
  session.Stop();
}

void TestTerminalStressStopAfterChildExit() {
  TemporaryDirectory temp;
  TerminalSession session;
  if (!session.Start(microide::platform::LocalProcessLauncher(), temp.path(), "printf 'stress-done\\n'; exit 0", kShell)) {
    return;
  }
  Expect(WaitUntil([&] { return !session.running(); }, std::chrono::seconds(5),
                   std::chrono::milliseconds(2)),
         "the session should observe the child's exit");
  Expect(WaitUntil([&] { return SnapshotContains(session, "[process exited]"); },
                   std::chrono::seconds(5), std::chrono::milliseconds(2)),
         "a self-exiting child should leave the process-exit marker");
  session.Stop();
  Expect(!session.running(), "Stop after child exit should stay stopped");
}

void TestTerminalStressOpenCloseLoopRacesChildExit() {
  TemporaryDirectory temp;
  TerminalSession session;
  for (int i = 0; i < 8; ++i) {
    if (!session.Start(microide::platform::LocalProcessLauncher(), temp.path(), i % 2 == 0 ? std::string_view("true") : kFloodCommand,
                       kShell)) {
      return;
    }
    if (i % 2 == 0) {
      Expect(!SnapshotContains(session, "terminal-stress-line"),
             "Start should reset the grid (no stale lines from the previous run)");
    } else {
      Expect(WaitForOutputGrowth(session, 1), "flood iterations should produce output");
    }
    session.Stop();
    Expect(!session.running(), "each open/close iteration should end stopped");
  }
}

void TestTerminalStressMultiTerminalShutdown() {
  TemporaryDirectory temp;
  constexpr std::size_t kSessions = 6;
  std::vector<std::unique_ptr<TerminalSession>> sessions;
  for (std::size_t i = 0; i < kSessions; ++i) {
    auto session = std::make_unique<TerminalSession>();
    if (!session->Start(microide::platform::LocalProcessLauncher(), temp.path(), kFloodCommand, kShell)) {
      return;
    }
    sessions.push_back(std::move(session));
  }
  for (auto& session : sessions) {
    Expect(WaitForOutputGrowth(*session, 10),
           "every concurrent session should stream output");
  }
  for (std::size_t i = 0; i < kSessions / 2; ++i) {
    sessions[i]->Stop();
    Expect(!sessions[i]->running(), "explicitly stopped sessions should be stopped");
  }
  sessions.clear();
}

}

void RegisterTerminalLifecycleStressTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalLifecycleStress/EnvironmentNamesThisTerminal", TestTerminalEnvironmentNamesThisTerminal);
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
}

}
