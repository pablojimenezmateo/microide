#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <SDL3/SDL.h>
#include <lua.hpp>
#endif

namespace microide::plugin::lua_interop {

// Identifier validation shared by command/sidebar/provider registration and by
// plugin-id validation during load. Pure std; usable regardless of Lua support.
bool IsValidIdentifier(std::string_view value);

// Accumulate a provider failure into `*error_message` without letting a flood of broken
// providers grow it without bound. Used by the completion/code-action, nav/reference, and
// hover harvest loops (TD-2026-07-17A-047/048/049): one failing provider must no longer
// suppress every healthy provider's results, so those loops now *continue* past a failed
// PCall and record the failure here instead of discarding accumulated results. A null
// `error_message` is a no-op; the aggregate is capped so a language with hundreds of
// broken providers cannot balloon a single warning string. Pure std.
void AppendProviderFailure(std::string* error_message, std::string_view kind,
                           std::string_view provider_id, std::string_view call_error);

#if MICROIDE_HAS_LUA_PLUGINS
// Pushes `table[field]` onto the stack, protected against a raising `__index`
// metamethod. A plugin controls the tables we harvest and can install a metatable
// whose `__index` raises (`setmetatable`+`error` are in the exposed base lib); a bare
// `lua_getfield` would then longjmp (Lua links as C) to the enclosing pcall, skipping
// the caller's live C++ destructors (UB + leak — the hard "no longjmp over C++ locals"
// invariant, from the read-in direction). This runs the metamethod-capable lookup
// inside a nested `lua_pcall` so any raise is caught as a status; on a raise (or an
// absent field) it pushes nil. Always leaves exactly one value on top; stack-balanced.
// Benign `__index` *defaults* still resolve (VSCode/JS prototype-chain parity). A table
// with no metatable takes an allocation-free fast path identical to `lua_getfield`.
// This is the ONLY sanctioned field-fetch in src/plugin; the ban on raw
// lua_getfield/lua_gettable/lua_geti is enforced by CheckPluginFieldReadsAre-
// MetamethodProtected (PluginLuaInterop.cpp is the one exempt TU that defines it).
void GetFieldProtected(lua_State* state, int table_index, const char* field);

// Centralized Lua-table field readers. Each fetches its field via GetFieldProtected
// and then uses only metamethod-free stack ops (lua_tostring/lua_toboolean/lua_pop/
// luaL_ref/…), so they never longjmp over live C++ locals. Each leaves the stack
// balanced.
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

// Length-preserving extraction of a host string from the value at `index`. Returns
// nullopt when the value is not a string/number OR contains an embedded NUL byte. Lua
// strings are length-bearing, so "foo\0bar" is a single distinct string; the legacy
// `std::string(lua_tostring(...))` path silently truncated it at the first NUL, which
// could collapse two distinct plugin ids to one host id, skip validation of the bytes
// after the NUL, or hand a truncated path/argument to a C-string OS call
// (TD-2026-07-17A-080). Every scalar id/label/path/argument read routes through here (or
// the field readers above, which share it); binary payloads that legitimately carry NULs
// use the explicit length-preserving ReadStringField(..., std::string*) overload instead.
// Like lua_tolstring, a number value is coerced to its string form in place.
std::optional<std::string> ToHostString(lua_State* state, int index);
int ReadFunctionRefField(lua_State* state, int table_index, const char* field);
std::optional<std::vector<std::string>> ReadStringArrayField(lua_State* state,
                                                             int table_index,
                                                             const char* field);

// Length-preserving string read (embedded NULs kept). Returns true when the field
// was a string — then `out` is assigned (possibly empty); otherwise returns false
// and leaves `out` untouched. The presentation parsers (decorations, surfaces) use
// this overload to distinguish "absent" from "present but empty" for validation,
// unlike the value-returning ReadStringField above.
bool ReadStringField(lua_State* state, int table_index, const char* field, std::string* out);

// Read a boolean field (`lua_toboolean` truthiness); missing/false => false.
bool ReadBoolField(lua_State* state, int table_index, const char* field);

// Read a numeric field; missing/non-number => `fallback`.
float ReadNumberField(lua_State* state, int table_index, const char* field, float fallback);

// Read an integer field. Returns nullopt when the field is absent or not an integer
// (Lua distinguishes integer from float subtypes); the value is returned as-is (no
// range/sign validation — callers that require positivity check it themselves).
std::optional<lua_Integer> ReadOptionalIntegerField(lua_State* state,
                                                    int table_index,
                                                    const char* field);

// Parse a `#rrggbb` or `#rrggbbaa` hex color. Returns nullopt for any other shape.
std::optional<SDL_Color> ParseHexColor(std::string_view text);

// Read an optional hex-color field into `*out`. A missing field keeps `*out`
// untouched and returns true; a present-but-malformed value sets `*error_message`
// (when non-null) and returns false.
bool ReadOptionalColorField(lua_State* state, int table_index, const char* field, SDL_Color* out,
                            std::string* error_message);

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
