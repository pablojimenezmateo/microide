#pragma once

#include <cstdint>

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
void InstallSdlWaker();

// Route util::Log — the kernel's operational-notice sink — into SDL's logger, so a
// kernel notice lands in the same stream as the shell's own SDL_Log lines instead of
// bypassing whatever SDL's log output is bound to.
void InstallSdlLogSink();

// Allocate a wake channel from SDL's custom event range. Returns 0 when SDL cannot
// allocate one; callers treat 0 as "waking disabled" and the shell records the
// degraded state via util::SetWakeRegistrationDegraded.
std::uint32_t RegisterSdlWakeChannel();

}  // namespace microide::app
