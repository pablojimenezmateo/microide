#pragma once

namespace microide::app {

// Event-drain budget for the main loop. The inner `do { HandleEvent } while
// (SDL_PollEvent)` would otherwise drain the ENTIRE SDL queue before returning to
// the render step. Mouse-motion is coalesced, but a sustained flood of keyboard,
// window, plugin, or control-wake events keeps accumulating dirty rects /
// full-redraw requests and delays visual feedback well past one frame. Once a
// redraw is pending, yield to render after this many events; the remainder is
// handled on the next loop iteration (after a render). SDL events are discrete (no
// multi-event atomic sequence spans PollEvent), so breaking mid-queue preserves
// per-event ordering. TD-2026-07-17A-100.
inline constexpr int kMaxEventsPerDrain = 512;

// True when the inner event drain should break out to render: a redraw is pending
// and at least `budget` events have already been processed this drain.
inline bool ShouldYieldEventDrain(int events_processed, bool redraw_pending,
                                  int budget = kMaxEventsPerDrain) {
  return redraw_pending && events_processed >= budget;
}

}  // namespace microide::app
