#pragma once

#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"
#include "plugin/PluginRegistrationParsers.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::contribution_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message);

bool RegisterSaveParticipant(lua_State* state,
                             std::string_view plugin_id,
                             std::vector<PluginHost::ContributedSaveParticipant>* participants,
                             std::vector<runtime_types::SaveParticipantRuntime>* runtimes,
                             std::string* error_message);

bool RegisterCompletion(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCompletion>* contributions,
                        std::vector<runtime_types::CompletionRuntime>* runtimes,
                        std::string* error_message);

bool RegisterCodeAction(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCodeAction>* contributions,
                        std::vector<runtime_types::CodeActionRuntime>* runtimes,
                        std::string* error_message);

bool RegisterTask(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTask>* tasks,
                  std::string* error_message);

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* servers,
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
                          std::vector<PluginHost::ContributedTestProvider>* providers,
                          std::vector<runtime_types::TestProviderRuntime>* runtimes,
                          std::string* error_message);
#endif

}  // namespace microide::plugin::contribution_interop
