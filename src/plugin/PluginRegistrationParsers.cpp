#include "plugin/PluginRegistrationParsers.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <optional>

namespace microide::plugin::registration_parsers {
namespace {

std::optional<std::string> ReadStringField(lua_State* state,
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

int ReadFunctionRefField(lua_State* state, int table_index, const char* field) {
  lua_getfield(state, table_index, field);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  return luaL_ref(state, LUA_REGISTRYINDEX);
}

}  // namespace

bool ParseCompletionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CompletionRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "completion registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!id_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "completion requires id and language_id";
    }
    return false;
  }
  std::string trigger_characters;
  if (auto trigger_opt = ReadStringField(state, table_index, "trigger_characters")) {
    trigger_characters = std::move(*trigger_opt);
  }
  const int provide_ref = ReadFunctionRefField(state, table_index, "provide");
  out->contributed = PluginHost::ContributedCompletion{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .trigger_characters = trigger_characters,
      .plugin_id = plugin_id,
  };
  out->has_runtime = provide_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::CompletionRuntime{
        .id = out->contributed.id,
        .language_id = out->contributed.language_id,
        .trigger_characters = out->contributed.trigger_characters,
        .plugin_id = out->contributed.plugin_id,
        .state = state,
        .provide_ref = provide_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseCodeActionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CodeActionRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "code action registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!id_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "code action requires id and language_id";
    }
    return false;
  }
  const int provide_ref = ReadFunctionRefField(state, table_index, "provide");
  out->contributed = PluginHost::ContributedCodeAction{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime = provide_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::CodeActionRuntime{
        .id = out->contributed.id,
        .language_id = out->contributed.language_id,
        .plugin_id = out->contributed.plugin_id,
        .state = state,
        .provide_ref = provide_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseTestProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   TestProviderRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "test provider registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!id_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "test provider requires id and language_id";
    }
    return false;
  }
  const int discover_ref = ReadFunctionRefField(state, table_index, "discover");
  const int run_ref = ReadFunctionRefField(state, table_index, "run");
  out->contributed = PluginHost::ContributedTestProvider{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime = discover_ref != LUA_NOREF || run_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::TestProviderRuntime{
        .id = out->contributed.id,
        .language_id = out->contributed.language_id,
        .plugin_id = out->contributed.plugin_id,
        .state = state,
        .discover_ref = discover_ref,
        .run_ref = run_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

}  // namespace microide::plugin::registration_parsers

#endif
