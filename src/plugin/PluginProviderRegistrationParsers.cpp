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

int ReadFunctionRefField(lua_State* state, int table_index, const char* field) {
  lua_getfield(state, table_index, field);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  return luaL_ref(state, LUA_REGISTRYINDEX);
}

}  // namespace

bool ParseLanguageServerRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     LanguageServerRegistration* out,
                                     std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto language_id_opt = ReadStringField(state, 1, "language_id");
  auto command_opt = ReadStringArrayField(state, 1, "command");
  if (!id_opt || !language_id_opt || !command_opt || command_opt->empty()) return false;
  out->contributed = PluginHost::ContributedLanguageServer{
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
      .initialization_options = {},
      .settings = {},
  };
  // initialization_options / settings are accepted as JSON-string fields and
  // parsed host-side; malformed JSON is ignored (left Null).
  if (auto json = ReadStringField(state, 1, "initialization_options")) {
    if (auto parsed = util::ParseJson(*json)) {
      out->contributed.initialization_options = std::move(*parsed);
    }
  }
  if (auto json = ReadStringField(state, 1, "settings")) {
    if (auto parsed = util::ParseJson(*json)) {
      out->contributed.settings = std::move(*parsed);
    }
  }
  if (error_message) error_message->clear();
  return true;
}

bool ParseToolRegistration(lua_State* state,
                           const std::string& plugin_id,
                           ToolRegistration* out,
                           std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto platform_opt = ReadStringField(state, 1, "platform");
  auto url_opt = ReadStringField(state, 1, "url");
  auto sha256_opt = ReadStringField(state, 1, "sha256");
  if (!id_opt || !platform_opt || !url_opt || !sha256_opt) return false;
  std::string label;
  if (auto value = ReadStringField(state, 1, "label")) label = std::move(*value);
  std::string install_dir;
  if (auto value = ReadStringField(state, 1, "install_dir")) install_dir = std::move(*value);
  out->contributed = PluginHost::ContributedTool{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(label),
      .platform = std::move(*platform_opt),
      .download_url = std::move(*url_opt),
      .sha256 = std::move(*sha256_opt),
      .install_dir = std::move(install_dir),
      .plugin_id = plugin_id,
  };
  if (error_message) error_message->clear();
  return true;
}

bool ParseScmProviderRegistration(lua_State* state,
                                  const std::string& plugin_id,
                                  ScmProviderRegistration* out,
                                  std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto label_opt = ReadStringField(state, 1, "label");
  if (!id_opt || !label_opt) return false;
  const int snapshot_ref = ReadFunctionRefField(state, 1, "snapshot");
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
  if (error_message) error_message->clear();
  return true;
}

bool ParseAnnotationProviderRegistration(lua_State* state,
                                         const std::string& plugin_id,
                                         AnnotationProviderRegistration* out,
                                         std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto label_opt = ReadStringField(state, 1, "label");
  auto type_opt = ReadStringField(state, 1, "type");
  auto language_id_opt = ReadStringField(state, 1, "language_id");
  if (!id_opt || !label_opt || !type_opt || !language_id_opt) return false;
  const int provide_ref = ReadFunctionRefField(state, 1, "provide");
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
  if (error_message) error_message->clear();
  return true;
}

bool ParseAuthProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   AuthProviderRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto label_opt = ReadStringField(state, 1, "label");
  if (!id_opt || !label_opt) return false;
  const int login_ref = ReadFunctionRefField(state, 1, "login");
  const int refresh_ref = ReadFunctionRefField(state, 1, "refresh");
  const int logout_ref = ReadFunctionRefField(state, 1, "logout");
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
  if (error_message) error_message->clear();
  return true;
}

bool ParseAiProviderRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 AiProviderRegistration* out,
                                 std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto label_opt = ReadStringField(state, 1, "label");
  auto type_opt = ReadStringField(state, 1, "type");
  if (!id_opt || !label_opt || !type_opt) return false;
  std::vector<std::string> models;
  std::string runtime;
  std::string base_url;
  std::string default_model;
  if (auto value = ReadStringField(state, 1, "runtime")) {
    runtime = std::move(*value);
  }
  if (auto value = ReadStringField(state, 1, "base_url")) {
    base_url = std::move(*value);
  }
  if (auto value = ReadStringField(state, 1, "default_model")) {
    default_model = std::move(*value);
  }
  lua_getfield(state, 1, "models");
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (lua_isstring(state, -1)) models.push_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  out->contributed = PluginHost::ContributedAiProvider{
      .id = plugin_id + "." + *id_opt,
      .label = std::move(*label_opt),
      .type = std::move(*type_opt),
      .models = std::move(models),
      .runtime = std::move(runtime),
      .base_url = std::move(base_url),
      .default_model = std::move(default_model),
      .plugin_id = plugin_id,
  };
  if (error_message) error_message->clear();
  return true;
}

bool ParseExternalAgentRegistration(lua_State* state,
                                    const std::string& plugin_id,
                                    ExternalAgentRegistration* out,
                                    std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto label_opt = ReadStringField(state, 1, "label");
  auto protocol_opt = ReadStringField(state, 1, "protocol");
  auto command_opt = ReadStringArrayField(state, 1, "command");
  if (!id_opt || !label_opt || !protocol_opt || !command_opt || command_opt->empty()) return false;
  std::vector<std::string> capabilities;
  lua_getfield(state, 1, "capabilities");
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (lua_isstring(state, -1)) capabilities.push_back(lua_tostring(state, -1));
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
  if (error_message) error_message->clear();
  return true;
}

bool ParseMcpToolRegistration(lua_State* state,
                              const std::string& plugin_id,
                              McpToolRegistration* out,
                              std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto name_opt = ReadStringField(state, 1, "name");
  auto description_opt = ReadStringField(state, 1, "description");
  auto schema_opt = ReadStringField(state, 1, "input_schema");
  if (!id_opt || !name_opt || !description_opt || !schema_opt) return false;
  const int run_ref = ReadFunctionRefField(state, 1, "run");
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
  if (error_message) error_message->clear();
  return true;
}

}  // namespace microide::plugin::registration_parsers

#endif
