#pragma once

#include <functional>
#include <string_view>

// Kernel-side diagnostic logging. The kernel (util, platform, project, terminal,
// persistence) must not name the windowing library, so it cannot call SDL_Log; it
// calls util::Log and the host decides where the line goes. The default sink writes
// to stderr, which is what SDL_Log does on this platform anyway, so an unbound host
// (a test binary, a headless agent) still sees the message.
//
// This is for rare, once-per-condition operational notices — an inotify limit hit, a
// degraded fallback taken. It is not a tracing facility: util::PerformanceTrace and
// util::DebugTrace are.
namespace microide::util {

using LogSink = std::function<void(std::string_view)>;

// Install the host's sink (the shell binds SDL_Log). Passing nullptr restores the
// stderr default. Not thread-safe against concurrent Log() calls; install once at
// startup before any worker thread runs.
void SetLogSink(LogSink sink);

void Log(std::string_view message);

}  // namespace microide::util
