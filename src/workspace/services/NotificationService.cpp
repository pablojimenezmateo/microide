#include "workspace/services/NotificationService.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "util/StringUtil.h"

namespace microide::workspace {
namespace {

// Expiry for a transient row posted at `now_ms`. Saturates rather than wrapping: a
// monotonic clock near UINT64_MAX would otherwise overflow to a tiny expiry and
// drop the notification on the next ExpireDue.
std::uint64_t TransientExpiry(std::uint64_t now_ms, std::uint64_t duration_ms) {
  return now_ms > std::numeric_limits<std::uint64_t>::max() - duration_ms
             ? std::numeric_limits<std::uint64_t>::max()
             : now_ms + duration_ms;
}

NotificationService::Actions CapActions(std::vector<NotificationService::Action>& requested) {
  NotificationService::Actions actions;
  for (NotificationService::Action& action : requested) {
    if (actions.size() >= NotificationService::MaxActions()) {
      break;
    }
    if (action.label.empty()) {
      continue;  // a button with no label is a button nobody can find
    }
    if (util::TruncateUtf8ToByteBudget(action.label, NotificationService::MaxActionLabelBytes())) {
      action.label += "…";
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

}  // namespace

NotificationService::Tone NotificationService::ToneFromLevel(std::string_view level) {
  if (level == "error" || level == "err") {
    return Tone::Error;
  }
  if (level == "warning" || level == "warn") {
    return Tone::Warning;
  }
  return Tone::Info;
}

bool NotificationService::CapMessage(std::string& message) {
  // Byte-cap the message at ingress on a UTF-8 codepoint boundary so one oversized
  // toast (a plugin ctx.notify, or a subprocess/provider error) cannot force large
  // string copies + text measurement during a full redraw (TD-2026-07-17A-101).
  const bool truncated = util::TruncateUtf8ToByteBudget(message, MaxMessageBytes());
  if (truncated) {
    message += "…";  // ellipsis marker so the shortened display string reads as clipped
  }
  return truncated;
}

void NotificationService::Show(Tone tone, std::string message, std::uint64_t now_ms) {
  Show(Request{.tone = tone, .message = std::move(message)}, now_ms);
}

void NotificationService::Show(Request request, std::uint64_t now_ms) {
  if (request.message.empty()) {
    return;
  }
  const bool truncated = CapMessage(request.message);
  // Progress implies sticky: the row ends when the work does. A sticky row with no
  // key is a row nothing can ever dismiss, so it is posted transient instead —
  // silently leaking a permanent toast is the worse failure.
  const bool sticky = (request.sticky || request.progress.has_value()) && !request.key.empty();
  Actions actions = CapActions(request.actions);
  const std::uint64_t expiry_ms =
      TransientExpiry(now_ms, actions.empty() ? DurationMs() : ActionDurationMs());

  // Identity first: a keyed row replaces the one already on screen IN PLACE, so an
  // updating row keeps its stack position instead of walking to the top under the
  // pointer.
  if (!request.key.empty()) {
    for (Notification& existing : notifications_) {
      if (existing.key == request.key) {
        existing.tone = request.tone;
        existing.message = std::move(request.message);
        existing.expiry_ms = expiry_ms;
        existing.truncated = truncated;
        existing.sticky = sticky;
        existing.progress = request.progress;
        // The button set is part of what the row says, so it is replaced with
        // the text; a hovered button that no longer exists stops being hovered.
        existing.actions = std::move(actions);
        if (existing.hovered_action.has_value() &&
            *existing.hovered_action >= existing.actions.size()) {
          existing.hovered_action.reset();
        }
        return;
      }
    }
  } else {
    // Anonymous rows keep the old dedup: repeating the same message refreshes the
    // one already on screen instead of stacking a copy. A held shortcut that keeps
    // getting refused, or a button clicked twice, would otherwise push the other
    // toasts out of the stack with duplicates of one sentence.
    for (Notification& existing : notifications_) {
      if (existing.key.empty() && existing.tone == request.tone &&
          existing.message == request.message) {
        existing.expiry_ms = expiry_ms;
        existing.truncated = truncated;
        return;
      }
    }
  }

  if (sticky) {
    std::size_t sticky_count = 0;
    for (const Notification& existing : notifications_) {
      sticky_count += existing.sticky ? 1 : 0;
    }
    if (sticky_count >= MaxSticky()) {
      return;  // refused rather than stacked off the top of the window
    }
  }

  notifications_.push_back(Notification{
      .tone = request.tone,
      .key = std::move(request.key),
      .message = std::move(request.message),
      .expiry_ms = expiry_ms,
      .truncated = truncated,
      .sticky = sticky,
      .progress = request.progress,
      .actions = std::move(actions),
  });
  TrimTransientOverflow();
}

void NotificationService::TrimTransientOverflow() {
  // Drop the OLDEST transient rows past the cap. Sticky rows are skipped: they
  // report a state, and dropping one because three warnings arrived would hide the
  // state rather than the warnings.
  std::size_t transient = 0;
  for (const Notification& existing : notifications_) {
    transient += existing.sticky ? 0 : 1;
  }
  for (auto it = notifications_.begin(); transient > MaxVisible() && it != notifications_.end();) {
    if (it->sticky) {
      ++it;
      continue;
    }
    it = notifications_.erase(it);
    --transient;
  }
}

bool NotificationService::DismissKey(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  const auto match = std::find_if(notifications_.begin(), notifications_.end(),
                                  [key](const Notification& n) { return n.key == key; });
  if (match == notifications_.end()) {
    return false;
  }
  notifications_.erase(match);
  return true;
}

bool NotificationService::ExpireDue(std::uint64_t now_ms) {
  const std::size_t before = notifications_.size();
  notifications_.erase(
      std::remove_if(notifications_.begin(), notifications_.end(),
                     [now_ms](const Notification& notification) {
                       return !notification.sticky && !notification.hovered &&
                              notification.expiry_ms <= now_ms;
                     }),
      notifications_.end());
  return notifications_.size() != before;
}

std::optional<std::uint64_t> NotificationService::NextExpiryDelayMs(std::uint64_t now_ms) const {
  std::optional<std::uint64_t> earliest;
  for (const Notification& notification : notifications_) {
    if (notification.sticky || notification.hovered) {
      // Nothing to wake for: a sticky row ends when its owner says so, and a
      // hovered one when the pointer leaves (SetHovered re-arms its expiry).
      continue;
    }
    earliest = earliest.has_value() ? std::min(*earliest, notification.expiry_ms)
                                    : notification.expiry_ms;
  }
  if (!earliest.has_value()) {
    return std::nullopt;
  }
  return *earliest <= now_ms ? 0 : *earliest - now_ms;
}

bool NotificationService::SetHovered(std::optional<std::size_t> index,
                                     std::optional<std::size_t> action,
                                     std::uint64_t now_ms) {
  bool changed = false;
  for (std::size_t i = 0; i < notifications_.size(); ++i) {
    Notification& notification = notifications_[i];
    const bool hovered = index.has_value() && *index == i;
    const std::optional<std::size_t> hovered_action =
        hovered && action.has_value() && *action < notification.actions.size() ? action
                                                                                : std::nullopt;
    if (notification.hovered && !hovered && !notification.sticky) {
      // Leaving a row that expired under the pointer: give the eye a moment to
      // follow instead of removing it on the very motion event that left it.
      notification.expiry_ms =
          std::max(notification.expiry_ms, TransientExpiry(now_ms, HoverGraceMs()));
    }
    changed = changed || notification.hovered != hovered ||
              notification.hovered_action != hovered_action;
    notification.hovered = hovered;
    notification.hovered_action = hovered_action;
  }
  return changed;
}

std::optional<NotificationService::Action> NotificationService::TakeAction(std::size_t index,
                                                                           std::size_t action) {
  if (index >= notifications_.size() || action >= notifications_[index].actions.size()) {
    return std::nullopt;
  }
  Action taken = notifications_[index].actions[action];
  if (!taken.keep_open && !notifications_[index].sticky) {
    Dismiss(index);
  }
  return taken;
}

}  // namespace microide::workspace
