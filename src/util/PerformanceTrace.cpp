#include "util/PerformanceTrace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "util/StringUtil.h"

namespace microide::util {

namespace {

using Clock = std::chrono::steady_clock;

struct TraceState {
  std::mutex mutex;
  bool started = false;
  Clock::time_point origin{};
  int depth = 0;
};

TraceState& GetTraceState() {
  static TraceState state;
  return state;
}

bool ParseEnabledValue(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return !IsFalseyToken(value);
}

double ParseMinimumDurationMs(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return 0.0;
  }

  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || !std::isfinite(parsed)) {
    return 0.0;
  }
  return std::max(0.0, parsed);
}

void EnsureStarted(TraceState& state) {
  if (state.started) {
    return;
  }
  state.origin = Clock::now();
  state.started = true;
}

double DurationMs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

bool PerformanceTrace::Enabled() {
  static const bool enabled = ParseEnabledValue(std::getenv("MICROIDE_PERF_TRACE"));
  return enabled;
}

bool PerformanceTrace::FlagEnabled(const char* env_name) {
  if (env_name == nullptr || env_name[0] == '\0') {
    return false;
  }
  return ParseEnabledValue(std::getenv(env_name));
}

double PerformanceTrace::MinimumDurationMs() {
  static const double minimum_ms =
      ParseMinimumDurationMs(std::getenv("MICROIDE_PERF_TRACE_MIN_MS"));
  return minimum_ms;
}

PerformanceTrace::Scope::Scope(std::string_view label) {
  if (!Enabled()) {
    return;
  }

  // Copy the label only on the enabled path -- `label_` is read solely by the
  // destructor when `enabled_`. These scopes sit in hot paths (the highlight
  // query path constructs several per call), so an unconditional copy here was a
  // per-scope heap allocation in production even though tracing was off.
  label_.assign(label);
  TraceState& state = GetTraceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  EnsureStarted(state);
  enabled_ = true;
  depth_ = state.depth++;
  start_ = Clock::now();
}

PerformanceTrace::Scope::~Scope() {
  if (!enabled_) {
    return;
  }

  const Clock::time_point end = Clock::now();
  TraceState& state = GetTraceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  const double elapsed_ms = DurationMs(end - state.origin);
  const double duration_ms = DurationMs(end - start_);
  state.depth = std::max(0, state.depth - 1);

  if (duration_ms < MinimumDurationMs()) {
    return;
  }

  std::fprintf(stderr, "[perf] %8.2f ms total | %8.2f ms | %*s%s\n", elapsed_ms,
               duration_ms, depth_ * 2, "", label_.c_str());
  std::fflush(stderr);
}

}  // namespace microide::util
