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

bool ParseLanguageServerRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     LanguageServerRegistration* out,
                                     std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "language server registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!id_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "language server requires id and language_id";
    }
    return false;
  }
  auto command_opt = ReadStringArrayField(state, table_index, "command");
  if (!command_opt) {
    if (error_message != nullptr) {
      *error_message = "language server command must be a string array";
    }
    return false;
  }
  if (command_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = "language server command cannot be empty";
    }
    return false;
  }
  out->contributed = PluginHost::ContributedLanguageServer{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseToolRegistration(lua_State* state,
                           const std::string& plugin_id,
                           ToolRegistration* out,
                           std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "tool registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto platform_opt = ReadStringField(state, table_index, "platform");
  auto url_opt = ReadStringField(state, table_index, "url");
  auto sha256_opt = ReadStringField(state, table_index, "sha256");
  if (!id_opt || !platform_opt || !url_opt || !sha256_opt) {
    if (error_message != nullptr) {
      *error_message = "tool requires id, platform, url, and sha256";
    }
    return false;
  }
  std::string label;
  if (auto label_opt = ReadStringField(state, table_index, "label")) {
    label = std::move(*label_opt);
  }
  std::string install_dir;
  if (auto dir_opt = ReadStringField(state, table_index, "install_dir")) {
    install_dir = std::move(*dir_opt);
  }
  out->contributed = PluginHost::ContributedTool{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(label),
      .platform = std::move(*platform_opt),
      .download_url = std::move(*url_opt),
      .sha256 = std::move(*sha256_opt),
      .install_dir = std::move(install_dir),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseDebuggerRegistration(lua_State* state,
                               const std::string& plugin_id,
                               DebuggerRegistration* out,
                               std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "debugger registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto type_opt = ReadStringField(state, table_index, "type");
  if (!id_opt || !type_opt) {
    if (error_message != nullptr) {
      *error_message = "debugger requires id and type";
    }
    return false;
  }
  auto command_opt = ReadStringArrayField(state, table_index, "command");
  if (!command_opt) {
    if (error_message != nullptr) {
      *error_message = "debugger command must be a string array";
    }
    return false;
  }
  if (command_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = "debugger command cannot be empty";
    }
    return false;
  }
  out->contributed = PluginHost::ContributedDebugger{
      .id = plugin_id + "." + *id_opt,
      .type = std::move(*type_opt),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseScmProviderRegistration(lua_State* state,
                                  const std::string& plugin_id,
                                  ScmProviderRegistration* out,
                                  std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "scm provider registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  if (!id_opt || !label_opt) {
    if (error_message != nullptr) {
      *error_message = "scm provider requires id and label";
    }
    return false;
  }
  const int snapshot_ref = ReadFunctionRefField(state, table_index, "snapshot");
  out->contributed = PluginHost::ContributedScmProvider{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime = snapshot_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::ScmProviderRuntime{
        .id = out->contributed.id,
        .plugin_id = plugin_id,
        .state = state,
        .snapshot_ref = snapshot_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseAnnotationProviderRegistration(lua_State* state,
                                         const std::string& plugin_id,
                                         AnnotationProviderRegistration* out,
                                         std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "annotation provider registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  auto type_opt = ReadStringField(state, table_index, "type");
  auto language_id_opt = ReadStringField(state, table_index, "language_id");
  if (!id_opt || !label_opt || !type_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "annotation provider requires id, label, type, and language_id";
    }
    return false;
  }
  const int provide_ref = ReadFunctionRefField(state, table_index, "provide");
  out->contributed = PluginHost::ContributedAnnotationProvider{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .type = std::move(*type_opt),
      .language_id = std::move(*language_id_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime = provide_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::AnnotationProviderRuntime{
        .id = out->contributed.id,
        .language_id = out->contributed.language_id,
        .type = out->contributed.type,
        .plugin_id = plugin_id,
        .state = state,
        .provide_ref = provide_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseAuthProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   AuthProviderRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "auth provider registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  if (!id_opt || !label_opt) {
    if (error_message != nullptr) {
      *error_message = "auth provider requires id and label";
    }
    return false;
  }
  const int login_ref = ReadFunctionRefField(state, table_index, "login");
  const int refresh_ref = ReadFunctionRefField(state, table_index, "refresh");
  const int logout_ref = ReadFunctionRefField(state, table_index, "logout");
  out->contributed = PluginHost::ContributedAuthProvider{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime =
      login_ref != LUA_NOREF || refresh_ref != LUA_NOREF || logout_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::AuthProviderRuntime{
        .id = out->contributed.id,
        .plugin_id = plugin_id,
        .state = state,
        .login_ref = login_ref,
        .refresh_ref = refresh_ref,
        .logout_ref = logout_ref,
    };
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseAiProviderRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 AiProviderRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "AI provider registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  auto type_opt = ReadStringField(state, table_index, "type");
  if (!id_opt || !label_opt || !type_opt) {
    if (error_message != nullptr) {
      *error_message = "AI provider requires id, label, and type";
    }
    return false;
  }

  std::vector<std::string> models;
  lua_getfield(state, table_index, "models");
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (lua_isstring(state, -1)) {
        models.push_back(lua_tostring(state, -1));
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);

  out->contributed = PluginHost::ContributedAiProvider{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .type = std::move(*type_opt),
      .models = std::move(models),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseExternalAgentRegistration(lua_State* state,
                                    const std::string& plugin_id,
                                    ExternalAgentRegistration* out,
                                    std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "external agent registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto label_opt = ReadStringField(state, table_index, "label");
  auto protocol_opt = ReadStringField(state, table_index, "protocol");
  auto command_opt = ReadStringArrayField(state, table_index, "command");
  if (!id_opt || !label_opt || !protocol_opt || !command_opt || command_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = "external agent requires id, label, protocol, and non-empty command";
    }
    return false;
  }

  std::vector<std::string> capabilities;
  lua_getfield(state, table_index, "capabilities");
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (lua_isstring(state, -1)) {
        capabilities.push_back(lua_tostring(state, -1));
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);

  out->contributed = PluginHost::ContributedExternalAgent{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .protocol = std::move(*protocol_opt),
      .command = std::move(*command_opt),
      .capabilities = std::move(capabilities),
      .plugin_id = plugin_id,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseMcpToolRegistration(lua_State* state,
                              const std::string& plugin_id,
                              McpToolRegistration* out,
                              std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "MCP tool registration output is required";
    }
    return false;
  }
  const int table_index = 1;
  auto id_opt = ReadStringField(state, table_index, "id");
  auto name_opt = ReadStringField(state, table_index, "name");
  auto description_opt = ReadStringField(state, table_index, "description");
  auto schema_opt = ReadStringField(state, table_index, "input_schema");
  if (!id_opt || !name_opt || !description_opt || !schema_opt) {
    if (error_message != nullptr) {
      *error_message = "MCP tool requires id, name, description, and input_schema";
    }
    return false;
  }
  const int run_ref = ReadFunctionRefField(state, table_index, "run");
  out->contributed = PluginHost::ContributedMcpTool{
      .id = plugin_id + "." + *id_opt,
      .name = std::move(*name_opt),
      .description = std::move(*description_opt),
      .input_schema = std::move(*schema_opt),
      .plugin_id = plugin_id,
  };
  out->has_runtime = run_ref != LUA_NOREF;
  if (out->has_runtime) {
    out->runtime = runtime_types::McpToolRuntime{
        .id = out->contributed.id,
        .plugin_id = plugin_id,
        .state = state,
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
