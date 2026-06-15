#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::lifecycle_callback_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool CallSetup(runtime_types::PluginInstance* plugin,
               runtime_types::PluginInstance** active_plugin_slot,
               const std::function<void(lua_State*)>& push_plugin_context,
               const std::function<std::string(const runtime_types::PluginInstance*)>&
                   format_plugin_prefix,
               std::string* error_message);

void CallProjectCallback(runtime_types::PluginInstance* plugin,
                         int ref,
                         const char* callback_name,
                         bool has_project_root,
                         const std::function<void(lua_State*)>& push_plugin_context,
                         const std::function<void(lua_State*)>& push_project_table,
                         const std::function<void(std::string)>& record_error,
                         const std::function<std::string(const runtime_types::PluginInstance*)>&
                             format_plugin_prefix);

void CallBufferCallback(runtime_types::PluginInstance* plugin,
                        int ref,
                        const char* callback_name,
                        const std::filesystem::path& path,
                        const std::function<void(lua_State*)>& push_plugin_context,
                        const std::function<void(lua_State*, const std::filesystem::path&)>&
                            push_buffer_table,
                        const std::function<void(std::string)>& record_error,
                        const std::function<std::string(const runtime_types::PluginInstance*)>&
                            format_plugin_prefix);

void CallShutdown(runtime_types::PluginInstance* plugin,
                  const std::function<void(lua_State*)>& push_plugin_context,
                  const std::function<void(std::string)>& record_error,
                  const std::function<std::string(const runtime_types::PluginInstance*)>&
                      format_plugin_prefix);

void TearDownPlugins(std::vector<runtime_types::PluginInstance>* plugins,
                     bool has_project_root,
                     const std::function<void(runtime_types::PluginInstance*, int, const char*)>&
                         call_project_callback,
                     const std::function<void(runtime_types::PluginInstance*)>& call_shutdown,
                     const std::function<void(lua_State*)>& unregister_for_state,
                     const std::function<void(const runtime_types::PluginInstance*)>&
                         clear_plugin_diagnostics,
                     const std::function<void(runtime_types::PluginInstance*)>&
                         destroy_plugin_state);

bool LoadPluginRoot(const std::filesystem::path& plugin_root,
                    std::vector<runtime_types::PluginInstance>* plugins,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        initialize_state,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        load_plugin_descriptor,
                    const std::function<bool(runtime_types::PluginInstance*, std::string*)>&
                        call_setup,
                    const std::function<void(lua_State*)>& unregister_for_state,
                    const std::function<void(const runtime_types::PluginInstance*)>&
                        clear_plugin_diagnostics,
                    const std::function<void(runtime_types::PluginInstance*)>&
                        destroy_plugin_state,
                    const std::function<bool(const std::string&)>& is_plugin_disabled,
                    const std::function<void(const runtime_types::PluginInstance&)>& record_disabled,
                    std::string* error_message);
#endif

}  // namespace microide::plugin::lifecycle_callback_interop
