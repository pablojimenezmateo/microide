#include "plugin/LuaRuntime.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <string>
#endif

namespace microide::plugin {

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
bool LuaRuntime::PCall(int nargs, int nresults, std::string* error_message) const {
  if (state_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Lua runtime unavailable";
    }
    return false;
  }
  if (lua_pcall(state_, nargs, nresults, 0) == LUA_OK) {
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
