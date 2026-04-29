#include "plugin/PluginLuaProviderRegistrationInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginContributionInterop.h"
#include "plugin/PluginRegistrationParsers.h"
#include "plugin/PluginRegistryInterop.h"

namespace microide::plugin::lua_provider_registration_interop {

bool RegisterCommand(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const PluginHost::Callbacks& callbacks,
    const char* name,
    int function_index,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::string* error_message) {
  return registry_interop::RegisterCommand(state, plugin, callbacks, name, function_index,
                                           commands, command_names, error_message);
}

bool RegisterSidebar(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    int descriptor_index,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::string* error_message) {
  return registry_interop::RegisterSidebar(state, plugin, descriptor_index, sidebars,
                                           sidebar_providers, error_message);
}

bool RegisterHoverProvider(lua_State* state,
                           const runtime_types::PluginInstance* plugin,
                           int descriptor_index,
                           std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
                           std::vector<std::string>* hover_provider_order,
                           std::string* error_message) {
  return registry_interop::RegisterHoverProvider(state, plugin, descriptor_index, hovers,
                                                 hover_provider_order, error_message);
}

bool RegisterMenuEntry(lua_State* state,
                       const runtime_types::PluginInstance* plugin,
                       std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
                       std::string* error_message) {
  registration_parsers::MenuEntryRegistration registration;
  if (!registration_parsers::ParseMenuEntryRegistration(
          state, plugin != nullptr ? plugin->id : std::string{}, &registration, error_message)) {
    return false;
  }
  return registry_interop::RegisterMenuEntry(plugin, std::move(registration.contributed),
                                             menu_entries, error_message);
}

bool RegisterKeybinding(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        std::vector<PluginHost::ContributedKeybinding>* keybindings,
                        std::string* error_message) {
  registration_parsers::KeybindingRegistration registration;
  if (!registration_parsers::ParseKeybindingRegistration(
          state, plugin != nullptr ? plugin->id : std::string{}, &registration, error_message)) {
    return false;
  }
  return registry_interop::RegisterKeybinding(plugin, std::move(registration.contributed),
                                              keybindings, error_message);
}

bool RegisterSetting(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     std::vector<PluginHost::ContributedSettingSpec>* settings,
                     std::string* error_message) {
  registration_parsers::SettingRegistration registration;
  if (!registration_parsers::ParseSettingRegistration(
          state, plugin != nullptr ? plugin->id : std::string{}, &registration, error_message)) {
    return false;
  }
  return registry_interop::RegisterSetting(plugin, std::move(registration.contributed), settings,
                                           error_message);
}

bool RegisterStatusItem(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::string* error_message) {
  registration_parsers::StatusItemRegistration registration;
  if (!registration_parsers::ParseStatusItemRegistration(
          state, plugin != nullptr ? plugin->id : std::string{}, &registration, error_message)) {
    return false;
  }
  return registry_interop::RegisterStatusItem(plugin, std::move(registration.contributed),
                                              status_items, status_item_order, error_message);
}

void UpdateStatusItem(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      const char* id,
                      std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
                      std::vector<PluginHost::ContributedStatusItem>* status_item_order,
                      const std::function<void()>& request_status_redraw) {
  if (registry_interop::UpdateStatusItem(state, plugin, id, status_items, status_item_order) &&
      request_status_redraw) {
    request_status_redraw();
  }
}

bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message) {
  return contribution_interop::RegisterFormatter(state, std::string(plugin_id), formatters,
                                                 error_message);
}

bool RegisterSaveParticipant(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedSaveParticipant>* save_participants,
    std::vector<runtime_types::SaveParticipantRuntime>* save_participant_runtimes,
    std::string* error_message) {
  return contribution_interop::RegisterSaveParticipant(state, std::string(plugin_id),
                                                       save_participants, save_participant_runtimes,
                                                       error_message);
}

bool RegisterCompletion(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCompletion>* completions,
                        std::vector<runtime_types::CompletionRuntime>* completion_runtimes,
                        std::string* error_message) {
  return contribution_interop::RegisterCompletion(state, std::string(plugin_id), completions,
                                                  completion_runtimes, error_message);
}

