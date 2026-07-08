#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

// Thrown by the installed lua_atpanic handler when a Lua error is raised with no
// protected frame above it (e.g. a metamethod raising during post-PCall result
// harvesting on the worker). The C build of Lua would otherwise abort() the whole
// process; converting to a C++ exception lets the worker's exception firewall log
// and abandon the offending job instead. Unlike a longjmp this unwinds C++ frames
// normally, so it never skips a live destructor.
class LuaPanicError : public std::runtime_error {
 public:
  explicit LuaPanicError(const std::string& message) : std::runtime_error(message) {}
};

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

  // Protected call for a *nested* callback invoked from inside a C interop
  // function that is already running under an outer PCall (today: the
  // `process.run_async` completion callback). It runs on its own fresh deadline,
  // then restores the enclosing call's watchdog (deadline + hook) on return. The
  // outer deadline matters because `run_async` deliberately blocks the worker on
  // the subprocess: by the time the callback runs, the outer call's budget can
  // already be spent, and without a reset the watchdog would abort an otherwise
  // healthy callback on its very first instruction.
  bool PCallNested(int nargs, int nresults, std::string* error_message) const;

  // Override the per-call watchdog budget. Primarily for tests that need to trip
  // the watchdog quickly; production uses the generous default hang guard.
  void set_call_budget(std::chrono::steady_clock::duration budget) { call_budget_ = budget; }
#endif

 private:
  LuaRuntime() = default;

#if MICROIDE_HAS_LUA_PLUGINS
  static void TimeoutHook(lua_State* state, lua_Debug* ar);

  // Custom lua_Alloc enforcing the per-state heap ceiling (see the .cpp). Denies
  // any growth past the budget so a runaway plugin gets a recoverable Lua memory
  // error instead of OOMing the host.
  static void* BoundedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

  // Per-state heap accounting for the custom allocator. Bounds how much memory a
  // single plugin can hold so a hostile or buggy plugin (`("x"):rep(2^40)`,
  // unbounded table growth) cannot OOM the whole host process. Lives as long as
  // state_ — lua_close in the destructor body still frees through the allocator,
  // and this member outlives that call.
  struct MemoryBudget {
    std::size_t used = 0;
    std::size_t limit = 0;
  };
  MemoryBudget memory_budget_;

  lua_State* state_ = nullptr;
  // Generous hang guard, not a stutter guard: the goal is "never freeze forever",
  // not "never stutter". Plugin queries are user-triggered, not per-frame.
  std::chrono::steady_clock::duration call_budget_ = std::chrono::milliseconds(750);
  mutable std::chrono::steady_clock::time_point deadline_{};
#endif
};

}  // namespace microide::plugin
