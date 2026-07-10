#pragma once

#include <cstdint>

namespace microide::app {

// Global atomic counter tracking in-flight background tasks across all services.
// Services call IncrementBackgroundTaskCount() when starting async work and
// DecrementBackgroundTaskCountAndWake() when finishing. The wake call posts an
// SDL event so the event loop exits SDL_WaitEvent on task completion.
// The counter never goes negative (ASAN assertion fires on underflow).
void IncrementBackgroundTaskCount();
void DecrementBackgroundTaskCountAndWake();
int GetBackgroundTaskCount();

// Set the SDL event type used for the neutral idle-recheck wake. Must be a value
// obtained from SDL_RegisterEvents so it is not claimed by any event-dispatch
// branch (the dispatcher routes it to the neutral default). If never set, the
// wake falls back to the bare SDL_EVENT_USER base, which aliases the first
// registered custom event and mis-routes the wake — so the shell sets a dedicated
// type at startup.
void SetBackgroundTaskWakeEventType(std::uint32_t event_type);

}  // namespace microide::app
