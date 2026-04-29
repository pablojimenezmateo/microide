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

bool ParseTaskRegistration(lua_State* state,
                           const std::string& plugin_id,
                           TaskRegistration* out,
                           std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "task registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  if (!id_opt || !label_opt) {
    if (error_message != nullptr) {
      *error_message = "task requires id and label";
    }
    return false;
  }
  auto command_opt = ReadStringArrayField(state, table_index, "command");
  if (!command_opt) {
    if (error_message != nullptr) {
      *error_message = "task command must be a string array";
    }
    return false;
  }
  if (command_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = "task command cannot be empty";
    }
    return false;
  }

  std::string group;
  if (auto group_opt = ReadStringField(state, table_index, "group")) {
    group = std::move(*group_opt);
  }
  std::string cwd;
  if (auto cwd_opt = ReadStringField(state, table_index, "cwd")) {
    cwd = std::move(*cwd_opt);
  }
  bool run_in_shell = false;
  lua_getfield(state, table_index, "run_in_shell");
  if (lua_isboolean(state, -1)) {
    run_in_shell = lua_toboolean(state, -1) != 0;
  }
  lua_pop(state, 1);

  out->contributed = PluginHost::ContributedTask{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .group = std::move(group),
      .command = std::move(*command_opt),
      .cwd = std::move(cwd),
      .run_in_shell = run_in_shell,
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseMenuEntryRegistration(lua_State* state,
                                const std::string& plugin_id,
                                MenuEntryRegistration* out,
                                std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "menu entry registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto menu_opt = ReadStringField(state, table_index, "menu");
  auto action_opt = ReadStringField(state, table_index, "action");
  auto label_opt = ReadStringField(state, table_index, "label");
  if (!id_opt || !menu_opt || !action_opt || !label_opt) {
    if (error_message != nullptr) {
      *error_message = "menu entry requires id, menu, action, and label";
    }
    return false;
  }
  std::string accelerator;
  if (auto accel_opt = ReadStringField(state, table_index, "accelerator")) {
    accelerator = std::move(*accel_opt);
  }
  bool separator_before = false;
  lua_getfield(state, table_index, "separator_before");
  if (lua_isboolean(state, -1)) {
    separator_before = lua_toboolean(state, -1) != 0;
  }
  lua_pop(state, 1);
  out->contributed = PluginHost::ContributedMenuEntry{
      .id = plugin_id + "." + *id_opt,
      .menu = std::move(*menu_opt),
      .action = std::move(*action_opt),
      .label = std::move(*label_opt),
      .accelerator = std::move(accelerator),
      .separator_before = separator_before,
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseKeybindingRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 KeybindingRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "keybinding registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto action_opt = ReadStringField(state, table_index, "action");
  auto key_opt = ReadStringField(state, table_index, "key");
  if (!id_opt || !action_opt || !key_opt) {
    if (error_message != nullptr) {
      *error_message = "keybinding requires id, action, and key";
    }
    return false;
  }
  std::string context;
  if (auto context_opt = ReadStringField(state, table_index, "context")) {
    context = std::move(*context_opt);
  }
  out->contributed = PluginHost::ContributedKeybinding{
      .id = plugin_id + "." + *id_opt,
      .action = std::move(*action_opt),
      .key_chord = std::move(*key_opt),
      .context = std::move(context),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseSettingRegistration(lua_State* state,
                              const std::string& plugin_id,
                              SettingRegistration* out,
                              std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "setting registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto type_opt = ReadStringField(state, table_index, "type");
  if (!id_opt || !type_opt) {
    if (error_message != nullptr) {
      *error_message = "setting requires id and type";
    }
    return false;
  }
  std::string label;
  if (auto label_opt = ReadStringField(state, table_index, "label")) {
    label = std::move(*label_opt);
  }
  std::string description;
  if (auto desc_opt = ReadStringField(state, table_index, "description")) {
    description = std::move(*desc_opt);
  }
  std::string scope;
  if (auto scope_opt = ReadStringField(state, table_index, "scope")) {
    scope = std::move(*scope_opt);
  }
  std::string default_value;
  if (auto default_opt = ReadStringField(state, table_index, "default")) {
    default_value = std::move(*default_opt);
  }

  std::vector<std::string> enum_values;
  if (*type_opt == "enum") {
    lua_getfield(state, table_index, "enum_values");
    if (lua_istable(state, -1)) {
      const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(state, -1));
      for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(state, -1, i);
        if (lua_isstring(state, -1)) {
          enum_values.emplace_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }

  out->contributed = PluginHost::ContributedSettingSpec{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(label),
      .description = std::move(description),
      .type = std::move(*type_opt),
      .scope = std::move(scope),
      .default_value = std::move(default_value),
      .enum_values = std::move(enum_values),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseStatusItemRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 StatusItemRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "status item registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  if (!id_opt) {
    if (error_message != nullptr) {
      *error_message = "status item requires id";
    }
    return false;
  }
  std::string text;
  if (auto text_opt = ReadStringField(state, table_index, "text")) {
    text = std::move(*text_opt);
  }
  std::string tooltip;
  if (auto tooltip_opt = ReadStringField(state, table_index, "tooltip")) {
    tooltip = std::move(*tooltip_opt);
  }
  std::string alignment;
  if (auto align_opt = ReadStringField(state, table_index, "alignment")) {
    alignment = std::move(*align_opt);
  }
  int priority = 0;
  lua_getfield(state, table_index, "priority");
  if (lua_isinteger(state, -1)) {
    priority = static_cast<int>(lua_tointeger(state, -1));
  }
  lua_pop(state, 1);
  out->contributed = PluginHost::ContributedStatusItem{
      .id = plugin_id + "." + *id_opt,
      .text = std::move(text),
      .tooltip = std::move(tooltip),
      .alignment = std::move(alignment),
      .priority = priority,
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseFormatterRegistration(lua_State* state,
                                const std::string& plugin_id,
                                FormatterRegistration* out,
                                std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "formatter registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  auto label_opt = ReadStringField(state, table_index, "label");
  if (!id_opt || !language_id_opt || !label_opt) {
    if (error_message != nullptr) {
      *error_message = "formatter requires id, language_id, and label";
    }
    return false;
  }
  auto command_opt = ReadStringArrayField(state, table_index, "command");
  if (!command_opt) {
    if (error_message != nullptr) {
      *error_message = "formatter command must be a string array";
    }
    return false;
  }
  if (command_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = "formatter command cannot be empty";
    }
    return false;
  }
  out->contributed = PluginHost::ContributedFormatter{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .label = std::move(*label_opt),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseSaveParticipantRegistration(lua_State* state,
                                      const std::string& plugin_id,
                                      SaveParticipantRegistration* out,
                                      std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "save participant registration output is required";
    }
    return false;
  }
  const char* id = luaL_checkstring(state, 1);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  lua_pushvalue(state, 2);
  const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  out->contributed = PluginHost::ContributedSaveParticipant{
      .id = plugin_id + "." + std::string(id),
      .plugin_id = plugin_id,
  };
  out->runtime = runtime_types::SaveParticipantRuntime{
      .id = out->contributed.id,
      .plugin_id = plugin_id,
      .state = state,
      .function_ref = function_ref,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

}  // namespace microide::plugin::registration_parsers

#endif
