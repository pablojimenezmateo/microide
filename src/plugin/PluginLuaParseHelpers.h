#pragma once

#include <optional>
#include <string>
#include <string_view>

#if MICROIDE_HAS_LUA_PLUGINS
#include <SDL3/SDL.h>
#include <lua.hpp>
#endif

// Small, shared Lua-table field readers used by the plugin interop translation
// units (decorations, surfaces, ...). They were duplicated verbatim across two
// TUs; centralizing them keeps the parse behavior identical and the interop TUs
// well under the 800-line cap.
//
// All helpers are pure stack reads: they push and pop their own values and never
// raise (no `luaL_error`), so a caller can keep destructing its C++ locals before
// reporting a `kPendingError`.
namespace microide::plugin::lua_parse {

#if MICROIDE_HAS_LUA_PLUGINS

// Parse a `#rrggbb` or `#rrggbbaa` hex color. Returns nullopt for any other shape.
std::optional<SDL_Color> ParseHexColor(std::string_view text);

// Read a boolean field (`lua_toboolean` truthiness); missing/false => false.
bool ReadBoolField(lua_State* state, int table_index, const char* key);

// Read a numeric field; missing/non-number => `fallback`.
float ReadNumberField(lua_State* state, int table_index, const char* key, float fallback);

// Read a string field into `out` (length-preserving, embedded NULs kept). Returns
// true when the field was a string (then `out` is assigned, possibly empty);
// returns false and leaves `out` untouched otherwise. Callers that require a
// non-empty value should additionally check `out->empty()`.
bool ReadStringField(lua_State* state, int table_index, const char* key, std::string* out);

// Read an optional hex-color field `key` into `*out`. A missing field keeps `*out`
// untouched and returns true; a present-but-malformed value sets `*error_message`
// (when non-null) and returns false.
bool ReadOptionalColorField(lua_State* state, int table_index, const char* key, SDL_Color* out,
                            std::string* error_message);

#endif  // MICROIDE_HAS_LUA_PLUGINS

}  // namespace microide::plugin::lua_parse
