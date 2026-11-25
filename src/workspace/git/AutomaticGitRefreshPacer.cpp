#include "workspace/git/AutomaticGitRefreshPacer.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace microide::workspace {

namespace {

Uint32 SDLCALL PushWakeOnce(void* userdata, SDL_TimerID , Uint32 ) {
  (void)util::PushWake(static_cast<util::WakeChannel>(reinterpret_cast<std::uintptr_t>(userdata)));
  return 0;
}

}

bool AutomaticGitRefreshPacer::Request(const std::uint64_t now_ms, const bool refresh_running) {
  if (!refresh_running && now_ms >= next_allowed_ms_) {
    pending_ = false;
    next_allowed_ms_ = now_ms + kIntervalMs;
    return true;
  }
  pending_ = true;
  if (!refresh_running && wake_armed_for_ms_ != next_allowed_ms_) {
    wake_armed_for_ms_ = next_allowed_ms_;
    if (wake_channel_ != 0) {
      const auto delay = static_cast<Uint32>(std::max<std::uint64_t>(1, next_allowed_ms_ - now_ms));
      (void)SDL_AddTimer(delay, PushWakeOnce,
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(wake_channel_)));
    }
  }
  return false;
}

bool AutomaticGitRefreshPacer::Resume(const std::uint64_t now_ms, const bool refresh_running) {
  if (!pending_) {
    return false;
  }
  if (now_ms >= wake_armed_for_ms_) {
    wake_armed_for_ms_ = 0;
  }
  if (!refresh_running && now_ms >= next_allowed_ms_) {
    return true;
  }
  return Request(now_ms, refresh_running);
}

}
