#include "plugin/LuaRuntime.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "plugin/LuaError.h"
#endif

namespace microide::plugin {

#if MICROIDE_HAS_LUA_PLUGINS
namespace {

// The count hook fires every `kHookInstructionBatch` Lua bytecode instructions.
// A large batch keeps the steady-clock read off the per-instruction path so the
// watchdog adds negligible overhead to normal plugin execution (speed first).
constexpr int kHookInstructionBatch = 100000;

// Per-plugin-state heap ceiling. Generous enough for real plugin work, but far
// below what it takes to OOM the host, so a plugin allocating without bound gets
// a clean Lua "not enough memory" error (caught by the enclosing PCall) instead
// of taking the process down.
constexpr std::size_t kLuaMemoryLimitBytes = 256ull * 1024 * 1024;

// Panic handler installed via lua_atpanic. Lua calls this only when an error is
// raised with NO protected frame on the stack — the C build's default then calls
// abort(). That can happen when interop C++ reads plugin-supplied result tables
// after a PCall returns and a field access triggers a raising `__index`
// metamethod (see PluginProviderQueryInterop harvesting). Throwing here converts
// the would-be abort into a normal C++ exception that unwinds back to the worker
// loop's firewall, which logs it and abandons the job. On the platforms we ship,
// the intervening Lua C frames carry unwind tables, so the throw propagates
// cleanly and runs C++ destructors on the way out (unlike a longjmp).
[[noreturn]] int LuaPanic(lua_State* state) {
  const char* message = lua_tostring(state, -1);
  throw LuaPanicError(message != nullptr ? message : "unprotected Lua error");
}

}  // namespace
#endif

std::unique_ptr<LuaRuntime> LuaRuntime::Create(std::string* error_message) {
#if MICROIDE_HAS_LUA_PLUGINS
  auto runtime = std::unique_ptr<LuaRuntime>(new LuaRuntime());
  // Bounded allocator: the budget lives in the runtime (destroyed after the
  // destructor's lua_close), so it is valid for the whole state lifetime.
  runtime->memory_budget_.limit = kLuaMemoryLimitBytes;
  runtime->state_ = lua_newstate(&LuaRuntime::BoundedAlloc, &runtime->memory_budget_);
  if (runtime->state_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "failed to create Lua state";
    }
    return nullptr;
  }
  // Replace the C build's abort()-on-panic with a throwing handler so an
  // unprotected Lua error (e.g. a raising metamethod during result harvesting)
  // becomes a catchable C++ exception instead of killing the editor.
  lua_atpanic(runtime->state_, &LuaPanic);

  // Expose only a narrow baseline stdlib set.
  luaL_requiref(runtime->state_, "_G", luaopen_base, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_LOADLIBNAME, luaopen_package, 1);
  lua_pop(runtime->state_, 1);

  // Remove the base library's file loaders (TD-2026-07-17A-128): global `loadfile` and
  // `dofile` open and execute a file straight off the filesystem, bypassing the plugin's
  // declared fs.read capability and the host-owned `ctx.files.*` containment resolver. A
  // plugin declaring `fs = { read = "none" }` could otherwise `dofile("../outside.lua")`.
  // Plugin descriptors are loaded host-side via luaL_loadfile, so nothing in-VM needs
  // these; `load` (string/reader chunks only, no filesystem) is left intact.
  lua_pushnil(runtime->state_);
  lua_setglobal(runtime->state_, "loadfile");
  lua_pushnil(runtime->state_);
  lua_setglobal(runtime->state_, "dofile");

  // Stash the owning runtime in the per-state extra space so the watchdog hook
  // (a plain C callback that only receives lua_State*) can reach the deadline.
  *static_cast<LuaRuntime**>(lua_getextraspace(runtime->state_)) = runtime.get();
  return runtime;
#else
  (void)error_message;
  return nullptr;
#endif
}

LuaRuntime::~LuaRuntime() {
#if MICROIDE_HAS_LUA_PLUGINS
  if (state_ != nullptr) {
    lua_close(state_);
    state_ = nullptr;
  }
#endif
}

