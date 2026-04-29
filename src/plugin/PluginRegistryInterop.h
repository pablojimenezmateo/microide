#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::registry_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool RegisterCommand(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     const PluginHost::Callbacks& callbacks,
                     std::string_view command_name,
                     int function_index,
                     std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
                     std::vector<std::string>* command_names,
                     std::string* error_message);

void RebuildCommandNames(const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
                         std::vector<std::string>* command_names);

bool RegisterSidebar(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     int table_index,
                     std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
                     std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
                     std::string* error_message);

void RebuildSidebarProviders(
    const std::unordered_map<std::string, runtime_types::SidebarProvider>& sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers);

bool RegisterHoverProvider(lua_State* state,
                           const runtime_types::PluginInstance* plugin,
                           int table_index,
                           std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
                           std::vector<std::string>* hover_provider_order,
                           std::string* error_message);

bool RegisterMenuEntry(const runtime_types::PluginInstance* plugin,
                       PluginHost::ContributedMenuEntry contributed,
                       std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
                       std::string* error_message);

bool RegisterKeybinding(const runtime_types::PluginInstance* plugin,
                        PluginHost::ContributedKeybinding contributed,
                        std::vector<PluginHost::ContributedKeybinding>* keybindings,
                        std::string* error_message);

bool RegisterSetting(const runtime_types::PluginInstance* plugin,
                     PluginHost::ContributedSettingSpec contributed,
                     std::vector<PluginHost::ContributedSettingSpec>* settings,
                     std::string* error_message);

bool RegisterStatusItem(const runtime_types::PluginInstance* plugin,
                        PluginHost::ContributedStatusItem contributed,
                        std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
                        std::vector<PluginHost::ContributedStatusItem>* status_item_order,
                        std::string* error_message);

bool UpdateStatusItem(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      std::string_view id,
                      std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
                      std::vector<PluginHost::ContributedStatusItem>* status_item_order);
#endif

}  // namespace microide::plugin::registry_interop
