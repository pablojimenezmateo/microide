#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::provider_query_interop {

#if MICROIDE_HAS_LUA_PLUGINS
std::vector<PluginHost::CompletionCandidate> QueryCompletions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    std::string_view trigger_character,
    const std::vector<runtime_types::CompletionRuntime>& completion_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message);

std::vector<PluginHost::CodeActionCandidate> QueryCodeActions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t start_column,
    std::size_t end_line,
    std::size_t end_column,
    const std::vector<runtime_types::CodeActionRuntime>& code_action_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message);

bool DiscoverTests(
    std::string_view provider_id,
    const std::filesystem::path& path,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::TestProviderRuntime>& test_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>&
        resolve_runtime_path,
    std::vector<PluginHost::TestCase>* tests,
    std::string* error_message);

bool RunTests(
    std::string_view provider_id,
    const std::vector<std::string>& test_ids,
    const std::vector<runtime_types::TestProviderRuntime>& test_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::vector<PluginHost::TestRunResult>* results,
    std::string* error_message);

bool SnapshotScm(
    std::string_view provider_id,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::ScmProviderRuntime>& scm_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>&
        resolve_runtime_path,
    PluginHost::ScmSnapshot* snapshot,
    std::string* error_message);

std::vector<PluginHost::AnnotationLine> QueryAnnotations(
    std::string_view provider_id,
    const std::filesystem::path& path,
    std::string_view language_id,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    const std::vector<runtime_types::AnnotationProviderRuntime>& annotation_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message);

bool LoginAuthProvider(
    std::string_view provider_id,
    const std::vector<std::string>& scopes,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::AuthSessionData* session,
    std::string* error_message);

bool RefreshAuthSession(
    std::string_view provider_id,
    std::string_view session_id,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::AuthSessionData* session,
    std::string* error_message);

bool LogoutAuthSession(
    std::string_view provider_id,
    std::string_view session_id,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::string* error_message);

bool InvokeMcpTool(
    std::string_view tool_id,
    std::string_view input_json,
    const std::vector<runtime_types::McpToolRuntime>& mcp_tool_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::string* output_json,
    std::string* error_message);
#endif

}  // namespace microide::plugin::provider_query_interop
