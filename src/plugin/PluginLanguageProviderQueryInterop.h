#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

// Query side of the four plugin-native language providers (go-to-definition,
// find references, signature help, document symbols). Mirrors
// PluginProviderQueryInterop (completion / code action) but lives in its own TU
// to keep both under the 800-line plugin cap. Every plugin-supplied table is
// bounded before allocation (see the kMax* limits in the .cpp).
namespace microide::plugin::language_provider_query_interop {

#if MICROIDE_HAS_LUA_PLUGINS
// Handles both Definition and References (the only difference is References
// forwards `include_declaration` as a third Lua argument). Returned paths are
// resolved against `current_project_root`.
std::vector<PluginHost::LocationResult> QueryLocations(
    runtime_types::LanguageQueryKind kind,
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    bool include_declaration,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    std::string* error_message);

bool QuerySignatureHelp(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    PluginHost::SignatureHelpResult* result,
    std::string* error_message);

std::vector<PluginHost::DocumentSymbolNode> QueryDocumentSymbols(
    std::string_view language_id,
    const std::filesystem::path& path,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message);
#endif

}  // namespace microide::plugin::language_provider_query_interop
