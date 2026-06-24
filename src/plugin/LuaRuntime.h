#pragma once

#include <chrono>
#include <memory>
#include <string>

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

class LuaRuntime {
 public:
  static std::unique_ptr<LuaRuntime> Create(std::string* error_message);
  ~LuaRuntime();

  LuaRuntime(const LuaRuntime&) = delete;
  LuaRuntime& operator=(const LuaRuntime&) = delete;

#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state() const { return state_; }

  // Protected call with a watchdog: a runaway or hung plugin call (e.g. an
  // accidental infinite loop in a `provide` callback) is aborted once it exceeds
  // the call budget instead of freezing the single UI thread Lua runs on. See
  // the .cpp for why the count-hook abort is safe w.r.t. C++ destructors.
  bool PCall(int nargs, int nresults, std::string* error_message) const;

  // Override the per-call watchdog budget. Primarily for tests that need to trip
  // the watchdog quickly; production uses the generous default hang guard.
  void set_call_budget(std::chrono::steady_clock::duration budget) { call_budget_ = budget; }
#endif

 private:
  LuaRuntime() = default;

#if MICROIDE_HAS_LUA_PLUGINS
  static void TimeoutHook(lua_State* state, lua_Debug* ar);

  lua_State* state_ = nullptr;
  // Generous hang guard, not a stutter guard: the goal is "never freeze forever",
  // not "never stutter". Plugin queries are user-triggered, not per-frame.
  std::chrono::steady_clock::duration call_budget_ = std::chrono::milliseconds(750);
  mutable std::chrono::steady_clock::time_point deadline_{};
#endif
};

}  // namespace microide::plugin
