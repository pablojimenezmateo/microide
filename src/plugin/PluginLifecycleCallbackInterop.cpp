#include "plugin/PluginLifecycleCallbackInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>

namespace microide::plugin::lifecycle_callback_interop {

bool CallSetup(runtime_types::PluginInstance* plugin,
               runtime_types::PluginInstance** active_plugin_slot,
               const std::function<void(lua_State*)>& push_plugin_context,
               const std::function<std::string(const runtime_types::PluginInstance*)>&
                   format_plugin_prefix,
               std::string* error_message) {
  if (plugin == nullptr || active_plugin_slot == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin setup requires an initialized plugin runtime";
    }
    return false;
  }
  if (plugin->setup_ref == LUA_NOREF) {
    return true;
  }

  *active_plugin_slot = plugin;
  lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, plugin->setup_ref);
  push_plugin_context(plugin->state);
  std::string call_error;
  if (!plugin->runtime->PCall(1, 0, &call_error)) {
    if (error_message != nullptr) {
      *error_message = format_plugin_prefix(plugin) + " setup failed: " + call_error;
    }
    *active_plugin_slot = nullptr;
    return false;
  }
  *active_plugin_slot = nullptr;
  return true;
}

void CallProjectCallback(runtime_types::PluginInstance* plugin,
                         int ref,
                         const char* callback_name,
                         bool has_project_root,
                         const std::function<void(lua_State*)>& push_plugin_context,
                         const std::function<void(lua_State*)>& push_project_table,
                         const std::function<void(std::string)>& record_error,
                         const std::function<std::string(const runtime_types::PluginInstance*)>&
                             format_plugin_prefix) {
  if (plugin == nullptr || ref == LUA_NOREF || !has_project_root) {
    return;
  }

  lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, ref);
  push_plugin_context(plugin->state);
  push_project_table(plugin->state);
  std::string call_error;
  if (!plugin->runtime->PCall(2, 0, &call_error)) {
    record_error(format_plugin_prefix(plugin) + " " + callback_name + " failed: " + call_error);
  }
}

void CallBufferCallback(runtime_types::PluginInstance* plugin,
                        int ref,
                        const char* callback_name,
                        const std::filesystem::path& path,
                        const std::function<void(lua_State*)>& push_plugin_context,
                        const std::function<void(lua_State*, const std::filesystem::path&)>&
                            push_buffer_table,
                        const std::function<void(std::string)>& record_error,
                        const std::function<std::string(const runtime_types::PluginInstance*)>&
                            format_plugin_prefix) {
  if (plugin == nullptr || ref == LUA_NOREF || path.empty()) {
    return;
  }
  lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, ref);
  push_plugin_context(plugin->state);
  push_buffer_table(plugin->state, path);
  std::string call_error;
  if (!plugin->runtime->PCall(2, 0, &call_error)) {
    record_error(format_plugin_prefix(plugin) + " " + callback_name + " failed: " + call_error);
  }
}

void CallBufferEventCallback(runtime_types::PluginInstance* plugin,
                             int ref,
                             const char* callback_name,
                             const std::filesystem::path& path,
                             const std::function<void(lua_State*)>& push_plugin_context,
                             const std::function<void(lua_State*, const std::filesystem::path&)>&
                                 push_buffer_table,
                             const std::function<void(lua_State*)>& push_payload,
                             const std::function<void(std::string)>& record_error,
                             const std::function<std::string(const runtime_types::PluginInstance*)>&
                                 format_plugin_prefix) {
  if (plugin == nullptr || ref == LUA_NOREF || path.empty()) {
    return;
  }
  lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, ref);
  push_plugin_context(plugin->state);
  push_buffer_table(plugin->state, path);
  push_payload(plugin->state);
  std::string call_error;
  if (!plugin->runtime->PCall(3, 0, &call_error)) {
    record_error(format_plugin_prefix(plugin) + " " + callback_name + " failed: " + call_error);
  }
}

void CallShutdown(runtime_types::PluginInstance* plugin,
                  const std::function<void(lua_State*)>& push_plugin_context,
                  const std::function<void(std::string)>& record_error,
                  const std::function<std::string(const runtime_types::PluginInstance*)>&
                      format_plugin_prefix) {
  if (plugin == nullptr || plugin->shutdown_ref == LUA_NOREF) {
    return;
  }
  lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, plugin->shutdown_ref);
  push_plugin_context(plugin->state);
  std::string call_error;
  if (!plugin->runtime->PCall(1, 0, &call_error)) {
    record_error(format_plugin_prefix(plugin) + " shutdown failed: " + call_error);
  }
}

