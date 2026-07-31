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

PerformanceTrace::ScopeLabel::ScopeLabel(std::string_view base) {
  if (!Enabled()) {
    return;
  }
  enabled_ = true;
  text_.assign(base);
}

PerformanceTrace::ScopeLabel& PerformanceTrace::ScopeLabel::Field(std::string_view key,
                                                                 std::string_view value) {
  if (!enabled_) {
    return *this;
  }
  text_ += open_ ? ',' : '(';
  open_ = true;
  text_.append(key);
  text_ += '=';
  text_.append(value);
  return *this;
}

PerformanceTrace::ScopeLabel& PerformanceTrace::ScopeLabel::Field(std::string_view key,
                                                                 long long value) {
  if (!enabled_) {
    return *this;
  }
  return Field(key, std::to_string(value));
}

std::string_view PerformanceTrace::ScopeLabel::View() {
  if (open_) {
    text_ += ')';
    open_ = false;
  }
  return text_;
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
