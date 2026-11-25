#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/InlineVector.h"
#include "workspace/actions/WorkspaceActionTypes.h"

namespace microide::workspace {

inline constexpr std::string_view kProjectIndexTruncatedNotificationKey =
    "project.index.truncated";

inline constexpr std::string_view kFileOpenInProgressNotificationKey = "editor.open.reading";
inline constexpr std::string_view kStateWriteFailedNotificationKey = "persistence.write-failed";

struct NotificationAction {
  std::string label;
  ActionId id = ActionId::OpenCommandPalette;
  std::vector<std::string> args;
  bool keep_open = false;

  bool operator==(const NotificationAction&) const = default;
};

class NotificationService {
 public:
  enum class Tone { Info, Warning, Error };

  using Action = NotificationAction;
  static constexpr std::size_t MaxActions() { return 3; }
  using Actions = util::InlineVector<Action, 3>;

  struct Notification {
    Tone tone = Tone::Info;
    std::string key;
    std::string message;
    std::uint64_t expiry_ms = 0;
    bool truncated = false;
    bool sticky = false;
    std::optional<float> progress;
    Actions actions;
    bool hovered = false;
    std::optional<std::size_t> hovered_action;
    bool focused = false;
    std::optional<std::size_t> focused_action;
  };

  enum class FocusKey {
    Older,
    Newer,
    Previous,
    Next,
    Activate,
    Close,
    Leave,
  };
  struct FocusKeyResult {
    bool changed = false;
    std::optional<Action> action;
  };

  struct Request {
    Tone tone = Tone::Info;
    std::string key;
    std::string message;
    bool sticky = false;
    std::optional<float> progress;
    std::vector<Action> actions;
  };

  static Tone ToneFromLevel(std::string_view level);

  bool Show(Tone tone, std::string message, std::uint64_t now_ms);

  bool Show(Request request, std::uint64_t now_ms);

  bool DismissKey(std::string_view key);

  bool ExpireDue(std::uint64_t now_ms);

  std::optional<std::uint64_t> NextExpiryDelayMs(std::uint64_t now_ms) const;

  bool SetHovered(std::optional<std::size_t> index, std::optional<std::size_t> action,
                  std::uint64_t now_ms);

  std::optional<Action> TakeAction(std::size_t index, std::size_t action);

  std::optional<std::size_t> FindRow(std::string_view key_or_index) const;
  std::optional<std::size_t> FindAction(std::size_t row, std::string_view label_or_index) const;

  bool Focus();
  bool HasFocus() const;
  void ClearFocus();
  FocusKeyResult HandleFocusKey(FocusKey key);

  void Dismiss(std::size_t index) {
    if (index < notifications_.size()) {
      notifications_.erase(notifications_.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  const std::vector<Notification>& Active() const { return notifications_; }
  static constexpr std::size_t kRecentCapacity = 32;
  struct Posted {
    std::uint64_t seq = 0;
    Notification row;
  };
  const std::deque<Posted>& Recent() const { return recent_; }
  bool Empty() const { return notifications_.empty(); }
  void Clear() { notifications_.clear(); }

  static constexpr std::uint64_t DurationMs() { return 4000; }
  static constexpr std::uint64_t ActionDurationMs() { return 10000; }
  static constexpr std::uint64_t HoverGraceMs() { return 1500; }
  static constexpr std::size_t MaxActionLabelBytes() { return 40; }
  static constexpr std::size_t MaxVisible() { return 4; }
  static constexpr std::size_t MaxSticky() { return 3; }
  static constexpr std::size_t MaxMessageBytes() { return 512; }

 private:
  static bool CapMessage(std::string& message);
  void TrimTransientOverflow();

  void Remember(const Notification& posted);

  std::vector<Notification> notifications_;
  std::deque<Posted> recent_;
  std::uint64_t posted_seq_ = 0;
};

}
