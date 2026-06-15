#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

// Host-owned transient notifications ("toasts"). Plugins (and built-in code) post a
// short message that auto-dismisses after a fixed duration. The service holds no
// timer of its own: callers pass the current time (SDL_GetTicks ms), so it stays
// deterministic and unit-testable, and the shell schedules a single wake at the next
// expiry instead of polling.
class NotificationService {
 public:
  enum class Tone { Info, Warning, Error };

  struct Notification {
    Tone tone = Tone::Info;
    std::string message;
    std::uint64_t expiry_ms = 0;
  };

  // Map a plugin-supplied level string to a tone ("warning"/"warn" -> Warning,
  // "error"/"err" -> Error, anything else -> Info).
  static Tone ToneFromLevel(std::string_view level);

  // Enqueue a notification expiring at now_ms + DurationMs(). Empty messages are
  // ignored. Keeps at most MaxVisible() entries, dropping the oldest.
  void Show(Tone tone, std::string message, std::uint64_t now_ms);

  // Remove notifications whose expiry has passed. Returns true if any were removed
  // (the caller should then request a redraw).
  bool ExpireDue(std::uint64_t now_ms);

  // Milliseconds until the earliest expiry (0 if already due), or nullopt when empty.
  std::optional<std::uint64_t> NextExpiryDelayMs(std::uint64_t now_ms) const;

  const std::vector<Notification>& Active() const { return notifications_; }
  bool Empty() const { return notifications_.empty(); }
  void Clear() { notifications_.clear(); }

  static constexpr std::uint64_t DurationMs() { return 4000; }
  static constexpr std::size_t MaxVisible() { return 4; }

 private:
  std::vector<Notification> notifications_;
};

}  // namespace microide::workspace
