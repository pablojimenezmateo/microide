#pragma once

namespace microide::app {

// Global atomic counter tracking in-flight background tasks across all services.
// Services call IncrementBackgroundTaskCount() when starting async work and
// DecrementBackgroundTaskCountAndWake() when finishing. The wake call posts an
// SDL user event so the event loop exits SDL_WaitEvent on task completion.
// The counter never goes negative (ASAN assertion fires on underflow).
void IncrementBackgroundTaskCount();
void DecrementBackgroundTaskCountAndWake();
int GetBackgroundTaskCount();

}  // namespace microide::app
