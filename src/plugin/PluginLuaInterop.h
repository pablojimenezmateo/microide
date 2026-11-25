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

bool IsValidIdentifier(std::string_view value);

void AppendProviderFailure(std::string* error_message, std::string_view kind,
                           std::string_view provider_id, std::string_view call_error);

#if MICROIDE_HAS_LUA_PLUGINS
void GetFieldProtected(lua_State* state, int table_index, const char* field);

std::string ReadStringField(lua_State* state, int table_index, const char* field);
std::optional<std::string> ReadOptionalStringField(lua_State* state,
                                                   int table_index,
                                                   const char* field);

std::optional<std::string> ToHostString(lua_State* state, int index);
int ReadFunctionRefField(lua_State* state, int table_index, const char* field);
std::optional<std::vector<std::string>> ReadStringArrayField(lua_State* state,
                                                             int table_index,
                                                             const char* field);
std::optional<std::vector<std::vector<std::string>>> ReadStringArrayListField(
    lua_State* state, int table_index, const char* field);
std::string ReadTriggerCharactersField(lua_State* state, int table_index, const char* field,
                                       std::string_view fallback);

bool ReadStringField(lua_State* state, int table_index, const char* field, std::string* out);

bool ReadBoolField(lua_State* state, int table_index, const char* field);

float ReadNumberField(lua_State* state, int table_index, const char* field, float fallback);

std::optional<lua_Integer> ReadOptionalIntegerField(lua_State* state,
                                                    int table_index,
                                                    const char* field);

std::optional<SDL_Color> ParseHexColor(std::string_view text);

bool ReadOptionalColorField(lua_State* state, int table_index, const char* field, SDL_Color* out,
                            std::string* error_message);

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

}
