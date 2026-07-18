#include "plugin/PluginRegistryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <cmath>

#include "plugin/PluginContributionLimits.h"
#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::registry_interop {
namespace {

using lua_interop::IsValidIdentifier;

// The per-kind contribution ceiling (kMaxPluginContributionsPerKind) and
// ContributionLimitReached now live in plugin/PluginContributionLimits.h so the
// parallel contribution_interop register path enforces the identical envelope.

void RebuildCommandNamesImpl(
    const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
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

void RebuildSidebarProvidersImpl(
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

void RebuildCommandNames(const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
                         std::vector<std::string>* command_names) {
  RebuildCommandNamesImpl(commands, command_names);
}

void RebuildSidebarProviders(
    const std::unordered_map<std::string, runtime_types::SidebarProvider>& sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers) {
  RebuildSidebarProvidersImpl(sidebars, sidebar_providers);
}

bool RegisterCommand(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     const PluginHost::Callbacks& callbacks,
                     std::string_view command_name,
                     int function_index,
                     std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
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
  if (ContributionLimitReached(commands, error_message)) {
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
  return true;
}

bool RegisterSidebar(lua_State* state,
                     const runtime_types::PluginInstance* plugin,
                     int table_index,
                     std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
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
  if (ContributionLimitReached(sidebars, error_message)) {
    return false;
  }
  lua_interop::GetFieldProtected(state, table_index, "id");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar id must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  // NUL-reject: a truncated id would pass IsValidIdentifier on its prefix and could
  // collide with another sidebar (TD-2026-07-17A-080).
  const std::string id = lua_interop::ToHostString(state, -1).value_or(std::string{});
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

  lua_interop::GetFieldProtected(state, table_index, "label");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar label must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  const std::string label = lua_tostring(state, -1);
  lua_pop(state, 1);

  lua_interop::GetFieldProtected(state, table_index, "snapshot");
  if (!lua_isfunction(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "sidebar snapshot must be a function";
    }
    lua_pop(state, 1);
    return false;
  }
  const int snapshot_ref = luaL_ref(state, LUA_REGISTRYINDEX);

  int confirm_ref = LUA_NOREF;
  lua_interop::GetFieldProtected(state, table_index, "on_confirm");
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

  int toggle_ref = LUA_NOREF;
  lua_interop::GetFieldProtected(state, table_index, "on_toggle");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
  } else if (lua_isfunction(state, -1)) {
    toggle_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  } else {
    if (error_message != nullptr) {
      *error_message = "sidebar on_toggle must be a function";
    }
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshot_ref);
    if (confirm_ref != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, confirm_ref);
    }
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
                            .toggle_ref = toggle_ref,
                        });
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
  if (ContributionLimitReached(hovers, error_message)) {
    return false;
  }
  const int absolute_index = lua_absindex(state, table_index);
  lua_interop::GetFieldProtected(state, absolute_index, "id");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "hover provider id must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  // NUL-reject the hover provider id (TD-2026-07-17A-080); see sidebar id above.
  const std::string id = lua_interop::ToHostString(state, -1).value_or(std::string{});
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

  lua_interop::GetFieldProtected(state, absolute_index, "provide");
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
  if (ContributionLimitReached(menu_entries, error_message)) {
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
  if (ContributionLimitReached(keybindings, error_message)) {
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
  if (ContributionLimitReached(settings, error_message)) {
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
  if (ContributionLimitReached(status_items, error_message)) {
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

bool ExtractStatusItemUpdate(lua_State* state,
                             const runtime_types::PluginInstance* plugin,
                             std::string_view id,
                             StatusItemUpdate* out) {
  if (plugin == nullptr || out == nullptr) {
    return false;
  }
  out->full_id = plugin->id + "." + std::string(id);
  lua_interop::GetFieldProtected(state, 2, "text");
  if (lua_isstring(state, -1)) {
    out->has_text = true;
    out->text = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  lua_interop::GetFieldProtected(state, 2, "tooltip");
  if (lua_isstring(state, -1)) {
    out->has_tooltip = true;
    out->tooltip = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  lua_interop::GetFieldProtected(state, 2, "icon");
  if (lua_isstring(state, -1)) {
    out->has_icon = true;
    out->icon = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  lua_interop::GetFieldProtected(state, 2, "tone");
  if (lua_isstring(state, -1)) {
    out->has_tone = true;
    out->tone = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  lua_interop::GetFieldProtected(state, 2, "progress");
  if (lua_isnumber(state, -1)) {
    const double value = lua_tonumber(state, -1);
    // Ignore non-finite progress (NaN slips through std::clamp) so it cannot
    // reach the status-bar progress-bar layout; treat it as "no bar".
    if (std::isfinite(value)) {
      out->has_progress = true;
      out->progress = value < 0.0 ? -1.0f : std::clamp(static_cast<float>(value), 0.0f, 1.0f);
    }
  }
  lua_pop(state, 1);
  return true;
}

bool ApplyStatusItemUpdate(
    const StatusItemUpdate& update,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::unordered_map<std::string, std::size_t>* index) {
  if (status_item_order == nullptr || index == nullptr) {
    return false;
  }
  const auto rebuild_index = [&] {
    index->clear();
    index->reserve(status_item_order->size());
    for (std::size_t i = 0; i < status_item_order->size(); ++i) {
      index->emplace((*status_item_order)[i].id, i);
    }
  };
  // Ids are unique, so index->size() tracks the vector's size 1:1. A structural
  // change (register append / teardown erase) always changes the vector size;
  // in-place field updates below keep positions valid. Rebuild only on a size
  // mismatch so steady-state updates stay O(1).
  if (index->size() != status_item_order->size()) {
    rebuild_index();
  }
  auto it = index->find(update.full_id);
  if (it == index->end() || it->second >= status_item_order->size() ||
      (*status_item_order)[it->second].id != update.full_id) {
    // Defend against a same-size structural swap: rebuild once and retry before
    // concluding the item is absent.
    rebuild_index();
    it = index->find(update.full_id);
    if (it == index->end()) {
      return false;
    }
  }
  auto& order_item = (*status_item_order)[it->second];
  if (update.has_text) {
    order_item.text = update.text;
  }
  if (update.has_tooltip) {
    order_item.tooltip = update.tooltip;
  }
  if (update.has_icon) {
    order_item.icon = update.icon;
  }
  if (update.has_tone) {
    order_item.tone = update.tone;
  }
  if (update.has_progress) {
    order_item.progress = update.progress;
  }
  return true;
}

}  // namespace microide::plugin::registry_interop

#endif
