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

bool RegisterLanguageQuery(lua_State* state,
                           std::string_view plugin_id,
                           runtime_types::LanguageQueryKind kind,
                           std::vector<runtime_types::LanguageQueryRuntime>* runtimes,
                           std::string* error_message);

bool RegisterTask(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTask>* tasks,
                  std::string* error_message);

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* servers,
                            std::string* error_message);

bool RegisterDebugAdapter(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedDebugAdapter>* adapters,
                          std::string* error_message);

bool RegisterLaunchConfig(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedLaunchConfig>* configs,
                          std::string* error_message);

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::string* error_message);

bool RegisterTestProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedTestProvider>* providers,
                          std::vector<runtime_types::TestProviderRuntime>* runtimes,
                          std::string* error_message);

bool RegisterScmProvider(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedScmProvider>* providers,
                         std::vector<runtime_types::ScmProviderRuntime>* runtimes,
                         std::string* error_message);

bool RegisterAnnotationProvider(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedAnnotationProvider>* providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* runtimes,
    std::string* error_message);

bool RegisterAuthProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedAuthProvider>* providers,
                          std::vector<runtime_types::AuthProviderRuntime>* runtimes,
                          std::string* error_message);

bool RegisterAiProvider(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedAiProvider>* providers,
                        std::string* error_message);

bool RegisterExternalAgent(lua_State* state,
                           std::string_view plugin_id,
                           std::vector<PluginHost::ContributedExternalAgent>* agents,
                           std::string* error_message);

bool RegisterBracketSet(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedBracketSet>* sets,
                        std::string* error_message);
bool RegisterCommentMarkers(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedCommentMarkers>* markers,
                            std::string* error_message);
bool RegisterIndentRules(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedIndentRules>* rules,
                         std::string* error_message);
bool RegisterSnippet(lua_State* state,
                     std::string_view plugin_id,
                     std::vector<PluginHost::ContributedSnippet>* snippets,
                     std::string* error_message);
#endif

}  // namespace microide::plugin::contribution_interop
