#include "util/StartupTrace.h"

namespace microide::util {

TraceChannel& StartupTrace::Channel() {
  static TraceChannel channel("startup", "MICROIDE_STARTUP_TRACE", "MICROIDE_STARTUP_SUMMARY",
                              /*min_duration_env=*/nullptr);
  return channel;
}

}  // namespace microide::util
