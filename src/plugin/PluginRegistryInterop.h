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
                     std::string* error_message);

void RebuildCommandNames(const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
                         std::vector<std::string>* command_names);

bool RegisterSidebar(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     int table_index,
                     std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
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

// A status-item mutation extracted from a Lua table on the worker thread. The
// Lua read (worker-only) is separated from the registry mutation (UI-thread-only)
// so a ctx.status.update issued from a reactive event never touches the shared
// status registries on the worker. Only the fields present in the table are set.
struct StatusItemUpdate {
  std::string full_id;
  bool has_text = false;
  std::string text;
  bool has_tooltip = false;
  std::string tooltip;
  bool has_icon = false;
  std::string icon;
  bool has_tone = false;
  std::string tone;
  bool has_progress = false;
  float progress = -1.0f;
};

// Worker thread: read the update fields from the Lua table at index 2. Touches no
// shared registries. Returns false when no calling plugin can be resolved.
bool ExtractStatusItemUpdate(lua_State* state,
                             const runtime_types::PluginInstance* plugin,
                             std::string_view id,
                             StatusItemUpdate* out);

// UI thread: apply a previously extracted update to the published status-item view.
// Operates on the order vector alone (the UI renders it; the worker-owned map is not
// touched at runtime). `index` is a caller-owned id->position cache kept alongside the
// order vector so a plugin firing frequent ctx.status.update calls resolves the target
// in O(1) instead of rescanning a status registry capped at kMaxPluginStatusItems; it is rebuilt
// lazily whenever the vector's size no longer matches (register/teardown). Returns true
// when the target item existed and was changed.
bool ApplyStatusItemUpdate(
    const StatusItemUpdate& update,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::unordered_map<std::string, std::size_t>* index);
#endif

}  // namespace microide::plugin::registry_interop
