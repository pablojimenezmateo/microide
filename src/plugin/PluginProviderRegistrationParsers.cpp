#include "plugin/PluginRegistrationParsers.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <optional>

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::registration_parsers {
namespace {

// Field readers are centralized in lua_interop; ReadStringField here is the
// optional-returning variant so parsers can detect missing required fields.
using lua_interop::ReadBoolField;
using lua_interop::ReadFunctionRefField;
using lua_interop::ReadStringArrayField;
constexpr auto& ReadStringField = lua_interop::ReadOptionalStringField;

}  // namespace

bool ParseLanguageServerRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     LanguageServerRegistration* out,
                                     std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto command_opt = ReadStringArrayField(state, 1, "command");
  // Accept either a `language_ids` array (one process serves several languages)
  // or a single `language_id` string; the latter folds into a one-element list.
  std::vector<std::string> language_ids;
  if (auto ids = ReadStringArrayField(state, 1, "language_ids")) {
    language_ids = std::move(*ids);
  }
  if (language_ids.empty()) {
    if (auto single = ReadStringField(state, 1, "language_id")) {
      if (!single->empty()) language_ids.push_back(std::move(*single));
    }
  }
  if (!id_opt || language_ids.empty() || !command_opt || command_opt->empty()) return false;
  out->contributed = PluginHost::ContributedLanguageServer{
      .id = plugin_id + "." + *id_opt,
      .language_ids = std::move(language_ids),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
      .initialization_options = {},
      .settings = {},
      .eager_start = ReadBoolField(state, 1, "eager_start"),
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

bool ParseLanguageQueryRegistration(lua_State* state,
                                    const std::string& plugin_id,
                                    runtime_types::LanguageQueryKind kind,
                                    LanguageQueryRegistration* out,
                                    std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto language_id_opt = ReadStringField(state, 1, "language_id");
  if (!id_opt || !language_id_opt) {
    if (error_message != nullptr) {
      *error_message = "language provider requires id and language_id";
    }
    return false;
  }
  const int provide_ref = ReadFunctionRefField(state, 1, "provide");
  if (provide_ref == LUA_NOREF) {
    if (error_message != nullptr) {
      *error_message = "language provider requires a provide function";
    }
    return false;
  }
  out->has_runtime = true;
  out->runtime = runtime_types::LanguageQueryRuntime{
      .kind = kind,
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .plugin_id = plugin_id,
      .state = state,
      .provide_ref = provide_ref,
  };
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ParseDebugAdapterRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   DebugAdapterRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto command_opt = ReadStringArrayField(state, 1, "command");
  // `type` is the DAP adapter type id matched by a LaunchConfig; default it to
  // the local id when omitted so a single-adapter plugin can stay terse.
  auto type_opt = ReadStringField(state, 1, "type");
  std::string type = (type_opt && !type_opt->empty()) ? std::move(*type_opt)
                     : (id_opt ? *id_opt : std::string());
  if (!id_opt || type.empty() || !command_opt || command_opt->empty()) return false;
  out->contributed = PluginHost::ContributedDebugAdapter{
      .id = plugin_id + "." + *id_opt,
      .type = std::move(type),
      .command = std::move(*command_opt),
      .plugin_id = plugin_id,
  };
  if (error_message) error_message->clear();
  return true;
}

bool ParseLaunchConfigRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   LaunchConfigRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto type_opt = ReadStringField(state, 1, "type");
  if (!id_opt || id_opt->empty() || !type_opt || type_opt->empty()) return false;
  auto name_opt = ReadStringField(state, 1, "name");
  auto request_opt = ReadStringField(state, 1, "request");
  // `arguments` is a JSON-string field (same convention as LSP
  // initialization_options); the host parses it into the launch request body.
  std::string arguments_json;
  if (auto json = ReadStringField(state, 1, "arguments")) {
    arguments_json = std::move(*json);
  }
  out->contributed = PluginHost::ContributedLaunchConfig{
      .id = plugin_id + "." + *id_opt,
      .name = (name_opt && !name_opt->empty()) ? std::move(*name_opt) : *id_opt,
      .type = std::move(*type_opt),
      .request = (request_opt && !request_opt->empty()) ? std::move(*request_opt)
                                                        : std::string("launch"),
      .arguments_json = std::move(arguments_json),
      .plugin_id = plugin_id,
  };
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
    // Cap the drain: a plugin table with an unbounded/huge sequence must not grow
    // this vector without limit. A provider never has anywhere near this many models.
    constexpr lua_Integer kMaxProviderModels = 100000;
    for (lua_Integer i = 1; i <= kMaxProviderModels; ++i) {
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
    // Cap the drain against an unbounded/huge plugin table (see models above).
    constexpr lua_Integer kMaxAgentCapabilities = 100000;
    for (lua_Integer i = 1; i <= kMaxAgentCapabilities; ++i) {
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
