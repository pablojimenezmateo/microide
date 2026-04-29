#include "plugin/PluginRuntimeApiInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginDiagnosticsInterop.h"

namespace microide::plugin::runtime_api_interop {

int LuaDiagnosticsPublish(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks) {
  const char* raw_path = luaL_checkstring(state, 1);
  luaL_checktype(state, 2, LUA_TTABLE);
  std::string error_message;
  if (plugin == nullptr ||
      !diagnostics_interop::PublishDiagnostics(state, plugin->id, current_project_root, raw_path, 2,
                                               callbacks, &error_message)) {
    return luaL_error(state, "%s",
                      error_message.empty() ? "failed to publish diagnostics"
                                            : error_message.c_str());
  }
  return 0;
}

int LuaDiagnosticsClear(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::optional<std::filesystem::path>& path,
                        const PluginHost::Callbacks& callbacks) {
  std::string error_message;
  if (plugin == nullptr ||
      !diagnostics_interop::ClearDiagnostics(plugin->id, path, callbacks, &error_message)) {
    return luaL_error(state, "%s",
                      error_message.empty() ? "failed to clear diagnostics"
                                            : error_message.c_str());
  }
  return 0;
}

int LuaSidebarShow(lua_State* state, const PluginHost::Callbacks& callbacks) {
  const char* id = luaL_checkstring(state, 1);
  if (!callbacks.show_sidebar) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, callbacks.show_sidebar(id) ? 1 : 0);
  return 1;
}

}  // namespace microide::plugin::runtime_api_interop

#endif
