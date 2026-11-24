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

// Every contribution list the host keeps, for a teardown that drops one plugin's
// (or one Lua state's) entries from all of them. All are required.
struct ContributionLists {
  std::unordered_map<std::string, runtime_types::PluginCommand>* commands = nullptr;
  std::vector<std::string>* command_names = nullptr;
  std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars = nullptr;
  std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers = nullptr;
  std::unordered_map<std::string, runtime_types::HoverProvider>* hovers = nullptr;
  std::vector<std::string>* hover_provider_order = nullptr;
  std::vector<PluginHost::ContributedMenuEntry>* menu_entries = nullptr;
  std::vector<PluginHost::ContributedKeybinding>* keybindings = nullptr;
  std::vector<PluginHost::ContributedSettingSpec>* settings = nullptr;
  std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items = nullptr;
  std::vector<PluginHost::ContributedStatusItem>* status_item_order = nullptr;
  std::vector<PluginHost::ContributedFormatter>* formatters = nullptr;
  std::vector<PluginHost::ContributedSaveParticipant>* save_participants = nullptr;
  std::vector<PluginHost::ContributedVirtualDocument>* virtual_documents = nullptr;
  std::vector<runtime_types::SaveParticipantRuntime>* save_participant_runtimes = nullptr;
  std::vector<PluginHost::ContributedCompletion>* completions = nullptr;
  std::vector<runtime_types::CompletionRuntime>* completion_runtimes = nullptr;
  std::vector<PluginHost::ContributedCodeAction>* code_actions = nullptr;
  std::vector<runtime_types::CodeActionRuntime>* code_action_runtimes = nullptr;
  std::vector<runtime_types::LanguageQueryRuntime>* language_query_runtimes = nullptr;
  std::vector<PluginHost::ContributedLanguageServer>* language_servers = nullptr;
  std::vector<PluginHost::ContributedDebugAdapter>* debug_adapters = nullptr;
  std::vector<PluginHost::ContributedLaunchConfig>* launch_configs = nullptr;
  std::vector<PluginHost::ContributedTask>* tasks = nullptr;
  std::vector<PluginHost::ContributedTool>* tools = nullptr;
  std::vector<PluginHost::ContributedTestProvider>* test_providers = nullptr;
  std::vector<runtime_types::TestProviderRuntime>* test_provider_runtimes = nullptr;
  std::vector<PluginHost::ContributedScmProvider>* scm_providers = nullptr;
  std::vector<runtime_types::ScmProviderRuntime>* scm_provider_runtimes = nullptr;
  std::vector<PluginHost::ContributedAnnotationProvider>* annotation_providers = nullptr;
  std::vector<runtime_types::AnnotationProviderRuntime>* annotation_provider_runtimes = nullptr;
  std::vector<PluginHost::ContributedAuthProvider>* auth_providers = nullptr;
  std::vector<runtime_types::AuthProviderRuntime>* auth_provider_runtimes = nullptr;
  std::vector<PluginHost::ContributedBracketSet>* bracket_sets = nullptr;
  std::vector<PluginHost::ContributedCommentMarkers>* comment_markers = nullptr;
  std::vector<PluginHost::ContributedIndentRules>* indent_rules = nullptr;
  std::vector<PluginHost::ContributedSnippet>* snippets = nullptr;
  std::vector<PluginHost::ContributedTheme>* themes = nullptr;
  std::vector<PluginHost::ContributedFileIconTheme>* file_icon_themes = nullptr;
};

void UnregisterContributionsForState(lua_State* state, const runtime_types::PluginInstance* plugin,
                                     const ContributionLists& lists);
#endif

}  // namespace microide::plugin::state_teardown_interop
