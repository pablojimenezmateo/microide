#include "plugin/PluginStateTeardownInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>

#include "plugin/PluginRegistryInterop.h"

namespace microide::plugin::state_teardown_interop {

void ClearPluginDiagnostics(
    const runtime_types::PluginInstance* plugin,
    const std::function<void(std::string_view)>& clear_owner_diagnostics) {
  if (plugin == nullptr || plugin->id.empty() || !clear_owner_diagnostics) {
    return;
  }
  clear_owner_diagnostics(plugin->id);
}

void DestroyPluginState(runtime_types::PluginInstance* plugin) {
  if (plugin == nullptr || plugin->state == nullptr) {
    return;
  }

  auto unref = [&](int* ref) {
    if (*ref != LUA_NOREF && *ref != LUA_REFNIL) {
      luaL_unref(plugin->state, LUA_REGISTRYINDEX, *ref);
      *ref = LUA_NOREF;
    }
  };
  unref(&plugin->setup_ref);
  unref(&plugin->on_project_open_ref);
  unref(&plugin->on_project_close_ref);
  unref(&plugin->on_buffer_open_ref);
  unref(&plugin->on_buffer_save_ref);
  unref(&plugin->on_buffer_change_ref);
  unref(&plugin->on_cursor_move_ref);
  unref(&plugin->on_selection_change_ref);
  unref(&plugin->on_buffer_close_ref);
  unref(&plugin->shutdown_ref);
  plugin->runtime.reset();
  plugin->state = nullptr;
}

namespace {

template <typename Contribution>
void EraseOwnedBy(std::vector<Contribution>& contributions, const std::string& plugin_id) {
  std::erase_if(contributions,
                [&](const Contribution& entry) { return entry.plugin_id == plugin_id; });
}

void UnrefIfSet(lua_State* state, int ref) {
  if (ref != LUA_NOREF && ref != LUA_REFNIL) {
    luaL_unref(state, LUA_REGISTRYINDEX, ref);
  }
}

}

void UnregisterContributionsForState(lua_State* state, const runtime_types::PluginInstance* plugin,
                                     const ContributionLists& lists) {
  std::erase_if(*lists.commands, [&](const auto& entry) {
    if (entry.second.state != state) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.second.function_ref);
    return true;
  });
  registry_interop::RebuildCommandNames(*lists.commands, lists.command_names);

  std::erase_if(*lists.sidebars, [&](const auto& entry) {
    if (entry.second.state != state) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.second.snapshot_ref);
    UnrefIfSet(state, entry.second.confirm_ref);
    UnrefIfSet(state, entry.second.toggle_ref);
    return true;
  });
  registry_interop::RebuildSidebarProviders(*lists.sidebars, lists.sidebar_providers);

  std::erase_if(*lists.hovers, [&](const auto& entry) {
    if (entry.second.state != state) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.second.provide_ref);
    return true;
  });
  lists.hover_provider_order->erase(
      std::remove_if(lists.hover_provider_order->begin(), lists.hover_provider_order->end(),
                     [&](std::string_view id) { return !lists.hovers->contains(std::string(id)); }),
      lists.hover_provider_order->end());

  if (plugin == nullptr) {
    return;
  }
  const std::string plugin_id = plugin->id;
  EraseOwnedBy(*lists.menu_entries, plugin_id);
  EraseOwnedBy(*lists.keybindings, plugin_id);
  EraseOwnedBy(*lists.settings, plugin_id);
  std::erase_if(*lists.status_items,
                [&](const auto& entry) { return entry.second.plugin_id == plugin_id; });
  EraseOwnedBy(*lists.status_item_order, plugin_id);
  EraseOwnedBy(*lists.formatters, plugin_id);
  EraseOwnedBy(*lists.save_participants, plugin_id);
  EraseOwnedBy(*lists.virtual_documents, plugin_id);
  std::erase_if(*lists.save_participant_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.function_ref);
    return true;
  });
  EraseOwnedBy(*lists.completions, plugin_id);
  std::erase_if(*lists.completion_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.provide_ref);
    return true;
  });
  EraseOwnedBy(*lists.code_actions, plugin_id);
  std::erase_if(*lists.code_action_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.provide_ref);
    return true;
  });
  std::erase_if(*lists.language_query_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, entry.provide_ref);
    return true;
  });
  EraseOwnedBy(*lists.language_servers, plugin_id);
  EraseOwnedBy(*lists.debug_adapters, plugin_id);
  EraseOwnedBy(*lists.launch_configs, plugin_id);
  EraseOwnedBy(*lists.tasks, plugin_id);
  EraseOwnedBy(*lists.tools, plugin_id);
  EraseOwnedBy(*lists.test_providers, plugin_id);
  std::erase_if(*lists.test_provider_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    UnrefIfSet(state, entry.discover_ref);
    UnrefIfSet(state, entry.run_ref);
    return true;
  });
  EraseOwnedBy(*lists.scm_providers, plugin_id);
  std::erase_if(*lists.scm_provider_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    UnrefIfSet(state, entry.snapshot_ref);
    return true;
  });
  EraseOwnedBy(*lists.annotation_providers, plugin_id);
  std::erase_if(*lists.annotation_provider_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    UnrefIfSet(state, entry.provide_ref);
    return true;
  });
  EraseOwnedBy(*lists.auth_providers, plugin_id);
  std::erase_if(*lists.auth_provider_runtimes, [&](const auto& entry) {
    if (entry.plugin_id != plugin_id) {
      return false;
    }
    UnrefIfSet(state, entry.login_ref);
    UnrefIfSet(state, entry.refresh_ref);
    UnrefIfSet(state, entry.logout_ref);
    return true;
  });
  EraseOwnedBy(*lists.bracket_sets, plugin_id);
  EraseOwnedBy(*lists.comment_markers, plugin_id);
  EraseOwnedBy(*lists.indent_rules, plugin_id);
  EraseOwnedBy(*lists.snippets, plugin_id);
  EraseOwnedBy(*lists.themes, plugin_id);
  lists.file_icon_themes->erase(std::remove_if(lists.file_icon_themes->begin(), lists.file_icon_themes->end(),
                                         [&](const PluginHost::ContributedFileIconTheme& e) {
                                           return e.plugin_id == plugin_id;
                                         }),
                          lists.file_icon_themes->end());
}

}

#endif
