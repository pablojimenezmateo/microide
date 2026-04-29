#pragma once

#include <string>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::registration_parsers {

struct CompletionRegistration {
  PluginHost::ContributedCompletion contributed;
  runtime_types::CompletionRuntime runtime;
  bool has_runtime = false;
};

struct CodeActionRegistration {
  PluginHost::ContributedCodeAction contributed;
  runtime_types::CodeActionRuntime runtime;
  bool has_runtime = false;
};

struct TestProviderRegistration {
  PluginHost::ContributedTestProvider contributed;
  runtime_types::TestProviderRuntime runtime;
  bool has_runtime = false;
};

#if MICROIDE_HAS_LUA_PLUGINS
bool ParseCompletionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CompletionRegistration* out,
                                 std::string* error_message);

bool ParseCodeActionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CodeActionRegistration* out,
                                 std::string* error_message);

bool ParseTestProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   TestProviderRegistration* out,
                                   std::string* error_message);
#endif

}  // namespace microide::plugin::registration_parsers
