#include "plugin/PluginAsyncCallbackInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginAsyncStateInterop.h"

namespace microide::plugin::async_callback_interop {

int ConsumeCallbacks(
    runtime_types::AsyncProcessState& async_process_state,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(std::string)>& error_sink) {
  std::vector<runtime_types::AsyncProcessCallback> callbacks =
      async_state_interop::TakePendingCallbacks(async_process_state);
  for (auto& cb : callbacks) {
    lua_State* state = cb.lua_state;
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    if (state == nullptr || cb.callback_ref == LUA_NOREF) {
      continue;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, cb.callback_ref);
    luaL_unref(state, LUA_REGISTRYINDEX, cb.callback_ref);
    cb.callback_ref = LUA_NOREF;
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, cb.result.exit_code);
    lua_setfield(state, -2, "exit_code");
    lua_pushlstring(state, cb.result.stdout_text.c_str(), cb.result.stdout_text.size());
    lua_setfield(state, -2, "stdout");
    lua_pushlstring(state, cb.result.stderr_text.c_str(), cb.result.stderr_text.size());
    lua_setfield(state, -2, "stderr");
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 0, &call_error)) {
      if (error_sink && !call_error.empty()) {
        error_sink(std::string("plugin async callback: ") + call_error);
      }
    }
  }
  return static_cast<int>(callbacks.size());
}

}  // namespace microide::plugin::async_callback_interop

#endif
