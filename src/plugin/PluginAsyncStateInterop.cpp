#include "plugin/PluginAsyncStateInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <mutex>

namespace microide::plugin::async_state_interop {

void SetEventType(runtime_types::AsyncProcessState& state, std::uint32_t type) {
  std::lock_guard lock(state.mutex);
  state.event_type = static_cast<Uint32>(type);
}

void CancelCallbacks(runtime_types::AsyncProcessState& state) {
  std::lock_guard lock(state.mutex);
  for (auto& request : state.active_requests) {
    if (!request) {
      continue;
    }
    if (request->callback_ref != LUA_NOREF && request->lua_state != nullptr) {
      luaL_unref(request->lua_state, LUA_REGISTRYINDEX, request->callback_ref);
    }
    request->lua_state = nullptr;
    request->callback_ref = LUA_NOREF;
    request->cancelled = true;
  }
  state.active_requests.clear();
  for (auto& callback : state.pending_callbacks) {
    if (callback.callback_ref != LUA_NOREF && callback.lua_state != nullptr) {
      luaL_unref(callback.lua_state, LUA_REGISTRYINDEX, callback.callback_ref);
    }
    callback.lua_state = nullptr;
    callback.callback_ref = LUA_NOREF;
  }
  state.pending_callbacks.clear();
}

std::vector<runtime_types::AsyncProcessCallback> TakePendingCallbacks(
    runtime_types::AsyncProcessState& state) {
  std::vector<runtime_types::AsyncProcessCallback> callbacks;
  std::lock_guard lock(state.mutex);
  callbacks.swap(state.pending_callbacks);
  return callbacks;
}

int PendingCount(runtime_types::AsyncProcessState& state) {
  std::lock_guard lock(state.mutex);
  int active_count = 0;
  for (const auto& request : state.active_requests) {
    if (request != nullptr && !request->cancelled) {
      ++active_count;
    }
  }
  return active_count + static_cast<int>(state.pending_callbacks.size());
}

}  // namespace microide::plugin::async_state_interop

#endif
