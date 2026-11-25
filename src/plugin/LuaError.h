#pragma once

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <string>
#include <string_view>

#include <lua.hpp>

namespace microide::plugin::lua_error_util {

inline constexpr int kPendingError = -1;

inline void PushMessage(lua_State* state, std::string_view message) {
  lua_pushlstring(state, message.data(), message.size());
}

inline void PushMessage(lua_State* state, const std::string& message, const char* fallback) {
  if (message.empty()) {
    lua_pushstring(state, fallback);
  } else {
    lua_pushlstring(state, message.data(), message.size());
  }
}

}

#endif
