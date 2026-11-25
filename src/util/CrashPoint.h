#pragma once

#include <atomic>
#include <string_view>

namespace microide::util {

using CrashPointHook = int (*)(const char* site, std::string_view detail);

namespace detail {
inline std::atomic<CrashPointHook> g_crash_point_hook{nullptr};
}

inline void SetCrashPointHook(CrashPointHook hook) noexcept {
  detail::g_crash_point_hook.store(hook, std::memory_order_release);
}

inline bool CrashPointsArmed() noexcept {
  return detail::g_crash_point_hook.load(std::memory_order_relaxed) != nullptr;
}

inline void CrashPoint(const char* site, std::string_view what = {}) noexcept {
  if (const CrashPointHook hook = detail::g_crash_point_hook.load(std::memory_order_acquire)) {
    (void)hook(site, what);
  }
}

[[nodiscard]] inline int FaultPoint(const char* site, std::string_view what = {}) noexcept {
  const CrashPointHook hook = detail::g_crash_point_hook.load(std::memory_order_acquire);
  return hook != nullptr ? hook(site, what) : 0;
}

}
