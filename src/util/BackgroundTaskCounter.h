#pragma once

#include "util/Waker.h"

namespace microide::util {

// Global atomic counter tracking in-flight background tasks across all services.
// Services call IncrementBackgroundTaskCount() when starting async work and
// DecrementBackgroundTaskCountAndWake() when finishing. The wake call pushes a
// neutral wake so the event loop stops blocking on task completion.
// The counter never goes negative (an underflow is clamped and logged).
void IncrementBackgroundTaskCount();
void DecrementBackgroundTaskCountAndWake();
int GetBackgroundTaskCount();

// Set the wake channel used for the neutral idle-recheck wake. Must be a channel the
// host loop routes to its neutral default rather than to a subsystem handler (in the
// SDL shell, one from app::RegisterSdlWakeChannel). Left at 0 the completion wake is
// disabled, which latches the shared wake-owed bit so the idle poll still rechecks —
// strictly better than the old SDL_EVENT_USER fallback, which aliased whichever
// subsystem registered first and mis-routed every completion wake into its handler.
void SetBackgroundTaskWakeChannel(WakeChannel channel);

}  // namespace microide::util
