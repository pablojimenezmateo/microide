#include "util/StartupTrace.h"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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

bool StartupTrace::Enabled() {
  static const bool enabled = ParseEnabledValue(std::getenv("MICROIDE_STARTUP_TRACE"));
  return enabled;
}

void StartupTrace::Reset() {
  if (!Enabled()) {
    return;
  }
  TraceState& state = GetTraceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.origin = Clock::now();
  state.started = true;
  state.depth = 0;
}

StartupTrace::Scope::Scope(std::string_view label) : label_(label) {
  if (!Enabled()) {
    return;
  }
  TraceState& state = GetTraceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  EnsureStarted(state);
  enabled_ = true;
  depth_ = state.depth++;
  start_ = Clock::now();
}

StartupTrace::Scope::~Scope() {
  if (!enabled_) {
    return;
  }

  const Clock::time_point end = Clock::now();
  TraceState& state = GetTraceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  const double elapsed_ms = DurationMs(end - state.origin);
  const double duration_ms = DurationMs(end - start_);
  state.depth = std::max(0, state.depth - 1);

  std::fprintf(stderr, "[startup] %8.2f ms total | %8.2f ms | %*s%s\n", elapsed_ms,
               duration_ms, depth_ * 2, "", label_.c_str());
  std::fflush(stderr);
}

}  // namespace microide::util
