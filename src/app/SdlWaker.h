#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace microide::app {

// Binds util::PushWake to SDL's event queue for this process: a wake channel is an
// SDL custom event type (from SDL_RegisterEvents) and delivery is SDL_PushEvent.
// Called once from the entry point, before any producer thread starts. Until it is
// called every wake push fails and latches the shared owed bit, so the loop's
// fallback poll covers the gap rather than losing the wake.
//
// This is the ONE place the wake path knows about SDL. Producers name only
// util::WakeChannel, which is what lets them run in a process that has no window
// (the headless control channel today; a remote agent later).
//
// It also starts the lost-wake relay (app/LostWakeRelay.h, TD-2026-11-07-433): SDL
// 3.4 can drop a cross-thread push's wake, and the main loop's blocking waits are
// bracketed with SdlBlockingWaitScope so the relay can re-deliver one. Its nudge is
// a pushed event AND a signal to the main thread (SIGRTMIN + 7, SA_RESTART), which
// ends SDL's poll whether or not SDL's own wake gets through -- in the soak it did
// not, the nudge's included. Must run on the main thread.
void InstallSdlWaker();

// The main loop's bracket around SDL_WaitEvent / SDL_WaitEventTimeout.
class SdlBlockingWaitScope {
 public:
  SdlBlockingWaitScope();
  ~SdlBlockingWaitScope();
  SdlBlockingWaitScope(const SdlBlockingWaitScope&) = delete;
  SdlBlockingWaitScope& operator=(const SdlBlockingWaitScope&) = delete;
};

// The wake path's counters: wakes pushed from any thread, and the relay's nudges.
struct SdlWakeCounts {
  std::uint64_t pushes = 0;
  std::uint64_t nudges = 0;
  // Steady-clock ms of the first wake pushed during the current blocking wait (0: none).
  std::uint64_t first_push_in_wait_ms = 0;
};
SdlWakeCounts ReadSdlWakeCounts();

// How long after its first wake a blocking wait may end before it is reported
// (DescribeLateWake): a delivered wake ends it within a frame.
inline constexpr std::uint64_t kLateWakeReportMs = 250;

// A blocking wait that ended late although wakes were pushed while it slept: a
// wake SDL lost and the relay did not rescue. The line says whether the relay's
// nudge fired and what finally ended the wait -- soak 049ca74b0cee caught the main
// thread asleep in that wait for 3 s with a control request queued, and nothing
// said which half failed. nullopt for an ordinary wait.
// `owed_ms`: from the first wake pushed during the wait to its end.
inline std::optional<std::string> DescribeLateWake(std::uint64_t owed_ms, bool timed, std::int32_t timeout_ms,
                                                   SdlWakeCounts before, SdlWakeCounts after,
                                                   bool has_event, std::uint32_t event_type) {
  const std::uint64_t pushed = after.pushes - before.pushes;
  if (owed_ms < kLateWakeReportMs || pushed == 0) {
    return std::nullopt;
  }
  std::string line = "microide idle-wait: woke " + std::to_string(owed_ms) + " ms after its first wake (" +
                     (timed ? "timeout " + std::to_string(timeout_ms) + " ms" : std::string("no timeout")) +
                     ") with " + std::to_string(pushed) + " wake(s) pushed during it and " +
                     std::to_string(after.nudges - before.nudges) + " relay nudge(s); ended by ";
  line += has_event ? "event " + std::to_string(event_type) : std::string("its timeout");
  return line;
}

// True for the relay's nudge: an event that only wakes the loop and carries
// nothing to handle.
bool IsSdlWakeNudge(std::uint32_t event_type);

// Route util::Log — the kernel's operational-notice sink — into SDL's logger, so a
// kernel notice lands in the same stream as the shell's own SDL_Log lines instead of
// bypassing whatever SDL's log output is bound to.
void InstallSdlLogSink();

// Allocate a wake channel from SDL's custom event range. Returns 0 when SDL cannot
// allocate one; callers treat 0 as "waking disabled" and the shell records the
// degraded state via util::SetWakeRegistrationDegraded.
// `name` (a string literal: it is kept, not copied) names the channel in the
// frame trace (SdlEventName).
std::uint32_t RegisterSdlWakeChannel(const char* name);

// What a frame's event was, for the frame trace: a wake channel's registered
// name, else SDL's event type in hex ("0x400" for a key down).
std::string SdlEventName(std::uint32_t event_type);

}  // namespace microide::app
