#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microide::util {

void MarkTracingMainThread();

bool TracingMainThreadIsKnown();

class TraceChannel {
 public:
  struct AggregateEntry {
    std::string label;
    std::uint64_t count = 0;
    double total_ms = 0.0;
    double self_ms = 0.0;
    double main_thread_self_ms = 0.0;
    double max_ms = 0.0;
  };

  static constexpr std::size_t kMaxAggregateLabels = 4096;

  TraceChannel(const char* prefix,
               const char* stream_env,
               const char* aggregate_env,
               const char* min_duration_env);
  ~TraceChannel();

  TraceChannel(const TraceChannel&) = delete;
  TraceChannel& operator=(const TraceChannel&) = delete;

  bool Enabled() const { return StreamEnabled() || AggregateEnabled(); }
  bool StreamEnabled() const { return stream_enabled_.load(std::memory_order_relaxed); }
  bool AggregateEnabled() const { return aggregate_enabled_.load(std::memory_order_relaxed); }
  double MinimumDurationMs() const { return minimum_duration_ms_; }

  void Reset();

  void ResetAggregate();

  void RecordSample(std::string_view label, double duration_ms);

  std::vector<AggregateEntry> AggregateSnapshot() const;

  void WriteAggregate(std::FILE* out) const;
  void DumpAggregateOnce();

 private:
  friend class TraceScope;

  using Clock = std::chrono::steady_clock;

  void RecordAggregate(std::string_view label, double total_ms, double self_ms, bool on_main_thread);
  Clock::time_point Origin() const;

  const char* prefix_ = "";
  std::atomic<bool> stream_enabled_{false};
  std::atomic<bool> aggregate_enabled_{false};
  bool dumped_ = false;
  double minimum_duration_ms_ = 0.0;
  std::size_t slot_ = 0;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TraceScope {
 public:
  TraceScope(TraceChannel& channel, std::string_view label) {
    if (channel.Enabled()) {
      Begin(channel, label);
    }
  }
  ~TraceScope() {
    if (channel_ != nullptr) {
      End();
    }
  }

  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  void Begin(TraceChannel& channel, std::string_view label);
  void End();

  TraceChannel* channel_ = nullptr;
  TraceScope* parent_ = nullptr;
  std::size_t slot_ = 0;
  std::string label_;
  std::chrono::steady_clock::time_point start_{};
  double child_ms_ = 0.0;
  int depth_ = 0;
};

template <typename Derived>
class TraceChannelFacade {
 public:
  static bool Enabled() { return Derived::Channel().Enabled(); }
  static bool SummaryEnabled() { return Derived::Channel().AggregateEnabled(); }

  static void ResetSummary() { Derived::Channel().ResetAggregate(); }
  static void WriteSummary(std::FILE* out) { Derived::Channel().WriteAggregate(out); }
  static void DumpSummaryOnce() { Derived::Channel().DumpAggregateOnce(); }

  class Scope {
   public:
    explicit Scope(std::string_view label) : scope_(Derived::Channel(), label) {}

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    TraceScope scope_;
  };
};

}
