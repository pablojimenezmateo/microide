#pragma once

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
  bool PCall(int nargs, int nresults, std::string* error_message) const;
#endif

 private:
  LuaRuntime() = default;

#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state_ = nullptr;
#endif
};

}  // namespace microide::plugin
