#include "plugin/PluginLuaParseHelpers.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <cstddef>

#include "util/Hex.h"

namespace microide::plugin::lua_parse {

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

bool ReadBoolField(lua_State* state, int table_index, const char* key) {
  lua_getfield(state, table_index, key);
  const bool value = lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
  return value;
}

float ReadNumberField(lua_State* state, int table_index, const char* key, float fallback) {
  lua_getfield(state, table_index, key);
  const float value =
      lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
  lua_pop(state, 1);
  return value;
}

bool ReadStringField(lua_State* state, int table_index, const char* key, std::string* out) {
  lua_getfield(state, table_index, key);
  const bool ok = lua_isstring(state, -1) != 0;
  if (ok) {
    std::size_t len = 0;
    const char* s = lua_tolstring(state, -1, &len);
    out->assign(s, len);
  }
  lua_pop(state, 1);
  return ok;
}

bool ReadOptionalColorField(lua_State* state, int table_index, const char* key, SDL_Color* out,
                            std::string* error_message) {
  lua_getfield(state, table_index, key);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  const bool is_str = lua_isstring(state, -1) != 0;
  const auto color = is_str ? ParseHexColor(lua_tostring(state, -1)) : std::nullopt;
  lua_pop(state, 1);
  if (!color) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " must be #rrggbb or #rrggbbaa";
    }
    return false;
  }
  *out = *color;
  return true;
}

}  // namespace microide::plugin::lua_parse

#endif  // MICROIDE_HAS_LUA_PLUGINS
