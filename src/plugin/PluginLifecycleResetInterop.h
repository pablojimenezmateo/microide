#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::lifecycle_reset_interop {

void ResetForDisabledRuntime(
    const std::filesystem::path& project_root,
    std::filesystem::path* current_project_root,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<runtime_types::PluginInstance>* plugins);

void ShutdownForDisabledRuntime(
    std::filesystem::path* current_project_root,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<runtime_types::PluginInstance>* plugins);

}  // namespace microide::plugin::lifecycle_reset_interop
