#include "util/PerformanceTrace.h"

#include <cstdlib>

#include "util/StringUtil.h"

namespace microide::util {

TraceChannel& PerformanceTrace::Channel() {
  // Function-local so the env read happens on first use rather than during
  // static initialization of an arbitrary translation unit.
  static TraceChannel channel("perf", "MICROIDE_PERF_TRACE", "MICROIDE_PERF_SUMMARY",
                              "MICROIDE_PERF_TRACE_MIN_MS");
  return channel;
}

bool PerformanceTrace::FlagEnabled(const char* env_name) {
  if (env_name == nullptr || env_name[0] == '\0') {
    return false;
  }
  const char* value = std::getenv(env_name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return !IsFalseyToken(value);
}

}  // namespace microide::util