#if MICROIDE_HAS_LUA_PLUGINS
// Custom lua_Alloc enforcing kLuaMemoryLimitBytes. Denies (returns nullptr) any
// growth that would push the state past its budget; Lua turns that into a
// recoverable memory error. Per the Lua manual, when `ptr` is null `osize` is a
// type tag rather than a real size, so the old size counts as 0 for accounting.
void* LuaRuntime::BoundedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* budget = static_cast<LuaRuntime::MemoryBudget*>(ud);
  const std::size_t old_size = (ptr == nullptr) ? 0 : osize;

  if (nsize == 0) {
    std::free(ptr);
    budget->used = (budget->used >= old_size) ? budget->used - old_size : 0;
    return nullptr;
  }

  if (nsize > old_size) {
    const std::size_t growth = nsize - old_size;
    if (budget->used + growth > budget->limit) {
      return nullptr;  // deny: over budget -> Lua raises "not enough memory"
    }
  }

  void* new_ptr = std::realloc(ptr, nsize);
  if (new_ptr == nullptr) {
    return nullptr;  // real allocation failure; leave accounting unchanged
  }
  budget->used = budget->used - old_size + nsize;
  return new_ptr;
}

// Count hooks fire only at Lua bytecode boundaries, never inside a C interop
// function, so when we raise here no C++ std::string / vector / path local is
// live on the native stack and the longjmp (caught by the enclosing lua_pcall)
// cannot skip a non-trivial destructor. We raise with the lua_error_util idiom
// rather than luaL_error, which is banned in src/plugin.
void LuaRuntime::TimeoutHook(lua_State* state, lua_Debug* /*ar*/) {
  const LuaRuntime* runtime = *static_cast<LuaRuntime**>(lua_getextraspace(state));
  if (runtime == nullptr || std::chrono::steady_clock::now() <= runtime->deadline_) {
    return;
  }
  lua_error_util::PushMessage(state, "plugin call exceeded its time budget (possible infinite loop)");
  lua_error(state);
}

bool LuaRuntime::PCall(int nargs, int nresults, std::string* error_message) const {
  if (state_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Lua runtime unavailable";
    }
    return false;
  }

  deadline_ = std::chrono::steady_clock::now() + call_budget_;
  lua_sethook(state_, &LuaRuntime::TimeoutHook, LUA_MASKCOUNT, kHookInstructionBatch);
  const int status = lua_pcall(state_, nargs, nresults, 0);
  lua_sethook(state_, nullptr, 0, 0);

  if (status == LUA_OK) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }
  if (error_message != nullptr) {
    const char* raw = lua_tostring(state_, -1);
    *error_message = raw != nullptr ? std::string(raw) : std::string("unknown Lua error");
  }
  lua_pop(state_, 1);
  return false;
}

bool LuaRuntime::PCallNested(int nargs, int nresults, std::string* error_message) const {
  if (state_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Lua runtime unavailable";
    }
    return false;
  }

  // Save the enclosing call's watchdog so the remainder of the outer call stays
  // bounded against its original deadline after this callback returns. We arm the
  // hook explicitly rather than assume the outer PCall left it installed, so this
  // helper is correct even if a future caller is not itself under a PCall.
  const std::chrono::steady_clock::time_point outer_deadline = deadline_;
  const lua_Hook outer_hook = lua_gethook(state_);
  const int outer_mask = lua_gethookmask(state_);
  const int outer_count = lua_gethookcount(state_);

  deadline_ = std::chrono::steady_clock::now() + call_budget_;
  lua_sethook(state_, &LuaRuntime::TimeoutHook, LUA_MASKCOUNT, kHookInstructionBatch);
  const int status = lua_pcall(state_, nargs, nresults, 0);
  lua_sethook(state_, outer_hook, outer_mask, outer_count);
  deadline_ = outer_deadline;

  if (status == LUA_OK) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }
  if (error_message != nullptr) {
    const char* raw = lua_tostring(state_, -1);
    *error_message = raw != nullptr ? std::string(raw) : std::string("unknown Lua error");
  }
  lua_pop(state_, 1);
  return false;
}
#endif

}  // namespace microide::plugin