bool RegisterCodeAction(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCodeAction>* code_actions,
                        std::vector<runtime_types::CodeActionRuntime>* code_action_runtimes,
                        std::string* error_message) {
  return contribution_interop::RegisterCodeAction(state, std::string(plugin_id), code_actions,
                                                  code_action_runtimes, error_message);
}

bool RegisterTask(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTask>* tasks,
                  std::string* error_message) {
  return contribution_interop::RegisterTask(state, std::string(plugin_id), tasks, error_message);
}

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* language_servers,
                            std::string* error_message) {
  return contribution_interop::RegisterLanguageServer(state, std::string(plugin_id),
                                                      language_servers, error_message);
}

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::string* error_message) {
  return contribution_interop::RegisterTool(state, std::string(plugin_id), tools, error_message);
}

bool RegisterDebugger(lua_State* state,
                      std::string_view plugin_id,
                      std::vector<PluginHost::ContributedDebugger>* debuggers,
                      std::string* error_message) {
  return contribution_interop::RegisterDebugger(state, std::string(plugin_id), debuggers,
                                                error_message);
}

bool RegisterTestProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedTestProvider>* test_providers,
                          std::vector<runtime_types::TestProviderRuntime>* test_provider_runtimes,
                          std::string* error_message) {
  return contribution_interop::RegisterTestProvider(state, std::string(plugin_id), test_providers,
                                                    test_provider_runtimes, error_message);
}

bool RegisterScmProvider(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedScmProvider>* scm_providers,
                         std::vector<runtime_types::ScmProviderRuntime>* scm_provider_runtimes,
                         std::string* error_message) {
  if (lua_istable(state, 1)) {
    return contribution_interop::RegisterScmProvider(state, std::string(plugin_id), scm_providers,
                                                     scm_provider_runtimes, error_message);
  }
  scm_providers->push_back(PluginHost::ContributedScmProvider{
      .id = std::string(plugin_id) + "." + std::string(luaL_checkstring(state, 1)),
      .label = std::string(luaL_checkstring(state, 2)),
      .plugin_id = std::string(plugin_id),
  });
  return true;
}

bool RegisterAnnotationProvider(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedAnnotationProvider>* annotation_providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* annotation_provider_runtimes,
    std::string* error_message) {
  return contribution_interop::RegisterAnnotationProvider(
      state, std::string(plugin_id), annotation_providers, annotation_provider_runtimes,
      error_message);
}

bool RegisterAuthProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedAuthProvider>* auth_providers,
                          std::vector<runtime_types::AuthProviderRuntime>* auth_provider_runtimes,
                          std::string* error_message) {
  if (lua_istable(state, 1)) {
    return contribution_interop::RegisterAuthProvider(state, std::string(plugin_id), auth_providers,
                                                      auth_provider_runtimes, error_message);
  }
  auth_providers->push_back(PluginHost::ContributedAuthProvider{
      .id = std::string(plugin_id) + "." + std::string(luaL_checkstring(state, 1)),
      .label = std::string(luaL_checkstring(state, 2)),
      .plugin_id = std::string(plugin_id),
  });
  return true;
}

bool RegisterAiProvider(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedAiProvider>* ai_providers,
                        std::string* error_message) {
  return contribution_interop::RegisterAiProvider(state, std::string(plugin_id), ai_providers,
                                                  error_message);
}

bool RegisterExternalAgent(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedExternalAgent>* external_agents,
    std::string* error_message) {
  return contribution_interop::RegisterExternalAgent(state, std::string(plugin_id), external_agents,
                                                     error_message);
}

bool RegisterMcpTool(lua_State* state,
                     std::string_view plugin_id,
                     std::vector<PluginHost::ContributedMcpTool>* mcp_tools,
                     std::vector<runtime_types::McpToolRuntime>* mcp_tool_runtimes,
                     std::string* error_message) {
  return contribution_interop::RegisterMcpTool(state, std::string(plugin_id), mcp_tools,
                                               mcp_tool_runtimes, error_message);
}

}  // namespace microide::plugin::lua_provider_registration_interop

#endif
