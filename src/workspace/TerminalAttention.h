#pragma once

// What a terminal program's bell or desktop notification (OSC 9 / 777) does
// beyond its own pane.
//
// Inside the window that was already decided: a tab the user cannot see gets a
// mark and, for a notification, a toast. Neither reaches a user who is in
// another window -- an agent asking for input sat unseen until they came back --
// so an alert that arrives while the window is unfocused also flags the window
// with the desktop (the taskbar's "wants attention") and, for a notification,
// posts it as a desktop notification.
//
// A pure function of one pane's signals so it is tested without a window, a
// terminal or a notification daemon; the shell's drain only acts on the result.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

struct DesktopNotification {
  std::string summary;
  std::string body;

  bool operator==(const DesktopNotification&) const = default;
};

// One pane's signals from one drain.
struct TerminalAlert {
  bool bell = false;
  bool notification = false;
  std::string_view title;  // of the notification; OSC 9 has none
  std::string_view body;
  std::string_view project_label;
  std::string_view terminal_label;
  // The project is the one in front: its toasts do not name it.
  bool project_in_front = true;
  bool tab_on_screen = false;
  bool window_focused = true;
  // `terminal.alert_when_unfocused`.
  bool alert_when_unfocused = true;
};

struct TerminalAlertOutcome {
  // The tab's bell mark (the caller skips a tab already marked).
  bool mark_tab = false;
  // Empty: no toast.
  std::string toast;
  // Ask the desktop to flag the window until it is focused.
  bool flag_window = false;
  std::optional<DesktopNotification> desktop;
};

TerminalAlertOutcome DecideTerminalAlert(const TerminalAlert& alert);

// At most one desktop notification a second: a program that prints
// notifications in a loop costs one spawned process a second, not one a frame.
// A notification inside the window is dropped; the window flag and the tab mark
// still say a terminal wants attention.
inline constexpr std::uint64_t kDesktopNotificationSpacingMs = 1000;

// Whether a desktop notification may be posted at `now_ms`, given the last one
// (0 = none yet); records `now_ms` when it may.
bool AdmitDesktopNotification(std::uint64_t now_ms, std::uint64_t& last_posted_ms);

}  // namespace microide::workspace
