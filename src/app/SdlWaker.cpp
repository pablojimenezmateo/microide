#include "app/SdlWaker.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

#include "util/Log.h"
#include "util/Waker.h"

namespace microide::app {

void InstallSdlWaker() {
  util::SetWakePusher([](util::WakeChannel channel) {
    SDL_Event event{};
    event.type = channel;
    return SDL_PushEvent(&event);
  });
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
