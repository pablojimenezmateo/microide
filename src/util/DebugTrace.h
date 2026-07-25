#pragma once

#include <filesystem>
#include <string_view>

namespace microide::util {

struct JsonValue;

// Diagnostic tracer for the debugger / DAP subsystem. Modeled on StartupTrace:
// a process-global, lazily-initialized sink that is silent unless explicitly
// enabled. Enable it either by setting the MICROIDE_DAP_LOG environment variable
// (value = file path; empty/"1" falls back to the default path) or via the
// `--dap-log [path]` startup flag, which calls EnableToFile().
//
// Every write is line-buffered and immediately flushed so the log can be read
// back live while microide is still running and the user is interacting.
//
// All formatting happens inside DebugTrace.cpp. Callers in render translation
// units (which are forbidden from materializing strings) only ever pass
// `const char*` / `std::string_view` / integral arguments, so they stay clean
// under the architecture-lint render-string rules.
class DebugTrace {
 public:
  // True once a sink has been opened (env or EnableToFile). Cheap to call.
  static bool Enabled();

  // Open `path` (truncating) as the trace sink. Safe to call once at startup
  // before any debug session exists. A failure to open is reported to stderr
  // and leaves the tracer disabled.
  static void EnableToFile(const std::filesystem::path& path);

  // Log one whole DAP protocol message. `direction` is "send" or "recv".
  // Serializes `msg` and records its type / command|event / seq for quick scans.
  static void Message(const char* direction, const JsonValue& msg);

  // Structured decision/note lines for the debug subsystem. The overloads keep
  // string assembly out of the callers (especially render TUs).
  static void Note(const char* category, std::string_view detail);
  static void Note(const char* category, std::string_view detail, std::string_view a);
  static void Note(const char* category, std::string_view detail, long long n);
  static void Note(const char* category, std::string_view detail, std::string_view a, long long n);
  static void Note(const char* category, std::string_view detail, long long n1, long long n2);
};

}  // namespace microide::util
