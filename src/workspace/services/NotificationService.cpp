#include "workspace/services/NotificationService.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "util/StringUtil.h"

namespace microide::workspace {

NotificationService::Tone NotificationService::ToneFromLevel(std::string_view level) {
  if (level == "error" || level == "err") {
    return Tone::Error;
  }
  if (level == "warning" || level == "warn") {
    return Tone::Warning;
  }
  return Tone::Info;
}

void NotificationService::Show(Tone tone, std::string message, std::uint64_t now_ms) {
  if (message.empty()) {
    return;
  }
  // Byte-cap the message at ingress on a UTF-8 codepoint boundary so one oversized
  // toast (a plugin ctx.notify, or a subprocess/provider error) cannot force large
  // string copies + text measurement during a full redraw (TD-2026-07-17A-101).
  const bool truncated = util::TruncateUtf8ToByteBudget(message, MaxMessageBytes());
  if (truncated) {
    message += "…";  // ellipsis marker so the shortened display string reads as clipped
  }
  // Saturate rather than wrap: a monotonic clock near UINT64_MAX would otherwise
  // overflow to a tiny expiry and drop the notification on the next ExpireDue.
  const std::uint64_t expiry_ms = now_ms > std::numeric_limits<std::uint64_t>::max() - DurationMs()
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : now_ms + DurationMs();
  notifications_.push_back(Notification{
      .tone = tone,
      .message = std::move(message),
      .expiry_ms = expiry_ms,
      .truncated = truncated,
  });
  if (notifications_.size() > MaxVisible()) {
    notifications_.erase(notifications_.begin(),
                         notifications_.begin() +
                             static_cast<std::ptrdiff_t>(notifications_.size() - MaxVisible()));
  }
}

bool NotificationService::ExpireDue(std::uint64_t now_ms) {
  const std::size_t before = notifications_.size();
  notifications_.erase(
      std::remove_if(notifications_.begin(), notifications_.end(),
                     [now_ms](const Notification& notification) {
                       return notification.expiry_ms <= now_ms;
                     }),
      notifications_.end());
  return notifications_.size() != before;
}

std::optional<std::uint64_t> NotificationService::NextExpiryDelayMs(std::uint64_t now_ms) const {
  if (notifications_.empty()) {
    return std::nullopt;
  }
  std::uint64_t earliest = notifications_.front().expiry_ms;
  for (const Notification& notification : notifications_) {
    earliest = std::min(earliest, notification.expiry_ms);
  }
  return earliest <= now_ms ? 0 : earliest - now_ms;
}

}  // namespace microide::workspace
