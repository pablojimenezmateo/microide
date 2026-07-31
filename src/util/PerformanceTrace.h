#pragma once

#include <cstdio>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microide::util {

// Runtime scope tracer for the live app.
//
// Two independent modes, both off by default:
//
//   MICROIDE_PERF_TRACE=1     stream one stderr line per scope, indented by
//                             nesting depth, filtered by
//                             MICROIDE_PERF_TRACE_MIN_MS.
//   MICROIDE_PERF_SUMMARY=1   accumulate per-label count/total/self/max and
//                             print one table ranked by self time at shutdown.
//
// Prefer the summary when hunting a hotspot: streaming does a write+flush inside
// the parent scope, so it distorts exactly the numbers you are reading.
class PerformanceTrace {
 public:
  static TraceChannel& Channel();

  static bool Enabled() { return Channel().Enabled(); }
  static bool SummaryEnabled() { return Channel().AggregateEnabled(); }
  static double MinimumDurationMs() { return Channel().MinimumDurationMs(); }

  // Generic env-flag reader for the adjacent opt-in trace flags
  // (MICROIDE_TRACE_REDRAW and friends).
  static bool FlagEnabled(const char* env_name);

  static void ResetSummary() { Channel().ResetAggregate(); }
  static std::vector<TraceChannel::AggregateEntry> SummarySnapshot() {
    return Channel().AggregateSnapshot();
  }
  static void WriteSummary(std::FILE* out) { Channel().WriteAggregate(out); }
  // Idempotent: the shutdown path and the atexit backstop both call this.
  static void DumpSummaryOnce() { Channel().DumpAggregateOnce(); }

  class Scope {
   public:
    explicit Scope(std::string_view label) : scope_(Channel(), label) {}

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    TraceScope scope_;
  };
};

}  // namespace microide::util
