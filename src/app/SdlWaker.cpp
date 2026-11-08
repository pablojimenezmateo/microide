#include "app/SdlWaker.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

#include <atomic>
#include <chrono>

#include "app/LostWakeRelay.h"
#include "util/Log.h"
#include "util/Waker.h"

namespace microide::app {

namespace {

// How long a pushed wake gets before the relay calls it lost: well past a delivered
// wake's latency, small next to a human's.
constexpr std::chrono::milliseconds kLostWakeDelay{10};

std::atomic<std::uint32_t> g_nudge_type{0};

// Process-lifetime: producers push from their own threads until exit.
LostWakeRelay& Relay() {
  static LostWakeRelay relay(kLostWakeDelay, [] {
    if (const std::uint32_t type = g_nudge_type.load(std::memory_order_acquire); type != 0) {
      SDL_Event event{};
      event.type = type;
      (void)SDL_PushEvent(&event);
    }
  });
  return relay;
}

}  // namespace

void InstallSdlWaker() {
  g_nudge_type.store(RegisterSdlWakeChannel(), std::memory_order_release);
  (void)Relay();
  util::SetWakePusher([](util::WakeChannel channel) {
    SDL_Event event{};
    event.type = channel;
    const bool pushed = SDL_PushEvent(&event);
    if (pushed) {
      Relay().NotePush();
    }
    return pushed;
  });
}

SdlBlockingWaitScope::SdlBlockingWaitScope() { Relay().EnterBlockingWait(); }
SdlBlockingWaitScope::~SdlBlockingWaitScope() { Relay().LeaveBlockingWait(); }

bool IsSdlWakeNudge(std::uint32_t event_type) {
  const std::uint32_t type = g_nudge_type.load(std::memory_order_acquire);
  return type != 0 && event_type == type;
}

void InstallSdlLogSink() {
  util::SetLogSink([](std::string_view message) {
    // %s with an explicit copy: SDL_Log takes a NUL-terminated format argument and
    // the view is not guaranteed to be one.
    SDL_Log("%s", std::string(message).c_str());
  });
}

std::uint32_t RegisterSdlWakeChannel() {
  const Uint32 type = SDL_RegisterEvents(1);
  // SDL returns (Uint32)-1 when the custom-event range is exhausted. Map that onto
  // the "waking disabled" channel so no producer ever pushes onto event id 0xFFFFFFFF.
  return type == static_cast<Uint32>(-1) ? 0u : static_cast<std::uint32_t>(type);
}

}  // namespace microide::app
