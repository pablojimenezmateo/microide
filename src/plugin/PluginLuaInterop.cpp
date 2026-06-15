#include "plugin/PluginLuaInterop.h"

#include <algorithm>
#include <cctype>

namespace microide::plugin::lua_interop {

bool IsValidIdentifier(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
  });
}

#if MICROIDE_HAS_LUA_PLUGINS
std::optional<std::string> ReadOptionalStringField(lua_State* state,
                                                   int table_index,
                                                   const char* field) {
  lua_getfield(state, table_index, field);
  if (!lua_isstring(state, -1)) {
    lua_pop(state, 1);
    return std::nullopt;
  }
  std::string value = lua_tostring(state, -1);
  lua_pop(state, 1);
  return value;
}

std::string ReadStringField(lua_State* state, int table_index, const char* field) {
  return ReadOptionalStringField(state, table_index, field).value_or(std::string{});
}

int ReadFunctionRefField(lua_State* state, int table_index, const char* field) {
  lua_getfield(state, table_index, field);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  return luaL_ref(state, LUA_REGISTRYINDEX);
}

std::optional<std::vector<std::string>> ReadStringArrayField(lua_State* state,
                                                             int table_index,
                                                             const char* field) {
  lua_getfield(state, table_index, field);
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return std::nullopt;
  }
  std::vector<std::string> values;
  for (lua_Integer i = 1;; ++i) {
    lua_geti(state, -1, i);
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      break;
    }
    if (!lua_isstring(state, -1)) {
      lua_pop(state, 2);
      return std::nullopt;
    }
    values.emplace_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return values;
}

void PushPosition(lua_State* state, std::size_t line, std::size_t column) {
  lua_createtable(state, 0, 2);
  lua_pushinteger(state, static_cast<lua_Integer>(line));
  lua_setfield(state, -2, "line");
  lua_pushinteger(state, static_cast<lua_Integer>(column));
  lua_setfield(state, -2, "column");
}

void PushRange(lua_State* state,
               std::size_t start_line,
               std::size_t start_column,
               std::size_t end_line,
               std::size_t end_column) {
  lua_createtable(state, 0, 2);
  PushPosition(state, start_line, start_column);
  lua_setfield(state, -2, "start");
  PushPosition(state, end_line, end_column);
  lua_setfield(state, -2, "end");
}

void PushHoverPosition(lua_State* state, std::size_t line, std::size_t column) {
  PushPosition(state, line, column);
}

PluginHost::SidebarItem ReadSidebarItem(lua_State* state, int table_index) {
  PluginHost::SidebarItem item;
  const int absolute_index = lua_absindex(state, table_index);

  lua_getfield(state, absolute_index, "label");
  if (lua_isstring(state, -1)) {
    item.label = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "detail");
  if (lua_isstring(state, -1)) {
    item.detail = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "path");
  if (lua_isstring(state, -1)) {
    item.path = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "line");
  if (lua_isinteger(state, -1)) {
    const lua_Integer line = lua_tointeger(state, -1);
    item.line = line > 0 ? static_cast<std::size_t>(line) : 0;
  }
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "column");
  if (lua_isinteger(state, -1)) {
    const lua_Integer column = lua_tointeger(state, -1);
    item.column = column > 0 ? static_cast<std::size_t>(column) : 0;
  }
  lua_pop(state, 1);

  return item;
}
#endif

}  // namespace microide::plugin::lua_interop
