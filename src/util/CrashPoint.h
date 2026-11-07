#pragma once

#include <atomic>
#include <string_view>

namespace microide::util {

// A test seam at a durable-write boundary: a place between two disk writes that
// a crash could split (a temporary written but not renamed, a file renamed but its
// directory not synced, a tree file written before the journal that records it).
//
// Production never installs a hook, so a crash point costs one relaxed atomic
// load. The mirror fault matrix (tests/MirrorFaultMatrixTests.cpp) installs one in
// a helper process that counts the points an operation passes on an undisturbed
// run and then SIGKILLs itself at each of them in turn: a crash between any two
// of the operation's disk writes, not only between its wire frames
// (TD-2026-11-02-430 item 1).
//
// `detail` names what the step acts on (the target of a rename, the directory
// synced), so a hook can model a power cut too: a rename no later sync of its
// directory covers is one a power cut may undo.
using CrashPointHook = void (*)(const char* site, std::string_view detail);

namespace detail {
inline std::atomic<CrashPointHook> g_crash_point_hook{nullptr};
}  // namespace detail

// Tests only. Pass nullptr to remove.
inline void SetCrashPointHook(CrashPointHook hook) noexcept {
  detail::g_crash_point_hook.store(hook, std::memory_order_release);
}

// For a caller whose detail costs something to build: build it only when armed.
inline bool CrashPointsArmed() noexcept {
  return detail::g_crash_point_hook.load(std::memory_order_relaxed) != nullptr;
}

inline void CrashPoint(const char* site, std::string_view what = {}) noexcept {
  if (const CrashPointHook hook = detail::g_crash_point_hook.load(std::memory_order_acquire)) {
    hook(site, what);
  }
}

}  // namespace microide::util
