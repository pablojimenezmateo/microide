#include "platform/TerminalLifeTrace.h"

#include "util/Log.h"
#include "util/PerformanceTrace.h"

#include <cstdlib>
#include <format>
#include <string>

#include <execinfo.h>

namespace microide::platform {

void TraceTerminalLife(std::string_view event, int pid, bool with_stack) {
  static const bool enabled = util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_TERMINAL_LIFE");
  if (!enabled) {
    return;
  }
  std::string report = std::format("microide terminal-life: {} pid={}", event, pid);
  if (with_stack) {
    // Deep enough to get past the backend and the session to the workspace code
    // that asked for the terminal.
    constexpr int kFrames = 24;
    void* frames[kFrames];
    const int count = ::backtrace(frames, kFrames);
    if (char** symbols = ::backtrace_symbols(frames, count); symbols != nullptr) {
      for (int i = 1; i < count; ++i) {  // frame 0 is this function
        report += std::format("\n  #{} {}", i, symbols[i]);
      }
      std::free(symbols);  // NOLINT(cppcoreguidelines-no-malloc): backtrace_symbols' contract
    }
  }
  util::Log(report);
}

}  // namespace microide::platform
