#pragma once

#include <cstdio>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microide::util {

class StartupTrace : public TraceChannelFacade<StartupTrace> {
 public:
  static TraceChannel& Channel() {
    static TraceChannel channel("startup", "MICROIDE_STARTUP_TRACE", "MICROIDE_STARTUP_SUMMARY",
                                nullptr);
    return channel;
  }

  static void Reset() { Channel().Reset(); }
};

}
