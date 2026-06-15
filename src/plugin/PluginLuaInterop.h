#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_interop {

// Identifier validation shared by command/sidebar/provider registration and by
// plugin-id validation during load. Pure std; usable regardless of Lua support.
bool IsValidIdentifier(std::string_view value);

#if MICROIDE_HAS_LUA_PLUGINS
// Centralized Lua-table field readers. None of these raise a Lua error (they use
// only lua_getfield/lua_tostring/lua_pop/luaL_ref), so they are safe to call while
// C++ locals are live — no longjmp over destructors. Each leaves the stack balanced.
//
// ReadStringField returns "" when the field is absent or not a string (matches the
// provider-query call sites that branch on emptiness). ReadOptionalStringField
// returns nullopt instead, so registration parsers can detect missing required
// fields. table_index may be relative (e.g. -1) or absolute; it must address the
// table at call time.
std::string ReadStringField(lua_State* state, int table_index, const char* field);
std::optional<std::string> ReadOptionalStringField(lua_State* state,
                                                   int table_index,
                                                   const char* field);
int ReadFunctionRefField(lua_State* state, int table_index, const char* field);
std::optional<std::vector<std::string>> ReadStringArrayField(lua_State* state,
                                                             int table_index,
                                                             const char* field);

// Restores the Lua stack to its construction-time height on scope exit. Provider
// interop calls push a function + arguments before a protected call; when the call
// is skipped (e.g. the plugin lookup returns null so PCall never runs) those slots
// would otherwise leak and accumulate across repeated queries until the stack
// overflows. Declaring one of these right after acquiring the provider state makes
// every early return — success or failure — stack-balanced.
class StackResetGuard {
 public:
  explicit StackResetGuard(lua_State* state) : state_(state), base_(lua_gettop(state)) {}
  ~StackResetGuard() { lua_settop(state_, base_); }
  StackResetGuard(const StackResetGuard&) = delete;
  StackResetGuard& operator=(const StackResetGuard&) = delete;

 private:
  lua_State* state_;
  int base_;
};

void PushPosition(lua_State* state, std::size_t line, std::size_t column);
void PushRange(lua_State* state,
               std::size_t start_line,
               std::size_t start_column,
               std::size_t end_line,
               std::size_t end_column);
void PushHoverPosition(lua_State* state, std::size_t line, std::size_t column);
PluginHost::SidebarItem ReadSidebarItem(lua_State* state, int table_index);
#endif

}  // namespace microide::plugin::lua_interop
