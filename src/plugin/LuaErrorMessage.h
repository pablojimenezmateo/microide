#pragma once

#if MICROIDE_HAS_LUA_PLUGINS

#include <string>

#include <lua.hpp>

namespace microide::plugin {

// Read the top-of-stack Lua value as an error message string.
// Returns "unknown Lua error" when the top is not convertible.
inline std::string LuaErrorString(lua_State* state) {
  const char* message = lua_tostring(state, -1);
  return message != nullptr ? std::string(message) : std::string("unknown Lua error");
}

}  // namespace microide::plugin

#endif  // MICROIDE_HAS_LUA_PLUGINS
