#include "plugin/PluginProcessInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/Subprocess.h"
#include "plugin/LuaError.h"
#include "plugin/PluginAsyncStateInterop.h"
#include "plugin/PluginPathInterop.h"

namespace microide::plugin::process_interop {
namespace {

using path_interop::ResolveRuntimePath;

// Native form of the `process.run` arguments. Holds every heap-backed object the
// call needs so the parse step can fail without leaving such an object alive on
// the stack when the caller raises a Lua error (a C longjmp — see LuaError.h).
struct ProcessRunArgs {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::string stdin_text;
  std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
};

// Parses the argv table (index 1) and optional options table (index 2) into
// `out`. Returns nullptr on success, or a static error-message literal on a type
// mismatch. Never calls luaL_error / luaL_checktype: those longjmp, which would
// skip the destructors of `out`'s std::vector / std::string members. The caller
// raises the returned literal only after `out` has gone out of scope.
const char* ParseProcessRunArgs(lua_State* state,
                                const std::filesystem::path& current_project_root,
                                ProcessRunArgs* out) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return "process argv must be a table";
  }
  const lua_Integer argc = static_cast<lua_Integer>(lua_rawlen(state, 1));
  out->argv.reserve(static_cast<std::size_t>(argc));
  for (lua_Integer i = 1; i <= argc; ++i) {
    lua_rawgeti(state, 1, i);
    if (!lua_isstring(state, -1)) {
      lua_pop(state, 1);
      return "process argv entries must be strings";
    }
    out->argv.emplace_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }

  out->cwd = current_project_root;
  if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
    if (lua_type(state, 2) != LUA_TTABLE) {
      return "process options must be a table";
    }
    lua_getfield(state, 2, "cwd");
    if (lua_isstring(state, -1)) {
      out->cwd =
          ResolveRuntimePath(current_project_root, std::filesystem::path(lua_tostring(state, -1)));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "stdin");
    if (lua_isstring(state, -1)) {
      size_t length = 0;
      const char* text = lua_tolstring(state, -1, &length);
      out->stdin_text.assign(text, length);
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "env");
    if (!lua_isnil(state, -1)) {
      if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return "process env must be a table";
      }
      lua_pushnil(state);
      while (lua_next(state, -2) != 0) {
        if (!lua_isstring(state, -2)) {
          lua_pop(state, 2);
          return "process env keys must be strings";
        }

        platform::SubprocessEnvironmentOverride override_entry;
        override_entry.name = lua_tostring(state, -2);
        if (lua_isstring(state, -1)) {
          size_t length = 0;
          const char* text = lua_tolstring(state, -1, &length);
          override_entry.value = std::string(text, length);
        } else if (lua_isboolean(state, -1) && lua_toboolean(state, -1) == 0) {
          override_entry.value = std::nullopt;
        } else {
          lua_pop(state, 2);
          return "process env values must be strings or false";
        }

        out->environment_overrides.push_back(std::move(override_entry));
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }

  return nullptr;
}

}  // namespace

int LuaProcessRun(lua_State* state, const std::filesystem::path& current_project_root) {
  const char* error = nullptr;
  {
    ProcessRunArgs parsed;
    error = ParseProcessRunArgs(state, current_project_root, &parsed);
    if (error == nullptr) {
      const platform::SubprocessResult result = platform::RunSubprocess(
          parsed.argv, platform::SubprocessOptions{
                           .cwd = parsed.cwd,
                           .stdin_text = parsed.stdin_text,
                           .environment_overrides = std::move(parsed.environment_overrides),
                           .capture_stdout = true,
                           .capture_stderr = true,
                       });
      lua_createtable(state, 0, 4);
      lua_pushinteger(state, result.exit_code);
      lua_setfield(state, -2, "exit_code");
      lua_pushboolean(state, result.exit_code == 0 ? 1 : 0);
      lua_setfield(state, -2, "ok");
      lua_pushlstring(state, result.stdout_text.c_str(), result.stdout_text.size());
      lua_setfield(state, -2, "stdout");
      lua_pushlstring(state, result.stderr_text.c_str(), result.stderr_text.size());
      lua_setfield(state, -2, "stderr");
      return 1;
    }
  }
  lua_error_util::PushMessage(state, error);
  return lua_error_util::kPendingError;
}

int LuaProcessRunAsync(lua_State* state,
                       const std::filesystem::path& current_project_root,
                       std::shared_ptr<runtime_types::AsyncProcessState> async_process_state) {
  if (lua_type(state, 3) != LUA_TFUNCTION) {
    lua_error_util::PushMessage(state, "process.run_async requires a callback function");
    return lua_error_util::kPendingError;
  }

  const char* error = nullptr;
  {
    ProcessRunArgs parsed;
    error = ParseProcessRunArgs(state, current_project_root, &parsed);
    if (error != nullptr) {
      // fall through to the raise below once `parsed` destructs.
    } else if (!async_process_state) {
      return 0;
    } else {
      lua_pushvalue(state, 3);
      const int callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
      const auto request = std::make_shared<runtime_types::AsyncProcessRequest>();
      request->lua_state = state;
      request->callback_ref = callback_ref;
      {
        std::lock_guard lock(async_process_state->mutex);
        async_process_state->active_requests.push_back(request);
      }

      platform::SubprocessOptions opts{
          .cwd = std::move(parsed.cwd),
          .stdin_text = std::move(parsed.stdin_text),
          .environment_overrides = std::move(parsed.environment_overrides),
          .capture_stdout = true,
          .capture_stderr = true,
      };

      async_process_state->in_flight.fetch_add(1, std::memory_order_relaxed);
      std::thread([async_process_state,
                   request,
                   argv = std::move(parsed.argv),
                   opts = std::move(opts)]() mutable {
    platform::SubprocessResult result = platform::RunSubprocess(argv, opts);
    Uint32 event_type = 0;
    bool should_push_event = false;
    {
      std::lock_guard lock(async_process_state->mutex);
      auto it = std::find(async_process_state->active_requests.begin(),
                          async_process_state->active_requests.end(), request);
      if (it != async_process_state->active_requests.end()) {
        async_process_state->active_requests.erase(it);
      }
      if (!request->cancelled && request->lua_state != nullptr && request->callback_ref != LUA_NOREF) {
        async_process_state->pending_callbacks.push_back(
            {request->lua_state, request->callback_ref, std::move(result)});
        request->lua_state = nullptr;
        request->callback_ref = LUA_NOREF;
        event_type = async_process_state->event_type;
        should_push_event = true;
      }
    }
    async_process_state->in_flight.fetch_sub(1, std::memory_order_release);
    async_state_interop::NotifyWorkerCompleted(*async_process_state);
    if (should_push_event && event_type != 0) {
      SDL_Event event{};
      event.type = event_type;
      SDL_PushEvent(&event);
    }
  }).detach();

      return 0;
    }
  }
  lua_error_util::PushMessage(state, error);
  return lua_error_util::kPendingError;
}

}  // namespace microide::plugin::process_interop

#endif
