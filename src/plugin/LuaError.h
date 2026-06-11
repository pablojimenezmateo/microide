#pragma once

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <string>
#include <string_view>

#include <lua.hpp>

namespace microide::plugin::lua_error_util {

// Sentinel return value for delegating interop functions that live in their own
// translation unit (e.g. process_interop / runtime_api_interop) and are called by
// the thin lua_CFunction wrappers in PluginHostLuaApi.inc. Such a function MUST
// NOT call lua_error itself: the longjmp would unwind back through the wrapper
// frame, skipping the destructors of any temporaries the wrapper materialized to
// pass down. Instead it copies the error message via PushMessage and returns this
// sentinel; the wrapper raises with `lua_error(state)` only after its own locals
// have destructed. The value is negative so it can never be confused with a valid
// lua_CFunction result count (which is >= 0) and never reaches Lua directly.
inline constexpr int kPendingError = -1;

// Raising a Lua error is a C `longjmp`: the project links the C build of Lua, so
// `luaL_error` / `lua_error` jump straight back to the enclosing protected call
// without running the destructors of any C++ automatic objects still alive on the
// stack. Calling `luaL_error(state, "%s", message.c_str())` while a `std::string`,
// `std::vector`, or `std::filesystem::path` local is in scope therefore leaks it
// and is undefined behaviour (longjmp over a non-trivially-destructible object).
//
// The safe idiom is to copy the message into Lua-managed memory *first*, then let
// every C++ local go out of scope, and only then `lua_error`. Use these helpers
// at the bottom of an enclosing block:
//
//   {
//     std::string err;
//     if (DoWork(&err)) return 0;        // success path: normal unwind
//     lua_error::PushMessage(state, err, "fallback message");
//   }                                    // `err` destructs here
//   return lua_error(state);             // longjmp with nothing live
//
// `PushMessage` copies the bytes immediately, so the source string may safely
// destruct before the `lua_error` call. The architecture-lint rule
// `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` forbids reintroducing the
// `luaL_error(..., .c_str())` shape that this replaces.

// Copy `message` onto the Lua stack as the pending error object.
inline void PushMessage(lua_State* state, std::string_view message) {
  lua_pushlstring(state, message.data(), message.size());
}

// Copy `message` (or `fallback` when empty) onto the Lua stack as the pending
// error object. The common case for service calls that report failure through an
// out-param `std::string` that may be left empty.
inline void PushMessage(lua_State* state, const std::string& message, const char* fallback) {
  if (message.empty()) {
    lua_pushstring(state, fallback);
  } else {
    lua_pushlstring(state, message.data(), message.size());
  }
}

}  // namespace microide::plugin::lua_error_util

#endif
