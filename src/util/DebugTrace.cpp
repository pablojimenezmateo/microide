#include "util/DebugTrace.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "util/JsonValue.h"
#include "util/StringUtil.h"

namespace microide::util {

namespace {

using Clock = std::chrono::steady_clock;

struct TraceState {
  std::mutex mutex;
  std::FILE* sink = nullptr;
  Clock::time_point origin{};
  bool origin_set = false;
};

TraceState& State() {
  static TraceState state;
  return state;
}

// Default sink when MICROIDE_DAP_LOG is set to an empty/truthy value rather than
// a path. Matches the path documented to the user by the --dap-log flag.
constexpr const char* kDefaultPath = "/tmp/microide-dap.log";

// Resolve the env-configured sink path. Returns empty if tracing is not enabled
// through the environment. A bare truthy value (e.g. "1") maps to the default.
std::string EnvSinkPath() {
  const char* value = std::getenv("MICROIDE_DAP_LOG");
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  const std::string normalized = ToLowerAscii(value);
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return {};
  }
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return kDefaultPath;
  }
  return value;
}

std::FILE* OpenSink(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "w");
  if (file == nullptr) {
    std::fprintf(stderr, "[dap-trace] failed to open log sink: %s\n", path.c_str());
    std::fflush(stderr);
  }
  return file;
}

// Lazily resolves the sink (honoring the env var on first use) and returns it,
// or nullptr when tracing is disabled. Caller must hold state.mutex.
std::FILE* EnsureSinkLocked(TraceState& state) {
  if (state.sink != nullptr) {
    return state.sink;
  }
  const std::string env_path = EnvSinkPath();
  if (!env_path.empty()) {
    state.sink = OpenSink(env_path);
  }
  return state.sink;
}

double ElapsedMsLocked(TraceState& state) {
  const Clock::time_point now = Clock::now();
  if (!state.origin_set) {
    state.origin = now;
    state.origin_set = true;
  }
  return std::chrono::duration<double, std::milli>(now - state.origin).count();
}

void WriteLineLocked(TraceState& state, const std::string& line) {
  std::FILE* sink = EnsureSinkLocked(state);
  if (sink == nullptr) {
    return;
  }
  const double ms = ElapsedMsLocked(state);
  std::fprintf(sink, "%10.2f  %s\n", ms, line.c_str());
  std::fflush(sink);
}

// Pull out a small summary so log scans don't require reading the whole JSON.
std::string SummarizeMessage(const JsonValue& msg) {
  std::string type = msg["type"].IsString() ? msg["type"].AsString() : "?";
  std::string what;
  if (msg["command"].IsString()) {
    what = msg["command"].AsString();
  } else if (msg["event"].IsString()) {
    what = msg["event"].AsString();
  }
  std::string summary = type;
  if (!what.empty()) {
    summary += ' ';
    summary += what;
  }
  summary += " seq=";
  summary += std::to_string(msg["seq"].AsInt(0));
  if (msg["request_seq"].AsInt(0) != 0) {
    summary += " req_seq=";
    summary += std::to_string(msg["request_seq"].AsInt(0));
  }
  if (type == "response") {
    // `success` is a JSON boolean — read it as such (AsInt returns 0 for a bool,
    // which would mislabel every successful response as a failure).
    summary += msg["success"].AsBool(false) ? " ok" : " FAIL";
  }
  return summary;
}

}  // namespace

bool DebugTrace::Enabled() {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return EnsureSinkLocked(state) != nullptr;
}

void DebugTrace::EnableToFile(const std::filesystem::path& path) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.sink != nullptr) {
    return;
  }
  state.sink = OpenSink(path.string());
  if (state.sink != nullptr) {
    ElapsedMsLocked(state);  // anchor the clock at enable time
    std::fprintf(state.sink, "%10.2f  trace start: %s\n", 0.0, path.string().c_str());
    std::fflush(state.sink);
  }
}

void DebugTrace::Message(const char* direction, const JsonValue& msg) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = direction;
  line += "  ";
  line += SummarizeMessage(msg);
  line += "  | ";
  line += SerializeJson(msg);
  WriteLineLocked(state, line);
}

void DebugTrace::Note(const char* category, std::string_view detail) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = "note ";
  line += category;
  line += ": ";
  line.append(detail);
  WriteLineLocked(state, line);
}

void DebugTrace::Note(const char* category, std::string_view detail, std::string_view a) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = "note ";
  line += category;
  line += ": ";
  line.append(detail);
  line += " ";
  line.append(a);
  WriteLineLocked(state, line);
}

void DebugTrace::Note(const char* category, std::string_view detail, long long n) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = "note ";
  line += category;
  line += ": ";
  line.append(detail);
  line += " ";
  line += std::to_string(n);
  WriteLineLocked(state, line);
}

void DebugTrace::Note(const char* category, std::string_view detail, std::string_view a,
                      long long n) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = "note ";
  line += category;
  line += ": ";
  line.append(detail);
  line += " ";
  line.append(a);
  line += "=";
  line += std::to_string(n);
  WriteLineLocked(state, line);
}

void DebugTrace::Note(const char* category, std::string_view detail, long long n1, long long n2) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (EnsureSinkLocked(state) == nullptr) {
    return;
  }
  std::string line = "note ";
  line += category;
  line += ": ";
  line.append(detail);
  line += " ";
  line += std::to_string(n1);
  line += " ";
  line += std::to_string(n2);
  WriteLineLocked(state, line);
}

}  // namespace microide::util
