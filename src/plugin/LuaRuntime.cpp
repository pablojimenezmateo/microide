#include "plugin/LuaRuntime.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <chrono>
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

}  // namespace
#endif

std::unique_ptr<LuaRuntime> LuaRuntime::Create(std::string* error_message) {
#if MICROIDE_HAS_LUA_PLUGINS
  auto runtime = std::unique_ptr<LuaRuntime>(new LuaRuntime());
  runtime->state_ = luaL_newstate();
  if (runtime->state_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "failed to create Lua state";
    }
    return nullptr;
  }

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
#endif

}  // namespace microide::plugin
