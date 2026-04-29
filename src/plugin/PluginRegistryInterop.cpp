#include "plugin/PluginRegistryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <cctype>

namespace microide::plugin::registry_interop {
namespace {

bool IsValidIdentifier(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
  });
}

void RebuildCommandNames(const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
                         std::vector<std::string>* command_names) {
  if (command_names == nullptr) {
    return;
  }
  command_names->clear();
  command_names->reserve(commands.size());
  for (const auto& entry : commands) {
    command_names->push_back(entry.first);
  }
  std::sort(command_names->begin(), command_names->end());
}

void RebuildSidebarProviders(
    const std::unordered_map<std::string, runtime_types::SidebarProvider>& sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers) {
  if (sidebar_providers == nullptr) {
    return;
  }
  sidebar_providers->clear();
  sidebar_providers->reserve(sidebars.size());
  for (const auto& entry : sidebars) {
    sidebar_providers->push_back(entry.second.info);
  }
  std::sort(sidebar_providers->begin(), sidebar_providers->end(),
            [](const PluginHost::SidebarProviderInfo& a, const PluginHost::SidebarProviderInfo& b) {
              return a.id < b.id;
            });
}

}  // namespace

bool RegisterCommand(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     const PluginHost::Callbacks& callbacks,
                     std::string_view command_name,
                     int function_index,
                     std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
                     std::vector<std::string>* command_names,
                     std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin command registration requires an active plugin state";
    }
    return false;
  }
  if (commands == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin command registry is unavailable";
    }
    return false;
  }
  if (!IsValidIdentifier(command_name)) {
    if (error_message != nullptr) {
      *error_message = "invalid command name: " + std::string(command_name);
    }
    return false;
  }
  if (callbacks.is_command_name_available && !callbacks.is_command_name_available(command_name)) {
    if (error_message != nullptr) {
      *error_message = "command name already used by the host: " + std::string(command_name);
    }
    return false;
  }
  if (commands->contains(std::string(command_name))) {
    if (error_message != nullptr) {
      *error_message = "duplicate plugin command: " + std::string(command_name);
    }
    return false;
  }
  lua_pushvalue(state, function_index);
  const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  commands->emplace(std::string(command_name),
                    runtime_types::PluginCommand{
                        .plugin_id = plugin->id,
                        .state = state,
                        .function_ref = function_ref,
                    });
  RebuildCommandNames(*commands, command_names);
  return true;
}

bool RegisterSidebar(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     int table_index,
                     std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
                     std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
                     std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin sidebar registration requires an active plugin state";
    }
    return false;
  }
  if (sidebars == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin sidebar registry is unavailable";
    }
    return false;
  }
  lua_getfield(state, table_index, "id");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar id must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  const std::string id = lua_tostring(state, -1);
  lua_pop(state, 1);
  if (!IsValidIdentifier(id)) {
    if (error_message != nullptr) {
      *error_message = "invalid sidebar id: " + id;
    }
    return false;
  }
  if (sidebars->contains(id)) {
    if (error_message != nullptr) {
      *error_message = "duplicate plugin sidebar: " + id;
    }
    return false;
  }

  lua_getfield(state, table_index, "label");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar label must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  const std::string label = lua_tostring(state, -1);
  lua_pop(state, 1);

  lua_getfield(state, table_index, "snapshot");
  if (!lua_isfunction(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar snapshot must be a function";
    }
    lua_pop(state, 1);
    return false;
  }
  const int snapshot_ref = luaL_ref(state, LUA_REGISTRYINDEX);

  int confirm_ref = LUA_NOREF;
  lua_getfield(state, table_index, "on_confirm");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
  } else if (lua_isfunction(state, -1)) {
    confirm_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  } else {
    if (error_message != nullptr) {
      *error_message = "sidebar on_confirm must be a function";
    }
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshot_ref);
    return false;
  }

  sidebars->emplace(id, runtime_types::SidebarProvider{
                            .info =
                                PluginHost::SidebarProviderInfo{
                                    .id = id,
                                    .label = label,
                                    .plugin_id = plugin->id,
                                },
                            .state = state,
                            .snapshot_ref = snapshot_ref,
                            .confirm_ref = confirm_ref,
                        });
  RebuildSidebarProviders(*sidebars, sidebar_providers);
  return true;
}

bool RegisterHoverProvider(lua_State* state,
                           const runtime_types::PluginInstance* plugin,
                           int table_index,
                           std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
                           std::vector<std::string>* hover_provider_order,
                           std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "hover provider registration requires an active plugin";
    }
    return false;
  }
  if (hovers == nullptr || hover_provider_order == nullptr) {
    if (error_message != nullptr) {
      *error_message = "hover provider registry is unavailable";
    }
    return false;
  }
  const int absolute_index = lua_absindex(state, table_index);
  lua_getfield(state, absolute_index, "id");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "hover provider id must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  const std::string id = lua_tostring(state, -1);
  lua_pop(state, 1);
  if (!IsValidIdentifier(id)) {
    if (error_message != nullptr) {
      *error_message = "invalid hover provider id: " + id;
    }
    return false;
  }
  if (hovers->contains(id)) {
    if (error_message != nullptr) {
      *error_message = "duplicate hover provider: " + id;
    }
    return false;
  }

  lua_getfield(state, absolute_index, "provide");
  if (!lua_isfunction(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "hover provider provide must be a function";
    }
    lua_pop(state, 1);
    return false;
  }
  const int provide_ref = luaL_ref(state, LUA_REGISTRYINDEX);

  hovers->emplace(id, runtime_types::HoverProvider{
                          .id = id,
                          .plugin_id = plugin->id,
                          .state = state,
                          .provide_ref = provide_ref,
                      });
  hover_provider_order->push_back(id);
  return true;
}

