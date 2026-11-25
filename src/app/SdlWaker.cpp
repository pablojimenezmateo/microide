#include "app/SdlWaker.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include <pthread.h>
#include <signal.h>

#include "app/LostWakeRelay.h"
#include "util/Log.h"
#include "util/Waker.h"

namespace microide::app {

namespace {

constexpr std::chrono::milliseconds kLostWakeDelay{10};

std::atomic<std::uint32_t> g_nudge_type{0};

const int kWakeSignal = SIGRTMIN + 7;
std::atomic<bool> g_interrupt_ready{false};
pthread_t g_main_thread{};

void InstallWakeSignal() {
  struct sigaction action {};
  action.sa_handler = [](int) {};
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (::sigaction(kWakeSignal, &action, nullptr) == 0) {
    g_main_thread = pthread_self();
    g_interrupt_ready.store(true, std::memory_order_release);
  }
}

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

}

void InstallSdlWaker() {
  g_nudge_type.store(RegisterSdlWakeChannel("wake-nudge"), std::memory_order_release);
  InstallWakeSignal();
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
    SDL_Log("%s", std::string(message).c_str());
  });
}

namespace {

std::mutex g_channel_names_mutex;
std::unordered_map<std::uint32_t, const char*>& ChannelNames() {
  static std::unordered_map<std::uint32_t, const char*> names;
  return names;
}

}

std::string SdlEventName(std::uint32_t event_type) {
  {
    std::scoped_lock lock(g_channel_names_mutex);
    if (const auto found = ChannelNames().find(event_type); found != ChannelNames().end()) {
      return found->second;
    }
  }
  char hex[16];
  (void)std::snprintf(hex, sizeof(hex), "0x%x", event_type);
  return hex;
}

std::uint32_t RegisterSdlWakeChannel(const char* name) {
  const Uint32 type = SDL_RegisterEvents(1);
  if (type != static_cast<Uint32>(-1)) {
    std::scoped_lock lock(g_channel_names_mutex);
    ChannelNames()[type] = name;
  }
  return type == static_cast<Uint32>(-1) ? 0u : static_cast<std::uint32_t>(type);
}

}
