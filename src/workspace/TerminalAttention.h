#pragma once

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

struct TerminalAlert {
  bool bell = false;
  bool notification = false;
  std::string_view title;
  std::string_view body;
  std::string_view project_label;
  std::string_view terminal_label;
  bool project_in_front = true;
  bool tab_on_screen = false;
  bool window_focused = true;
  bool alert_when_unfocused = true;
};

struct TerminalAlertOutcome {
  bool mark_tab = false;
  std::string toast;
  bool flag_window = false;
  std::optional<DesktopNotification> desktop;
};

TerminalAlertOutcome DecideTerminalAlert(const TerminalAlert& alert);

inline constexpr std::uint64_t kDesktopNotificationSpacingMs = 1000;

bool AdmitDesktopNotification(std::uint64_t now_ms, std::uint64_t& last_posted_ms);

}
