// What a terminal's bell or notification does past its own pane: the decision
// (DecideTerminalAlert) and the desktop notification's spacing, both without a
// window, a terminal or a notification daemon.

#include "TestSupport.h"

#include "workspace/TerminalAttention.h"

#include <cstdint>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::AdmitDesktopNotification;
using microide::workspace::DecideTerminalAlert;
using microide::workspace::kDesktopNotificationSpacingMs;
using microide::workspace::TerminalAlert;
using microide::workspace::TerminalAlertOutcome;

TerminalAlert AgentAlert() {
  return TerminalAlert{
      .bell = false,
      .notification = true,
      .title = {},
      .body = "needs your input",
      .project_label = "alpha",
      .terminal_label = "claude",
      .project_in_front = true,
      .tab_on_screen = true,
      .window_focused = true,
      .alert_when_unfocused = true,
  };
}

void TestAlertSeenOnScreenInAFocusedWindowDoesNothing() {
  const TerminalAlertOutcome outcome = DecideTerminalAlert(AgentAlert());
  Expect(!outcome.mark_tab && outcome.toast.empty(), "the program is already showing it");
  Expect(!outcome.flag_window && !outcome.desktop.has_value(), "and the user is looking at the window");

  Expect(!DecideTerminalAlert(TerminalAlert{}).mark_tab, "no signal, no mark");
}

void TestAlertBehindAnotherTabToastsAndMarks() {
  TerminalAlert alert = AgentAlert();
  alert.tab_on_screen = false;
  TerminalAlertOutcome outcome = DecideTerminalAlert(alert);
  Expect(outcome.mark_tab, "a tab out of sight is marked");
  Expect(outcome.toast == "claude: needs your input", "the front project's toast names the terminal: " + outcome.toast);
  Expect(!outcome.flag_window && !outcome.desktop.has_value(), "a focused window needs no desktop alert");

  alert.project_in_front = false;
  alert.title = "Claude Code";
  outcome = DecideTerminalAlert(alert);
  Expect(outcome.toast == "alpha \xe2\x80\x94 claude: Claude Code \xe2\x80\x94 needs your input",
         "a background project's toast names the project, and a title leads the body: " + outcome.toast);

  alert.notification = false;
  alert.bell = true;
  outcome = DecideTerminalAlert(alert);
  Expect(outcome.mark_tab && outcome.toast.empty(), "a bell marks the tab without a toast");
}

// The gap this closes: with the window unfocused and the terminal on screen,
// nothing at all told the user an agent was waiting.
void TestAlertInAnUnfocusedWindowReachesTheDesktop() {
  TerminalAlert alert = AgentAlert();
  alert.window_focused = false;
  TerminalAlertOutcome outcome = DecideTerminalAlert(alert);
  Expect(outcome.flag_window, "the window is flagged");
  Expect(outcome.desktop.has_value(), "and the notification is posted to the desktop");
  if (outcome.desktop.has_value()) {
    Expect(outcome.desktop->summary == "alpha \xe2\x80\x94 claude",
           "the summary says which project and terminal: " + outcome.desktop->summary);
    Expect(outcome.desktop->body == "needs your input", "the body is the program's text: " + outcome.desktop->body);
  }
  Expect(!outcome.mark_tab && outcome.toast.empty(), "the tab on screen still needs neither mark nor toast");

  alert.notification = false;
  alert.bell = true;
  outcome = DecideTerminalAlert(alert);
  Expect(outcome.flag_window && !outcome.desktop.has_value(), "a bell flags the window and posts nothing");

  alert.alert_when_unfocused = false;
  alert.notification = true;
  outcome = DecideTerminalAlert(alert);
  Expect(!outcome.flag_window && !outcome.desktop.has_value(), "the setting turns both off");
}

void TestDesktopNotificationsAreSpaced() {
  std::uint64_t last = 0;
  Expect(AdmitDesktopNotification(0, last), "the first is posted, even at tick 0");
  Expect(!AdmitDesktopNotification(kDesktopNotificationSpacingMs - 1, last), "one inside the spacing is dropped");
  Expect(AdmitDesktopNotification(kDesktopNotificationSpacingMs + 1, last), "one past it is posted");
  Expect(!AdmitDesktopNotification(kDesktopNotificationSpacingMs + 2, last), "and restarts the spacing");
}

}  // namespace

void RegisterTerminalAttentionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalAttention/SeenOnScreenInAFocusedWindowDoesNothing",
          TestAlertSeenOnScreenInAFocusedWindowDoesNothing);
  AddTest(tests, "TerminalAttention/BehindAnotherTabToastsAndMarks", TestAlertBehindAnotherTabToastsAndMarks);
  AddTest(tests, "TerminalAttention/UnfocusedWindowReachesTheDesktop",
          TestAlertInAnUnfocusedWindowReachesTheDesktop);
  AddTest(tests, "TerminalAttention/DesktopNotificationsAreSpaced", TestDesktopNotificationsAreSpaced);
}

}  // namespace microide::tests
