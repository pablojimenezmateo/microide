#pragma once

#include <cstdint>
#include <thread>

namespace microide::app {

// SIGTERM (logout, shutdown, `kill`), SIGINT (Ctrl+C in the launching terminal)
// and SIGHUP (that terminal closed) as ONE event on the SDL queue, so the app quits
// the way a window close does -- every project's session and unsaved buffers
// persisted for the next start -- instead of the alternatives it had: SIGHUP's
// default action killed the process with nothing saved since the last crash-safety
// flush, and SDL's own SIGTERM handler raised SDL_EVENT_QUIT, which the shell
// IGNORES while an unsaved-changes prompt is open, so a logout waited out its
// timeout and then SIGKILLed the app.
//
// The handler only writes the signal number to a close-on-exec self-pipe; a
// watcher thread reads it and pushes `event_type` with the number in user.code.
// Handlers (unlike a blocked mask) are reset by exec, so the shells, servers and
// tools the editor spawns get the default dispositions back. One per process.
class TerminationSignalWatcher {
 public:
  // Installs the handlers; `event_type` 0 or a failed pipe installs nothing.
  explicit TerminationSignalWatcher(std::uint32_t event_type);
  ~TerminationSignalWatcher();  // restores the default dispositions, joins
  TerminationSignalWatcher(const TerminationSignalWatcher&) = delete;
  TerminationSignalWatcher& operator=(const TerminationSignalWatcher&) = delete;

  bool installed() const { return installed_; }

 private:
  std::thread thread_;
  bool installed_ = false;
};

}  // namespace microide::app
