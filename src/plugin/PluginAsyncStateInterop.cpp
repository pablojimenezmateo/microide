#include "plugin/PluginAsyncStateInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <mutex>
#endif

namespace microide::plugin::async_state_interop {

void SetEventType(runtime_types::AsyncProcessState& state, std::uint32_t type) {
#if MICROIDE_HAS_LUA_PLUGINS
  std::lock_guard lock(state.mutex);
  state.event_type = static_cast<Uint32>(type);
#else
  (void)state;
  (void)type;
#endif
}

void CancelCallbacks(runtime_types::AsyncProcessState& state) {
#if MICROIDE_HAS_LUA_PLUGINS
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
  state.queued.store(0, std::memory_order_release);
#else
  (void)state;
#endif
}

std::vector<runtime_types::AsyncProcessCallback> TakePendingCallbacks(
    runtime_types::AsyncProcessState& state) {
#if MICROIDE_HAS_LUA_PLUGINS
  std::vector<runtime_types::AsyncProcessCallback> callbacks;
  std::lock_guard lock(state.mutex);
  callbacks.swap(state.pending_callbacks);
  state.queued.store(static_cast<int>(state.active_requests.size()),
                     std::memory_order_release);
  return callbacks;
#else
  (void)state;
  return {};
#endif
}

int PendingCount(runtime_types::AsyncProcessState& state) {
#if MICROIDE_HAS_LUA_PLUGINS
  // Fast path: with no plugin async work queued (the steady state on every
  // scheduled wake) skip the lock and vector scan entirely. `queued` is only ever
  // non-zero while a request is in flight or a callback awaits draining.
  if (state.queued.load(std::memory_order_acquire) == 0) {
    return 0;
  }
  std::lock_guard lock(state.mutex);
  int active_count = 0;
  for (const auto& request : state.active_requests) {
    if (request != nullptr && !request->cancelled) {
      ++active_count;
    }
  }
  return active_count + static_cast<int>(state.pending_callbacks.size());
#else
  (void)state;
  return 0;
#endif
}

void NotifyWorkerCompleted(runtime_types::AsyncProcessState& state) {
#if MICROIDE_HAS_LUA_PLUGINS
  state.in_flight_cv.notify_all();
#else
  (void)state;
#endif
}

bool DrainAndJoinWorkers(runtime_types::AsyncProcessState& state,
                         std::chrono::milliseconds deadline) {
#if MICROIDE_HAS_LUA_PLUGINS
  CancelCallbacks(state);
  std::unique_lock lock(state.mutex);
  return state.in_flight_cv.wait_for(lock, deadline, [&state] {
    return state.in_flight.load(std::memory_order_acquire) == 0;
  });
#else
  (void)state;
  (void)deadline;
  return true;
#endif
}

}  // namespace microide::plugin::async_state_interop
