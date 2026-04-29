#include "plugin/PluginLifecycleCallbackInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

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

}  // namespace microide::plugin::lifecycle_callback_interop

#endif
