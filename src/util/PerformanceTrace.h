#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microide::util {

class PerformanceTrace : public TraceChannelFacade<PerformanceTrace> {
 public:
  static TraceChannel& Channel() {
    static TraceChannel channel("perf", "MICROIDE_PERF_TRACE", "MICROIDE_PERF_SUMMARY",
                                "MICROIDE_PERF_TRACE_MIN_MS");
    return channel;
  }

  static bool StreamEnabled() { return Channel().StreamEnabled(); }
  static double MinimumDurationMs() { return Channel().MinimumDurationMs(); }

  static bool FlagEnabled(const char* env_name);

  static void RecordSample(std::string_view label, double duration_ms) {
    Channel().RecordSample(label, duration_ms);
  }
  static void RecordSampleNs(std::string_view label, std::uint64_t duration_ns) {
    if (Channel().AggregateEnabled()) {
      Channel().RecordSample(label, static_cast<double>(duration_ns) / 1'000'000.0);
    }
  }

  class ScopeLabel {
   public:
    explicit ScopeLabel(std::string_view base) : ScopeLabel(Channel(), base) {}
    ScopeLabel(TraceChannel& channel, std::string_view base);

    ScopeLabel(const ScopeLabel&) = delete;
    ScopeLabel& operator=(const ScopeLabel&) = delete;

    ScopeLabel& Field(std::string_view key, std::string_view value);
    ScopeLabel& Field(std::string_view key, long long value);

    template <typename P,
              typename = std::enable_if_t<std::is_same_v<std::decay_t<P>, std::filesystem::path>>>
    ScopeLabel& Field(std::string_view key, const P& value) {
      return FieldPath(key, value);
    }

    std::string_view View();

   private:
    ScopeLabel& FieldPath(std::string_view key, const std::filesystem::path& value);

    std::string text_;
    bool enabled_ = false;
    bool open_ = false;
  };
};

}
