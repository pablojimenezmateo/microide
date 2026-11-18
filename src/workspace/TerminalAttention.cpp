#include "workspace/TerminalAttention.h"

namespace microide::workspace {

TerminalAlertOutcome DecideTerminalAlert(const TerminalAlert& alert) {
  TerminalAlertOutcome outcome;
  if (!alert.bell && !alert.notification) {
    return outcome;
  }
  // On screen the program is already showing it; behind another tab, a hidden
  // panel or another project it is not.
  outcome.mark_tab = !alert.tab_on_screen;
  if (alert.notification && !alert.tab_on_screen) {
    if (!alert.project_in_front && !alert.project_label.empty()) {
      outcome.toast.append(alert.project_label).append(" — ");
    }
    outcome.toast.append(alert.terminal_label).append(": ");
    if (!alert.title.empty()) {
      outcome.toast.append(alert.title);
      if (!alert.body.empty()) {
        outcome.toast.append(" — ");
      }
    }
    outcome.toast.append(alert.body);
  }
  if (alert.window_focused || !alert.alert_when_unfocused) {
    return outcome;
  }
  outcome.flag_window = true;
  if (alert.notification) {
    DesktopNotification desktop;
    // The summary says where: the user is in another application and has to
    // know which project and terminal to come back to.
    if (!alert.project_label.empty()) {
      desktop.summary.append(alert.project_label).append(" — ");
    }
    desktop.summary.append(alert.terminal_label);
    desktop.body.append(alert.title);
    if (!alert.title.empty() && !alert.body.empty()) {
      desktop.body.append(" — ");
    }
    desktop.body.append(alert.body);
    outcome.desktop = std::move(desktop);
  }
  return outcome;
}

bool AdmitDesktopNotification(std::uint64_t now_ms, std::uint64_t& last_posted_ms) {
  if (last_posted_ms != 0 && now_ms - last_posted_ms < kDesktopNotificationSpacingMs) {
    return false;
  }
  // 0 means "none yet", so a post at tick 0 records 1.
  last_posted_ms = now_ms == 0 ? 1 : now_ms;
  return true;
}

}  // namespace microide::workspace
