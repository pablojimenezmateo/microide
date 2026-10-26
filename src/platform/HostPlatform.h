#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::platform {

// Ignore SIGPIPE process-wide so a write() to a child pipe/PTY whose reader has
// died returns EPIPE instead of terminating the whole process. Must be called once
// at startup before any subprocess/terminal/LSP I/O. Idempotent.
void IgnoreBrokenPipeSignal();

// Set the process's CHARACTER locale (LC_CTYPE only) to the user's, or to
// C.UTF-8 when the user's is not UTF-8 (a bare `C`/`POSIX` environment). The app
// is UTF-8 throughout, and Xlib converts text through the C library's locale: in
// the default "C" locale SDL's X11 title setter cannot convert a non-ASCII window
// title and leaves `_NET_WM_NAME` (what taskbars and Alt+Tab show) at its old
// value. Numbers stay locale-free (util/Parse.h, JsonValue use from_chars /
// to_chars), and LC_NUMERIC is not touched. Call once at startup, before any
// thread starts (setlocale is not thread-safe). Returns the LC_CTYPE now in
// effect.
std::string UseUtf8CharacterLocale();

// The current working directory of local process `pid` (/proc/<pid>/cwd), or
// empty when it is unknown: no such process, or not permitted.
std::filesystem::path ProcessWorkingDirectory(int pid);

// Sends SIGINT to local process `pid`, as Ctrl+C in its terminal would. True when
// the signal was delivered. A debugger tracing `pid` (gdb) catches it and stops the
// program instead of the program seeing it.
bool InterruptProcess(int pid);

}  // namespace microide::platform
