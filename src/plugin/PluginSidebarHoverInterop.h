#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::sidebar_hover_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool SnapshotSidebarProvider(
    const runtime_types::SidebarProvider& provider,
    const std::filesystem::path& current_project_root,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::vector<PluginHost::SidebarItem>* items,
    std::string* error_message);

bool ConfirmSidebarProviderItem(
    const runtime_types::SidebarProvider& provider,
    const PluginHost::SidebarItem& item,
    const std::filesystem::path& current_project_root,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<bool(const PluginHost::OpenFileRequest&)>& open_file,
    std::string* error_message);

bool QueryHoverProvider(
    const runtime_types::HoverProvider& provider,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_table,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::HoverResult* result,
    std::string* error_message);
#endif

}  // namespace microide::plugin::sidebar_hover_interop
