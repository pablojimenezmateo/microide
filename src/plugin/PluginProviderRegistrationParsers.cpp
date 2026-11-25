#include "plugin/PluginRegistrationParsers.h"

#include "platform/ProcessLauncher.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

#include "plugin/PluginLuaInterop.h"
#include "util/StringUtil.h"
#include "util/ErrorOut.h"

namespace microide::plugin::registration_parsers {
namespace {

using lua_interop::ReadBoolField;
using lua_interop::ReadFunctionRefField;
using lua_interop::ReadStringArrayField;
constexpr auto& ReadStringField = lua_interop::ReadOptionalStringField;

}

bool ParseLanguageServerRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     LanguageServerRegistration* out,
                                     std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  std::optional<std::vector<std::string>> command_opt;
  if (auto candidates = lua_interop::ReadStringArrayListField(state, 1, "commands")) {
    std::erase_if(*candidates, [](const std::vector<std::string>& argv) { return argv.empty(); });
    if (!candidates->empty()) {
      const std::string install_hint = ReadStringField(state, 1, "install_hint").value_or(
          "no language server found; install one of the listed programs");
      command_opt = platform::FirstAvailableCommand(*candidates, install_hint);
    }
  } else {
    command_opt = ReadStringArrayField(state, 1, "command");
  }
  std::vector<std::string> language_ids;
  if (auto ids = ReadStringArrayField(state, 1, "language_ids")) {
    language_ids = std::move(*ids);
  }
  if (language_ids.empty()) {
    if (auto single = ReadStringField(state, 1, "language_id")) {
      if (!single->empty()) language_ids.push_back(std::move(*single));
    }
  }
  const auto is_blank = [](const std::string& value) {
    return value.empty() || value.find_first_not_of(" \t\r\n") == std::string::npos;
  };
  if (std::any_of(language_ids.begin(), language_ids.end(), is_blank)) {
    return util::Fail(error_message, "language server language_ids must not contain empty entries");
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
    return util::Fail(error_message, "language provider requires id and language_id");
  }
  const int provide_ref = ReadFunctionRefField(state, 1, "provide");
  if (provide_ref == LUA_NOREF) {
    return util::Fail(error_message, "language provider requires a provide function");
  }
  std::string trigger_characters;
  if (kind == runtime_types::LanguageQueryKind::SignatureHelp) {
    trigger_characters =
        lua_interop::ReadTriggerCharactersField(state, 1, "trigger_characters", "(,");
  }
  out->has_runtime = true;
  out->runtime = runtime_types::LanguageQueryRuntime{
      .kind = kind,
      .id = plugin_id + "." + *id_opt,
      .language_id = std::move(*language_id_opt),
      .plugin_id = plugin_id,
      .trigger_characters = std::move(trigger_characters),
      .state = state,
      .provide_ref = provide_ref,
  };
  util::ClearError(error_message);
  return true;
}

bool ParseDebugAdapterRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   DebugAdapterRegistration* out,
                                   std::string* error_message) {
  if (out == nullptr) return false;
  auto id_opt = ReadStringField(state, 1, "id");
  auto command_opt = ReadStringArrayField(state, 1, "command");
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
  std::string arguments_json;
  if (auto json = ReadStringField(state, 1, "arguments"); json && !json->empty()) {
    if (!util::ParseJson(*json)) {
      return util::Fail(error_message, "launch config arguments must be valid JSON");
    }
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
  const auto is_hex_sha256 = [](std::string_view value) {
    if (value.size() != 64) return false;
    for (const char c : value) {
      if (util::IsAsciiHexDigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
  };
  if (!is_hex_sha256(*sha256_opt)) {
    return util::Fail(error_message, "tool sha256 must be a 64-character hex digest");
  }
  for (char& c : *sha256_opt) {
    c = util::ToLowerAsciiChar(static_cast<char>(c));
  }
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

}

#endif
