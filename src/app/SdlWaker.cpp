#include "app/SdlWaker.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

#include <atomic>
#include <chrono>

#include <pthread.h>
#include <signal.h>

#include "app/LostWakeRelay.h"
#include "util/Log.h"
#include "util/Waker.h"

namespace microide::app {

namespace {

// How long a pushed wake gets before the relay calls it lost: well past a delivered
// wake's latency, small next to a human's.
constexpr std::chrono::milliseconds kLostWakeDelay{10};

std::atomic<std::uint32_t> g_nudge_type{0};

// The relay's second, SDL-independent half: a signal that interrupts the main
// thread's blocking wait. SDL's own wake -- a ClientMessage through its X request
// connection -- is lost in the soak's busy cells, the nudge's included: every
// late wait the app logged ended by its timeout with the nudge sent (soak
// 049ca74b0cee, 3 s control answers). SDL's X11 and Wayland waits poll with
// SDL_IOR_NO_RETRY so that a signal ends them; the loop then pumps and finds the
// queued event. SA_RESTART keeps any other system call the signal might land in
// (the loop left its wait just after the relay looked) running as before; the
// poll family is never restarted, which is the point.
const int kWakeSignal = SIGRTMIN + 7;  // + 6 is the stall watchdog's (CrashDiagnostics.h)
std::atomic<bool> g_interrupt_ready{false};
pthread_t g_main_thread{};

void InstallWakeSignal() {
  struct sigaction action {};
  action.sa_handler = [](int) {};  // only interrupts
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (::sigaction(kWakeSignal, &action, nullptr) == 0) {
    g_main_thread = pthread_self();
    g_interrupt_ready.store(true, std::memory_order_release);
  }
}

// Process-lifetime: producers push from their own threads until exit.
LostWakeRelay& Relay() {
  static LostWakeRelay relay(kLostWakeDelay, [] {
    if (const std::uint32_t type = g_nudge_type.load(std::memory_order_acquire); type != 0) {
      SDL_Event event{};
      event.type = type;
      (void)SDL_PushEvent(&event);
    }
    if (g_interrupt_ready.load(std::memory_order_acquire)) {
      (void)pthread_kill(g_main_thread, kWakeSignal);
    }
  });
  return relay;
}

}  // namespace

void InstallSdlWaker() {
  g_nudge_type.store(RegisterSdlWakeChannel(), std::memory_order_release);
  InstallWakeSignal();  // InstallSdlWaker runs on the main thread, before any producer
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

SdlWakeCounts ReadSdlWakeCounts() {
  return SdlWakeCounts{.pushes = Relay().pushes(),
                       .nudges = Relay().nudges(),
                       .first_push_in_wait_ms = Relay().first_push_in_wait_ms()};
}

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
