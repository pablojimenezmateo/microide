#pragma once

namespace microide::app {

inline constexpr int kMaxEventsPerDrain = 512;

inline bool ShouldYieldEventDrain(int events_processed, bool redraw_pending,
                                  int budget = kMaxEventsPerDrain) {
  return redraw_pending && events_processed >= budget;
}

}