bool RegisterMenuEntry(const runtime_types::PluginInstance* plugin,
                       PluginHost::ContributedMenuEntry contributed,
                       std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
                       std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "menu contribution requires an active plugin state";
    }
    return false;
  }
  if (menu_entries == nullptr) {
    if (error_message != nullptr) {
      *error_message = "menu registry is unavailable";
    }
    return false;
  }
  if (!IsValidIdentifier(contributed.id.substr(plugin->id.size() + 1))) {
    if (error_message != nullptr) {
      *error_message = "invalid menu entry id: " + contributed.id;
    }
    return false;
  }
  for (const auto& existing : *menu_entries) {
    if (existing.id == contributed.id) {
      if (error_message != nullptr) {
        *error_message = "duplicate menu entry: " + contributed.id;
      }
      return false;
    }
  }
  menu_entries->push_back(std::move(contributed));
  return true;
}

bool RegisterKeybinding(const runtime_types::PluginInstance* plugin,
                        PluginHost::ContributedKeybinding contributed,
                        std::vector<PluginHost::ContributedKeybinding>* keybindings,
                        std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "keybinding registration requires an active plugin state";
    }
    return false;
  }
  if (keybindings == nullptr) {
    if (error_message != nullptr) {
      *error_message = "keybinding registry is unavailable";
    }
    return false;
  }
  if (!IsValidIdentifier(contributed.id.substr(plugin->id.size() + 1))) {
    if (error_message != nullptr) {
      *error_message = "invalid keybinding id: " + contributed.id;
    }
    return false;
  }
  for (const auto& existing : *keybindings) {
    if (existing.id == contributed.id) {
      if (error_message != nullptr) {
        *error_message = "duplicate keybinding: " + contributed.id;
      }
      return false;
    }
  }
  keybindings->push_back(std::move(contributed));
  return true;
}

bool RegisterSetting(const runtime_types::PluginInstance* plugin,
                     PluginHost::ContributedSettingSpec contributed,
                     std::vector<PluginHost::ContributedSettingSpec>* settings,
                     std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "setting declaration requires an active plugin state";
    }
    return false;
  }
  if (settings == nullptr) {
    if (error_message != nullptr) {
      *error_message = "setting registry is unavailable";
    }
    return false;
  }
  static const char* const kValidTypes[] = {"bool", "int", "float", "string", "enum"};
  bool type_valid = false;
  for (const char* t : kValidTypes) {
    if (contributed.type == t) {
      type_valid = true;
      break;
    }
  }
  if (!type_valid) {
    if (error_message != nullptr) {
      *error_message = "setting type must be one of: bool, int, float, string, enum";
    }
    return false;
  }
  if (!IsValidIdentifier(contributed.id.substr(plugin->id.size() + 1))) {
    if (error_message != nullptr) {
      *error_message = "invalid setting id: " + contributed.id;
    }
    return false;
  }
  for (const auto& existing : *settings) {
    if (existing.id == contributed.id) {
      if (error_message != nullptr) {
        *error_message = "duplicate setting: " + contributed.id;
      }
      return false;
    }
  }
  settings->push_back(std::move(contributed));
  return true;
}

bool RegisterStatusItem(
    const runtime_types::PluginInstance* plugin,
    PluginHost::ContributedStatusItem contributed,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "status item registration requires an active plugin state";
    }
    return false;
  }
  if (status_items == nullptr || status_item_order == nullptr) {
    if (error_message != nullptr) {
      *error_message = "status item registry is unavailable";
    }
    return false;
  }
  if (!IsValidIdentifier(contributed.id.substr(plugin->id.size() + 1))) {
    if (error_message != nullptr) {
      *error_message = "invalid status item id: " + contributed.id;
    }
    return false;
  }
  if (status_items->contains(contributed.id)) {
    if (error_message != nullptr) {
      *error_message = "duplicate status item: " + contributed.id;
    }
    return false;
  }
  status_item_order->push_back(contributed);
  status_items->emplace(contributed.id, std::move(contributed));
  return true;
}

bool UpdateStatusItem(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      std::string_view id,
                      std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
                      std::vector<PluginHost::ContributedStatusItem>* status_item_order) {
  if (plugin == nullptr || status_items == nullptr || status_item_order == nullptr) {
    return false;
  }
  const std::string full_id = plugin->id + "." + std::string(id);
  auto it = status_items->find(full_id);
  if (it == status_items->end()) {
    return false;
  }
  lua_getfield(state, 2, "text");
  if (lua_isstring(state, -1)) {
    it->second.text = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  lua_getfield(state, 2, "tooltip");
  if (lua_isstring(state, -1)) {
    it->second.tooltip = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  for (auto& order_item : *status_item_order) {
    if (order_item.id == full_id) {
      order_item.text = it->second.text;
      order_item.tooltip = it->second.tooltip;
      break;
    }
  }
  return true;
}

}  // namespace microide::plugin::registry_interop

#endif
