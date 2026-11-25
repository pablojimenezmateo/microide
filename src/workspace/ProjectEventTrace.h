#pragma once

#include "util/PerformanceTrace.h"

namespace microide::workspace {

inline bool TraceProjectEventsEnabled() {
  static const bool enabled =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_PROJECT_EVENTS");
  return enabled;
}

}
