#include "plugin/PluginRuntimeApiInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/LuaError.h"
#include "plugin/PluginDiagnosticsInterop.h"

namespace microide::plugin::runtime_api_interop {

// These delegating functions are called by the thin wrappers in
// PluginHostLuaApi.inc. They never longjmp (no luaL_error / luaL_check): on any
// error they push the message and return lua_error_util::kPendingError, and the
// wrapper raises after its own locals destruct. See src/plugin/LuaError.h.

int LuaDiagnosticsPublish(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks) {
  if (!lua_isstring(state, 1)) {
    lua_error_util::PushMessage(state, "diagnostics.publish requires a path string");
    return lua_error_util::kPendingError;
  }
  if (lua_type(state, 2) != LUA_TTABLE) {
    lua_error_util::PushMessage(state, "diagnostics.publish requires a diagnostics table");
    return lua_error_util::kPendingError;
  }
  const char* raw_path = lua_tostring(state, 1);
  {
    std::string error_message;
    if (plugin != nullptr &&
        diagnostics_interop::PublishDiagnostics(state, plugin->id, current_project_root, raw_path, 2,
                                                callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to publish diagnostics");
  }
  return lua_error_util::kPendingError;
}

int LuaDiagnosticsClear(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::optional<std::filesystem::path>& path,
                        const PluginHost::Callbacks& callbacks) {
  {
    std::string error_message;
    if (plugin != nullptr &&
        diagnostics_interop::ClearDiagnostics(plugin->id, path, callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to clear diagnostics");
  }
  return lua_error_util::kPendingError;
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
