#pragma once

#include <cstdio>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microide::util {

// Startup-phase scope tracer.
//
//   MICROIDE_STARTUP_TRACE=1    stream one stderr line per scope (unfiltered:
//                               startup scopes are few and all of them matter).
//   MICROIDE_STARTUP_SUMMARY=1  print a self-time-ranked table instead, which is
//                               what you want once the same scope runs per file
//                               or per plugin rather than once.
class StartupTrace : public TraceChannelFacade<StartupTrace> {
 public:
  static TraceChannel& Channel();

  // Rebase the elapsed-time origin so "ms total" is measured from the start of
  // Application::Initialize rather than from first use.
  static void Reset() { Channel().Reset(); }
};

}  // namespace microide::util
