#pragma once

#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_provider_registration_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool RegisterTask(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTask>* tasks,
                  std::string* error_message);
bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* language_servers,
                            std::string* error_message);
bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::string* error_message);
bool RegisterDebugger(lua_State* state,
                      std::string_view plugin_id,
                      std::vector<PluginHost::ContributedDebugger>* debuggers,
                      std::string* error_message);
bool RegisterTestProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedTestProvider>* test_providers,
                          std::vector<runtime_types::TestProviderRuntime>* test_provider_runtimes,
                          std::string* error_message);
bool RegisterScmProvider(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedScmProvider>* scm_providers,
                         std::vector<runtime_types::ScmProviderRuntime>* scm_provider_runtimes,
                         std::string* error_message);
bool RegisterAnnotationProvider(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedAnnotationProvider>* annotation_providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* annotation_provider_runtimes,
    std::string* error_message);
bool RegisterAuthProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedAuthProvider>* auth_providers,
                          std::vector<runtime_types::AuthProviderRuntime>* auth_provider_runtimes,
                          std::string* error_message);
bool RegisterAiProvider(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedAiProvider>* ai_providers,
                        std::string* error_message);
bool RegisterExternalAgent(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedExternalAgent>* external_agents,
    std::string* error_message);
bool RegisterMcpTool(lua_State* state,
                     std::string_view plugin_id,
                     std::vector<PluginHost::ContributedMcpTool>* mcp_tools,
                     std::vector<runtime_types::McpToolRuntime>* mcp_tool_runtimes,
                     std::string* error_message);
#endif

}  // namespace microide::plugin::lua_provider_registration_interop
