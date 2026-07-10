#include "plugin/PluginRegistrationParsers.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <optional>

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::registration_parsers {
namespace {

// Field readers are centralized in lua_interop; ReadStringField here is the
// optional-returning variant so parsers can detect missing required fields.
using lua_interop::ReadFunctionRefField;
using lua_interop::ReadStringArrayField;
constexpr auto& ReadStringField = lua_interop::ReadOptionalStringField;

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
  lua_interop::GetFieldProtected(state, table_index, "run_in_shell");
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
  lua_interop::GetFieldProtected(state, table_index, "separator_before");
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
    lua_interop::GetFieldProtected(state, table_index, "enum_values");
    if (lua_istable(state, -1)) {
      // Clamp against a sparse-border lua_rawlen; matches the hardened harvests.
      constexpr lua_Integer kMaxEnumValues = 4096;
      const lua_Integer n =
          std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, -1)), kMaxEnumValues);
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
  lua_interop::GetFieldProtected(state, table_index, "priority");
  if (lua_isinteger(state, -1)) {
    priority = static_cast<int>(lua_tointeger(state, -1));
  }
  lua_pop(state, 1);
  std::string icon;
  if (auto icon_opt = ReadStringField(state, table_index, "icon")) {
    icon = std::move(*icon_opt);
  }
  std::string tone;
  if (auto tone_opt = ReadStringField(state, table_index, "tone")) {
    tone = std::move(*tone_opt);
  }
  std::string command;
  if (auto command_opt = ReadStringField(state, table_index, "command")) {
    command = std::move(*command_opt);
  }
  float progress = -1.0f;
  lua_interop::GetFieldProtected(state, table_index, "progress");
  if (lua_isnumber(state, -1)) {
    progress = std::clamp(static_cast<float>(lua_tonumber(state, -1)), 0.0f, 1.0f);
  }
  lua_pop(state, 1);
  out->contributed = PluginHost::ContributedStatusItem{
      .id = plugin_id + "." + *id_opt,
      .text = std::move(text),
      .tooltip = std::move(tooltip),
      .alignment = std::move(alignment),
      .priority = priority,
      .icon = std::move(icon),
      .tone = std::move(tone),
      .command = std::move(command),
      .progress = progress,
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
  // Validate without raising: luaL_checkstring/luaL_checktype would longjmp over
  // the caller's live std::string error_message. Report via error_message so the
  // wrapper raises only after that local has destructed.
  if (!lua_isstring(state, 1)) {
    if (error_message != nullptr) {
      *error_message = "save participant registration requires a string id";
    }
    return false;
  }
  if (lua_type(state, 2) != LUA_TFUNCTION) {
    if (error_message != nullptr) {
      *error_message = "save participant registration requires a function handler";
    }
    return false;
  }
  const char* id = lua_tostring(state, 1);
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

namespace {

bool ReadPairArrayField(lua_State* state,
                        int table_index,
                        const char* field,
                        std::vector<std::pair<std::string, std::string>>* out) {
  lua_interop::GetFieldProtected(state, table_index, field);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;  // optional
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return false;
  }
  // Cap the drain so a plugin returning an unbounded/huge sequence cannot grow
  // this vector without limit and hang/OOM the host.
  constexpr lua_Integer kMaxPairArrayItems = 100000;
  // Read entries with lua_rawgeti, never metamethod-invoking lua_geti: this parse
  // runs inside the setup PCall with the count-hook watchdog armed, so an adversarial
  // __index that raises would longjmp over the caller's live `contributed`
  // (ContributedBracketSet with std::string/std::vector members) and the wrapper's
  // error_message std::string, leaking them. A bracket-pair sequence never
  // legitimately resolves through __index. Mirrors ReadStringArrayField.
  for (lua_Integer i = 1; i <= kMaxPairArrayItems; ++i) {
    lua_rawgeti(state, -1, i);
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      break;
    }
    if (!lua_istable(state, -1)) {
      lua_pop(state, 2);
      return false;
    }
    lua_rawgeti(state, -1, 1);
    lua_rawgeti(state, -2, 2);
    if (!lua_isstring(state, -2) || !lua_isstring(state, -1)) {
      lua_pop(state, 4);
      return false;
    }
    out->emplace_back(lua_tostring(state, -2), lua_tostring(state, -1));
    lua_pop(state, 3);
  }
  lua_pop(state, 1);
  return true;
}

}  // namespace

bool ParseBracketSetRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 BracketSetRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "bracket set registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "bracket set requires language_id";
    }
    return false;
  }
  PluginHost::ContributedBracketSet contributed;
  contributed.language_id = std::move(*language_id_opt);
  contributed.plugin_id = plugin_id;
  if (!ReadPairArrayField(state, table_index, "pairs", &contributed.bracket_pairs) ||
      !ReadPairArrayField(state, table_index, "auto_close", &contributed.auto_close_pairs) ||
      !ReadPairArrayField(state, table_index, "surround", &contributed.surround_pairs)) {
    if (error_message != nullptr) {
      *error_message = "bracket set fields must be arrays of {open, close} pairs";
    }
    return false;
  }
  out->contributed = std::move(contributed);
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseCommentMarkersRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     CommentMarkersRegistration* out,
                                     std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "comment markers registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "comment markers require language_id";
    }
    return false;
  }
  PluginHost::ContributedCommentMarkers contributed;
  contributed.language_id = std::move(*language_id_opt);
  contributed.plugin_id = plugin_id;
  if (auto line_opt = ReadStringField(state, table_index, "line")) {
    contributed.line_comment = std::move(*line_opt);
  }
  if (auto open_opt = ReadStringField(state, table_index, "block_open")) {
    contributed.block_comment_open = std::move(*open_opt);
  }
  if (auto close_opt = ReadStringField(state, table_index, "block_close")) {
    contributed.block_comment_close = std::move(*close_opt);
  }
  out->contributed = std::move(contributed);
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseIndentRulesRegistration(lua_State* state,
                                  const std::string& plugin_id,
                                  IndentRulesRegistration* out,
                                  std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "indent rules registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "indent rules require language_id";
    }
    return false;
  }
  PluginHost::ContributedIndentRules contributed;
  contributed.language_id = std::move(*language_id_opt);
  contributed.plugin_id = plugin_id;
  if (auto opens = ReadStringArrayField(state, table_index, "indent_after_open")) {
    contributed.indent_after_open_patterns = std::move(*opens);
  }
  if (auto closes = ReadStringArrayField(state, table_index, "dedent_on_close")) {
    contributed.dedent_on_close_chars = std::move(*closes);
  }
  out->contributed = std::move(contributed);
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseSnippetRegistration(lua_State* state,
                              const std::string& plugin_id,
                              SnippetRegistration* out,
                              std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "snippet registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  auto prefix_opt = ReadStringField(state, table_index, "prefix");
  auto body_opt = ReadStringField(state, table_index, "body");
  if (!id_opt || !language_id_opt || !prefix_opt || !body_opt) {
    if (error_message != nullptr) {
      *error_message = "snippet requires id, language_id, prefix, and body";
    }
    return false;
  }
  std::string label;
  if (auto label_opt = ReadStringField(state, table_index, "label")) {
    label = std::move(*label_opt);
  }
  out->contributed = PluginHost::ContributedSnippet{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .prefix = std::move(*prefix_opt),
      .label = std::move(label),
      .body = std::move(*body_opt),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

}  // namespace microide::plugin::registration_parsers

#endif
