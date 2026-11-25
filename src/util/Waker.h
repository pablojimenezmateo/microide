#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace microide::util {

using WakeChannel = std::uint32_t;

using WakePusher = std::function<bool(WakeChannel)>;

inline WakePusher& InstalledWakePusher() {
  static WakePusher pusher;
  return pusher;
}

inline void SetWakePusher(WakePusher pusher) { InstalledWakePusher() = std::move(pusher); }

struct WakePusherOverrideSlot {
  std::mutex mutex;
  WakePusher pusher;
  std::atomic<bool> active{false};
};

inline WakePusherOverrideSlot& WakePusherOverride() {
  static WakePusherOverrideSlot slot;
  return slot;
}

inline void SetWakePusherForTesting(WakePusher pusher) {
  WakePusherOverrideSlot& slot = WakePusherOverride();
  std::scoped_lock lock(slot.mutex);
  slot.pusher = std::move(pusher);
  slot.active.store(static_cast<bool>(slot.pusher), std::memory_order_release);
}

inline std::atomic<bool>& WakeOwedFlag() {
  static std::atomic<bool> owed{false};
  return owed;
}

inline bool HasOwedWake() { return WakeOwedFlag().load(std::memory_order_acquire); }

inline bool ConsumeOwedWake() {
  return WakeOwedFlag().exchange(false, std::memory_order_acq_rel);
}

inline std::atomic<bool>& WakeRegistrationDegradedFlag() {
  static std::atomic<bool> degraded{false};
  return degraded;
}

inline void SetWakeRegistrationDegraded(bool degraded) {
  WakeRegistrationDegradedFlag().store(degraded, std::memory_order_release);
}

inline bool WakeRegistrationDegraded() {
  return WakeRegistrationDegradedFlag().load(std::memory_order_acquire);
}

inline bool DeliverWake(WakeChannel channel) {
  if (channel == 0) {
    return false;
  }
  if (WakePusherOverrideSlot& slot = WakePusherOverride(); slot.active.load(std::memory_order_acquire)) {
    std::scoped_lock lock(slot.mutex);
    if (slot.pusher) {
      return slot.pusher(channel);
    }
  }
  const WakePusher& pusher = InstalledWakePusher();
  return pusher ? pusher(channel) : false;
}

inline bool PushWake(WakeChannel channel) {
  if (channel == 0) {
    return false;
  }
  const bool delivered = DeliverWake(channel);
  if (!delivered) {
    WakeOwedFlag().store(true, std::memory_order_release);
  }
  return delivered;
}

}
