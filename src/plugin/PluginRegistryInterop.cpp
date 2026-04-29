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

}  // namespace microide::plugin::registry_interop

#endif