void TearDownPlugins(std::vector<runtime_types::PluginInstance>* plugins,
                     bool has_project_root,
                     const std::function<void(runtime_types::PluginInstance*, int, const char*)>&
                         call_project_callback,
                     const std::function<void(runtime_types::PluginInstance*)>& call_shutdown,
                     const std::function<void(lua_State*)>& unregister_for_state,
                     const std::function<void(const runtime_types::PluginInstance*)>&
                         clear_plugin_diagnostics,
                     const std::function<void(runtime_types::PluginInstance*)>&
                         destroy_plugin_state) {
  if (plugins == nullptr) {
    return;
  }
  if (has_project_root) {
    for (auto& plugin : *plugins) {
      call_project_callback(&plugin, plugin.on_project_close_ref, "on_project_close");
    }
  }
  for (auto& plugin : *plugins) {
    call_shutdown(&plugin);
  }
  for (auto& plugin : *plugins) {
    if (plugin.state != nullptr) {
      unregister_for_state(plugin.state);
    }
  }
  for (const auto& plugin : *plugins) {
    clear_plugin_diagnostics(&plugin);
  }
  for (auto& plugin : *plugins) {
    destroy_plugin_state(&plugin);
  }
  plugins->clear();
}

bool LoadPluginRoot(const std::filesystem::path& plugin_root,
                    std::vector<runtime_types::PluginInstance>* plugins,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        initialize_state,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        load_plugin_descriptor,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        call_setup,
                    const std::function<void(const runtime_types::PluginInstance&)>&
                        unregister_for_state,
                    const std::function<void(const runtime_types::PluginInstance*)>&
                        clear_plugin_diagnostics,
                    const std::function<void(runtime_types::PluginInstance*)>&
                        destroy_plugin_state,
                    const std::function<bool(const std::string&)>& is_plugin_disabled,
                    const std::function<void(const runtime_types::PluginInstance&)>& record_disabled,
                    std::string* error_message) {
  if (plugins == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin root load requires plugin storage";
    }
    return false;
  }
  runtime_types::PluginInstance plugin{
      .id = {},
      .root = plugin_root.lexically_normal(),
      .data_dir = {},
      .capabilities = {},
      .runtime = nullptr,
      .state = nullptr,
      .setup_ref = LUA_NOREF,
      .on_project_open_ref = LUA_NOREF,
      .on_project_close_ref = LUA_NOREF,
      .on_buffer_open_ref = LUA_NOREF,
      .on_buffer_save_ref = LUA_NOREF,
      .on_buffer_change_ref = LUA_NOREF,
      .on_cursor_move_ref = LUA_NOREF,
      .on_selection_change_ref = LUA_NOREF,
      .on_buffer_close_ref = LUA_NOREF,
      .shutdown_ref = LUA_NOREF,
  };

  if (!initialize_state(&plugin, error_message)) {
    destroy_plugin_state(&plugin);
    return false;
  }
  if (!load_plugin_descriptor(&plugin, error_message)) {
    destroy_plugin_state(&plugin);
    return false;
  }

  // Disabled plugins load only far enough to learn their id, then stop: no duplicate
  // check, no setup, no lifecycle. They are recorded for the UI and torn down.
  if (is_plugin_disabled && is_plugin_disabled(plugin.id)) {
    if (record_disabled) {
      record_disabled(plugin);
    }
    destroy_plugin_state(&plugin);
    return true;
  }

  const auto duplicate = std::find_if(plugins->begin(), plugins->end(),
                                      [&](const runtime_types::PluginInstance& loaded) {
                                        return loaded.id == plugin.id;
                                      });
  if (duplicate != plugins->end()) {
    if (error_message != nullptr) {
      *error_message = "duplicate plugin id '" + plugin.id + "' in " + plugin.root.string() +
                       " and " + duplicate->root.string();
    }
    destroy_plugin_state(&plugin);
    return false;
  }

  if (!call_setup(&plugin, error_message)) {
    unregister_for_state(plugin);
    clear_plugin_diagnostics(&plugin);
    destroy_plugin_state(&plugin);
    return false;
  }

  plugins->push_back(std::move(plugin));
  return true;
}

}  // namespace microide::plugin::lifecycle_callback_interop

#endif
