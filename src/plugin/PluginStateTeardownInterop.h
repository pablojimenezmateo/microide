#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::state_teardown_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void ClearPluginDiagnostics(
    const runtime_types::PluginInstance* plugin,
    const std::function<void(std::string_view)>& clear_owner_diagnostics);

void DestroyPluginState(runtime_types::PluginInstance* plugin);

void UnregisterContributionsForState(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
    std::vector<PluginHost::ContributedKeybinding>* keybindings,
    std::vector<PluginHost::ContributedSettingSpec>* settings,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::vector<PluginHost::ContributedFormatter>* formatters,
    std::vector<PluginHost::ContributedSaveParticipant>* save_participants,
    std::vector<runtime_types::SaveParticipantRuntime>* save_participant_runtimes,
    std::vector<PluginHost::ContributedCompletion>* completions,
    std::vector<runtime_types::CompletionRuntime>* completion_runtimes,
    std::vector<PluginHost::ContributedCodeAction>* code_actions,
    std::vector<runtime_types::CodeActionRuntime>* code_action_runtimes,
    std::vector<PluginHost::ContributedLanguageServer>* language_servers,
    std::vector<PluginHost::ContributedDebugAdapter>* debug_adapters,
    std::vector<PluginHost::ContributedLaunchConfig>* launch_configs,
    std::vector<PluginHost::ContributedTask>* tasks,
    std::vector<PluginHost::ContributedTool>* tools,
    std::vector<PluginHost::ContributedTestProvider>* test_providers,
    std::vector<runtime_types::TestProviderRuntime>* test_provider_runtimes,
    std::vector<PluginHost::ContributedScmProvider>* scm_providers,
    std::vector<runtime_types::ScmProviderRuntime>* scm_provider_runtimes,
    std::vector<PluginHost::ContributedAnnotationProvider>* annotation_providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* annotation_provider_runtimes,
    std::vector<PluginHost::ContributedAuthProvider>* auth_providers,
    std::vector<runtime_types::AuthProviderRuntime>* auth_provider_runtimes,
    std::vector<PluginHost::ContributedBracketSet>* bracket_sets,
    std::vector<PluginHost::ContributedCommentMarkers>* comment_markers,
    std::vector<PluginHost::ContributedIndentRules>* indent_rules,
    std::vector<PluginHost::ContributedSnippet>* snippets);
#endif

}  // namespace microide::plugin::state_teardown_interop
