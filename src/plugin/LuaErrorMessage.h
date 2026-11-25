#pragma once

#if MICROIDE_HAS_LUA_PLUGINS

#include <string>

#include <lua.hpp>

namespace microide::plugin {

inline std::string LuaErrorString(lua_State* state) {
  const char* message = lua_tostring(state, -1);
  return message != nullptr ? std::string(message) : std::string("unknown Lua error");
}

}

#endif
