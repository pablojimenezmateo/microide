#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_provider_registration_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool RegisterCommand(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const PluginHost::Callbacks& callbacks,
    const char* name,
    int function_index,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::string* error_message);
bool RegisterSidebar(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    int descriptor_index,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::string* error_message);
bool RegisterHoverProvider(lua_State* state,
                           const runtime_types::PluginInstance* plugin,
                           int descriptor_index,
                           std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
                           std::vector<std::string>* hover_provider_order,
                           std::string* error_message);
bool RegisterMenuEntry(lua_State* state,
                       const runtime_types::PluginInstance* plugin,
                       std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
                       std::string* error_message);
bool RegisterKeybinding(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        std::vector<PluginHost::ContributedKeybinding>* keybindings,
                        std::string* error_message);
bool RegisterSetting(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     std::vector<PluginHost::ContributedSettingSpec>* settings,
                     std::string* error_message);
bool RegisterStatusItem(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::string* error_message);
void UpdateStatusItem(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      const char* id,
                      std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
                      std::vector<PluginHost::ContributedStatusItem>* status_item_order,
                      const std::function<void()>& request_status_redraw);

bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message);
bool RegisterSaveParticipant(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedSaveParticipant>* save_participants,
    std::vector<runtime_types::SaveParticipantRuntime>* save_participant_runtimes,
    std::string* error_message);
bool RegisterCompletion(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCompletion>* completions,
                        std::vector<runtime_types::CompletionRuntime>* completion_runtimes,
                        std::string* error_message);
bool RegisterCodeAction(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCodeAction>* code_actions,
                        std::vector<runtime_types::CodeActionRuntime>* code_action_runtimes,
                        std::string* error_message);

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

}  // namespace microide::plugin::lua_provider_registration_interop
