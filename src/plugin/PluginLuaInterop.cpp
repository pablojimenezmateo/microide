#include "plugin/PluginLuaInterop.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

#if MICROIDE_HAS_LUA_PLUGINS
#include "util/Hex.h"
#endif

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
namespace {

// Light C function (zero upvalues => LUA_VLCF, no allocation on push). Runs the
// metamethod-capable lookup INSIDE the pcall frame that invokes it, so a raising
// __index unwinds to lua_pcall rather than past it. arg1 = table, arg2 = key.
int GetFieldTrampoline(lua_State* state) {
  lua_pushvalue(state, 2);
  lua_gettable(state, 1);
  return 1;
}

}  // namespace

void GetFieldProtected(lua_State* state, int table_index, const char* field) {
  const int absolute_index = lua_absindex(state, table_index);
  if (lua_getmetatable(state, absolute_index) == 0) {
    // No metatable => __index cannot fire => a plain read cannot raise. Fast path:
    // identical to lua_getfield, no pcall/closure overhead (lua_getmetatable pushes
    // nothing when absent, so the stack is already balanced for the getfield).
    lua_getfield(state, absolute_index, field);
    return;
  }
  lua_pop(state, 1);  // discard the metatable; presence alone forces the protected path
  lua_pushcfunction(state, GetFieldTrampoline);
  lua_pushvalue(state, absolute_index);
  lua_pushstring(state, field);
  if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
    lua_pop(state, 1);   // discard the error object
    lua_pushnil(state);  // a raising read is treated as an absent field
  }
}

std::optional<std::string> ReadOptionalStringField(lua_State* state,
                                                   int table_index,
                                                   const char* field) {
  GetFieldProtected(state, table_index, field);
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

bool ReadStringField(lua_State* state, int table_index, const char* field, std::string* out) {
  GetFieldProtected(state, table_index, field);
  const bool ok = lua_isstring(state, -1) != 0;
  if (ok) {
    std::size_t len = 0;
    const char* s = lua_tolstring(state, -1, &len);
    out->assign(s, len);
  }
  lua_pop(state, 1);
  return ok;
}

bool ReadBoolField(lua_State* state, int table_index, const char* field) {
  GetFieldProtected(state, table_index, field);
  const bool value = lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
  return value;
}

float ReadNumberField(lua_State* state, int table_index, const char* field, float fallback) {
  GetFieldProtected(state, table_index, field);
  const float value =
      lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
  lua_pop(state, 1);
  return value;
}

std::optional<lua_Integer> ReadOptionalIntegerField(lua_State* state,
                                                    int table_index,
                                                    const char* field) {
  GetFieldProtected(state, table_index, field);
  std::optional<lua_Integer> value;
  if (lua_isinteger(state, -1)) {
    value = lua_tointeger(state, -1);
  }
  lua_pop(state, 1);
  return value;
}

std::optional<SDL_Color> ParseHexColor(std::string_view text) {
  if (text.size() == 7 && text.front() == '#') {
    const auto rgb = util::DecodeHexColor(text);
    if (!rgb) {
      return std::nullopt;
    }
    return SDL_Color{(*rgb)[0], (*rgb)[1], (*rgb)[2], 255};
  }
  if (text.size() == 9 && text.front() == '#') {
    const auto r = util::ParseHexByte(text[1], text[2]);
    const auto g = util::ParseHexByte(text[3], text[4]);
    const auto b = util::ParseHexByte(text[5], text[6]);
    const auto a = util::ParseHexByte(text[7], text[8]);
    if (!r || !g || !b || !a) {
      return std::nullopt;
    }
    return SDL_Color{*r, *g, *b, *a};
  }
  return std::nullopt;
}

bool ReadOptionalColorField(lua_State* state, int table_index, const char* field, SDL_Color* out,
                            std::string* error_message) {
  GetFieldProtected(state, table_index, field);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  const bool is_str = lua_isstring(state, -1) != 0;
  const auto color = is_str ? ParseHexColor(lua_tostring(state, -1)) : std::nullopt;
  lua_pop(state, 1);
  if (!color) {
    if (error_message != nullptr) {
      *error_message = std::string(field) + " must be #rrggbb or #rrggbbaa";
    }
    return false;
  }
  *out = *color;
  return true;
}

int ReadFunctionRefField(lua_State* state, int table_index, const char* field) {
  GetFieldProtected(state, table_index, field);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  return luaL_ref(state, LUA_REGISTRYINDEX);
}

std::optional<std::vector<std::string>> ReadStringArrayField(lua_State* state,
                                                             int table_index,
                                                             const char* field) {
  GetFieldProtected(state, table_index, field);
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return std::nullopt;
  }
  // Cap the drain so a plugin returning an accidentally-huge sequence cannot grow
  // this vector without bound and hang/OOM the host. 100000 is far beyond any real
  // string-array field. Elements are read *raw* (lua_rawgeti, not lua_geti): a
  // sequence never legitimately resolves elements through __index, and a raising
  // __index metamethod on lua_geti would longjmp over the live `values` vector to
  // the enclosing pcall, skipping its destructor (the no-longjmp-over-C++-locals
  // invariant). Raw access also sidesteps the unbounded-__index drain entirely.
  constexpr lua_Integer kMaxLuaStringArrayItems = 100000;
  std::vector<std::string> values;
  for (lua_Integer i = 1; i <= kMaxLuaStringArrayItems; ++i) {
    lua_rawgeti(state, -1, i);
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

  GetFieldProtected(state, absolute_index, "label");
  if (lua_isstring(state, -1)) {
    item.label = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "detail");
  if (lua_isstring(state, -1)) {
    item.detail = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "path");
  if (lua_isstring(state, -1)) {
    item.path = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "line");
  if (lua_isinteger(state, -1)) {
    const lua_Integer line = lua_tointeger(state, -1);
    item.line = line > 0 ? static_cast<std::size_t>(line) : 0;
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "column");
  if (lua_isinteger(state, -1)) {
    const lua_Integer column = lua_tointeger(state, -1);
    item.column = column > 0 ? static_cast<std::size_t>(column) : 0;
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "id");
  if (lua_isstring(state, -1)) {
    item.id = lua_tostring(state, -1);
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "depth");
  if (lua_isinteger(state, -1)) {
    const lua_Integer depth = lua_tointeger(state, -1);
    item.depth = depth > 0 ? static_cast<int>(depth) : 0;
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "collapsible");
  if (lua_isboolean(state, -1)) {
    item.collapsible = lua_toboolean(state, -1) != 0;
  }
  lua_pop(state, 1);

  GetFieldProtected(state, absolute_index, "collapsed");
  if (lua_isboolean(state, -1)) {
    item.collapsed = lua_toboolean(state, -1) != 0;
  }
  lua_pop(state, 1);

  return item;
}
#endif

}  // namespace microide::plugin::lua_interop
