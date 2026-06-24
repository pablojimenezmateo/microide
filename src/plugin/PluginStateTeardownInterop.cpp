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
  unref(&plugin->shutdown_ref);
  plugin->runtime.reset();
  plugin->state = nullptr;
}

void UnregisterContributionsForState(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<PluginHost::ContributedMenuEntry>* menu_entries,
    std::vector<PluginHost::ContributedKeybinding>* keybindings,
    std::vector<PluginHost::ContributedSettingSpec>* settings,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    std::vector<PluginHost::ContributedFormatter>* formatters,
    std::vector<PluginHost::ContributedSaveParticipant>* save_participants,
    std::vector<runtime_types::SaveParticipantRuntime>* save_participant_runtimes,
    std::vector<PluginHost::ContributedCompletion>* completions,
    std::vector<runtime_types::CompletionRuntime>* completion_runtimes,
    std::vector<PluginHost::ContributedCodeAction>* code_actions,
    std::vector<runtime_types::CodeActionRuntime>* code_action_runtimes,
    std::vector<runtime_types::LanguageQueryRuntime>* language_query_runtimes,
    std::vector<PluginHost::ContributedLanguageServer>* language_servers,
    std::vector<PluginHost::ContributedDebugAdapter>* debug_adapters,
    std::vector<PluginHost::ContributedLaunchConfig>* launch_configs,
    std::vector<PluginHost::ContributedTask>* tasks,
    std::vector<PluginHost::ContributedTool>* tools,
    std::vector<PluginHost::ContributedTestProvider>* test_providers,
    std::vector<runtime_types::TestProviderRuntime>* test_provider_runtimes,
    std::vector<PluginHost::ContributedScmProvider>* scm_providers,
    std::vector<runtime_types::ScmProviderRuntime>* scm_provider_runtimes,
    std::vector<PluginHost::ContributedAnnotationProvider>* annotation_providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* annotation_provider_runtimes,
    std::vector<PluginHost::ContributedAuthProvider>* auth_providers,
    std::vector<runtime_types::AuthProviderRuntime>* auth_provider_runtimes,
    std::vector<PluginHost::ContributedBracketSet>* bracket_sets,
    std::vector<PluginHost::ContributedCommentMarkers>* comment_markers,
    std::vector<PluginHost::ContributedIndentRules>* indent_rules,
    std::vector<PluginHost::ContributedSnippet>* snippets,
    std::vector<PluginHost::ContributedTheme>* themes,
    std::vector<PluginHost::ContributedFileIconTheme>* file_icon_themes) {
  for (auto it = commands->begin(); it != commands->end();) {
    if (it->second.state != state) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->second.function_ref);
    it = commands->erase(it);
  }
  registry_interop::RebuildCommandNames(*commands, command_names);

  for (auto it = sidebars->begin(); it != sidebars->end();) {
    if (it->second.state != state) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->second.snapshot_ref);
    if (it->second.confirm_ref != LUA_NOREF && it->second.confirm_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.confirm_ref);
    }
    if (it->second.toggle_ref != LUA_NOREF && it->second.toggle_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.toggle_ref);
    }
    it = sidebars->erase(it);
  }
  registry_interop::RebuildSidebarProviders(*sidebars, sidebar_providers);

  for (auto it = hovers->begin(); it != hovers->end();) {
    if (it->second.state != state) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->second.provide_ref);
    it = hovers->erase(it);
  }
  hover_provider_order->erase(
      std::remove_if(hover_provider_order->begin(), hover_provider_order->end(),
                     [&](std::string_view id) { return !hovers->contains(std::string(id)); }),
      hover_provider_order->end());

  if (plugin == nullptr) {
    return;
  }
  const std::string plugin_id = plugin->id;
  menu_entries->erase(std::remove_if(menu_entries->begin(), menu_entries->end(),
                                     [&](const PluginHost::ContributedMenuEntry& e) {
                                       return e.plugin_id == plugin_id;
                                     }),
                     menu_entries->end());
  keybindings->erase(std::remove_if(keybindings->begin(), keybindings->end(),
                                    [&](const PluginHost::ContributedKeybinding& e) {
                                      return e.plugin_id == plugin_id;
                                    }),
                    keybindings->end());
  settings->erase(std::remove_if(settings->begin(), settings->end(),
                                 [&](const PluginHost::ContributedSettingSpec& e) {
                                   return e.plugin_id == plugin_id;
                                 }),
                  settings->end());
  for (auto it = status_items->begin(); it != status_items->end();) {
    if (it->second.plugin_id == plugin_id) {
      it = status_items->erase(it);
    } else {
      ++it;
    }
  }
  status_item_order->erase(
      std::remove_if(status_item_order->begin(), status_item_order->end(),
                     [&](const PluginHost::ContributedStatusItem& e) {
                       return e.plugin_id == plugin_id;
                     }),
      status_item_order->end());
  formatters->erase(std::remove_if(formatters->begin(), formatters->end(),
                                   [&](const PluginHost::ContributedFormatter& e) {
                                     return e.plugin_id == plugin_id;
                                   }),
                    formatters->end());
  save_participants->erase(std::remove_if(save_participants->begin(), save_participants->end(),
                                          [&](const PluginHost::ContributedSaveParticipant& e) {
                                            return e.plugin_id == plugin_id;
                                          }),
                           save_participants->end());
  for (auto it = save_participant_runtimes->begin(); it != save_participant_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->function_ref);
    it = save_participant_runtimes->erase(it);
  }
  completions->erase(
      std::remove_if(completions->begin(), completions->end(),
                     [&](const PluginHost::ContributedCompletion& e) { return e.plugin_id == plugin_id; }),
      completions->end());
  for (auto it = completion_runtimes->begin(); it != completion_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
    it = completion_runtimes->erase(it);
  }
  code_actions->erase(std::remove_if(code_actions->begin(), code_actions->end(),
                                     [&](const PluginHost::ContributedCodeAction& e) {
                                       return e.plugin_id == plugin_id;
                                     }),
                      code_actions->end());
  for (auto it = code_action_runtimes->begin(); it != code_action_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
    it = code_action_runtimes->erase(it);
  }
  for (auto it = language_query_runtimes->begin(); it != language_query_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
    it = language_query_runtimes->erase(it);
  }
  language_servers->erase(
      std::remove_if(language_servers->begin(), language_servers->end(),
                     [&](const PluginHost::ContributedLanguageServer& e) {
                       return e.plugin_id == plugin_id;
                     }),
      language_servers->end());
  debug_adapters->erase(
      std::remove_if(debug_adapters->begin(), debug_adapters->end(),
                     [&](const PluginHost::ContributedDebugAdapter& e) {
                       return e.plugin_id == plugin_id;
                     }),
      debug_adapters->end());
  launch_configs->erase(
      std::remove_if(launch_configs->begin(), launch_configs->end(),
                     [&](const PluginHost::ContributedLaunchConfig& e) {
                       return e.plugin_id == plugin_id;
                     }),
      launch_configs->end());
  tasks->erase(std::remove_if(tasks->begin(), tasks->end(),
                              [&](const PluginHost::ContributedTask& e) {
                                return e.plugin_id == plugin_id;
                              }),
               tasks->end());
  tools->erase(std::remove_if(tools->begin(), tools->end(),
                              [&](const PluginHost::ContributedTool& e) {
                                return e.plugin_id == plugin_id;
                              }),
               tools->end());
  test_providers->erase(std::remove_if(test_providers->begin(), test_providers->end(),
                                       [&](const PluginHost::ContributedTestProvider& e) {
                                         return e.plugin_id == plugin_id;
                                       }),
                        test_providers->end());
  for (auto it = test_provider_runtimes->begin(); it != test_provider_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    if (it->discover_ref != LUA_NOREF && it->discover_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->discover_ref);
    }
    if (it->run_ref != LUA_NOREF && it->run_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->run_ref);
    }
    it = test_provider_runtimes->erase(it);
  }
  scm_providers->erase(std::remove_if(scm_providers->begin(), scm_providers->end(),
                                      [&](const PluginHost::ContributedScmProvider& e) {
                                        return e.plugin_id == plugin_id;
                                      }),
                       scm_providers->end());
  for (auto it = scm_provider_runtimes->begin(); it != scm_provider_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    if (it->snapshot_ref != LUA_NOREF && it->snapshot_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->snapshot_ref);
    }
    it = scm_provider_runtimes->erase(it);
  }
  annotation_providers->erase(
      std::remove_if(annotation_providers->begin(), annotation_providers->end(),
                     [&](const PluginHost::ContributedAnnotationProvider& e) {
                       return e.plugin_id == plugin_id;
                     }),
      annotation_providers->end());
  for (auto it = annotation_provider_runtimes->begin(); it != annotation_provider_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    if (it->provide_ref != LUA_NOREF && it->provide_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
    }
    it = annotation_provider_runtimes->erase(it);
  }
  auth_providers->erase(std::remove_if(auth_providers->begin(), auth_providers->end(),
                                       [&](const PluginHost::ContributedAuthProvider& e) {
                                         return e.plugin_id == plugin_id;
                                       }),
                        auth_providers->end());
  for (auto it = auth_provider_runtimes->begin(); it != auth_provider_runtimes->end();) {
    if (it->plugin_id != plugin_id) {
      ++it;
      continue;
    }
    if (it->login_ref != LUA_NOREF && it->login_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->login_ref);
    }
    if (it->refresh_ref != LUA_NOREF && it->refresh_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->refresh_ref);
    }
    if (it->logout_ref != LUA_NOREF && it->logout_ref != LUA_REFNIL) {
      luaL_unref(state, LUA_REGISTRYINDEX, it->logout_ref);
    }
    it = auth_provider_runtimes->erase(it);
  }
  bracket_sets->erase(std::remove_if(bracket_sets->begin(), bracket_sets->end(),
                                     [&](const PluginHost::ContributedBracketSet& e) {
                                       return e.plugin_id == plugin_id;
                                     }),
                      bracket_sets->end());
  comment_markers->erase(std::remove_if(comment_markers->begin(), comment_markers->end(),
                                        [&](const PluginHost::ContributedCommentMarkers& e) {
                                          return e.plugin_id == plugin_id;
                                        }),
                         comment_markers->end());
  indent_rules->erase(std::remove_if(indent_rules->begin(), indent_rules->end(),
                                     [&](const PluginHost::ContributedIndentRules& e) {
                                       return e.plugin_id == plugin_id;
                                     }),
                      indent_rules->end());
  snippets->erase(std::remove_if(snippets->begin(), snippets->end(),
                                 [&](const PluginHost::ContributedSnippet& e) {
                                   return e.plugin_id == plugin_id;
                                 }),
                  snippets->end());
  themes->erase(std::remove_if(themes->begin(), themes->end(),
                               [&](const PluginHost::ContributedTheme& e) {
                                 return e.plugin_id == plugin_id;
                               }),
                themes->end());
  file_icon_themes->erase(std::remove_if(file_icon_themes->begin(), file_icon_themes->end(),
                                         [&](const PluginHost::ContributedFileIconTheme& e) {
                                           return e.plugin_id == plugin_id;
                                         }),
                          file_icon_themes->end());
}

}  // namespace microide::plugin::state_teardown_interop

#endif
