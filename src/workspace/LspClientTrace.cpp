#include "workspace/LspClientTrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace microide::workspace {
namespace {

bool LspLifecycleTraceEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MICROIDE_TRACE_LSP_LIFECYCLE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

// Milliseconds since the first traced event, so a lifecycle trace reads as a
// timeline that shows exactly where the seconds between spawn and Ready go, without
// a wall-clock the reader has to subtract. One epoch shared across all TUs.
long LspTraceElapsedMs() {
  static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
  return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - epoch)
                               .count());
}

}  // namespace

void TraceLspLifecycle(std::string_view language_id, int pid, std::string_view phase,
                       std::string_view detail) {
  if (!LspLifecycleTraceEnabled()) {
    return;
  }
  if (detail.empty()) {
    std::fprintf(stderr, "[lsp] +%ldms %.*s pid=%d %.*s\n", LspTraceElapsedMs(),
                 static_cast<int>(language_id.size()), language_id.data(), pid,
                 static_cast<int>(phase.size()), phase.data());
    return;
  }
  std::fprintf(stderr, "[lsp] +%ldms %.*s pid=%d %.*s | %.*s\n", LspTraceElapsedMs(),
               static_cast<int>(language_id.size()), language_id.data(), pid,
               static_cast<int>(phase.size()), phase.data(), static_cast<int>(detail.size()),
               detail.data());
}

}  // namespace microide::workspace
