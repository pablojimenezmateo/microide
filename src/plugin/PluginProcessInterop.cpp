#include "plugin/PluginProcessInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/Subprocess.h"
#include "plugin/PluginAsyncStateInterop.h"

namespace microide::plugin::process_interop {
namespace {

std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  if (path.is_absolute() || project_root.empty()) {
    return path.lexically_normal();
  }
  return (project_root / path).lexically_normal();
}

}  // namespace

int LuaProcessRun(lua_State* state, const std::filesystem::path& current_project_root) {
  luaL_checktype(state, 1, LUA_TTABLE);

  std::vector<std::string> argv;
  const lua_Integer argc = static_cast<lua_Integer>(lua_rawlen(state, 1));
  argv.reserve(static_cast<std::size_t>(argc));
  for (lua_Integer i = 1; i <= argc; ++i) {
    lua_rawgeti(state, 1, i);
    if (!lua_isstring(state, -1)) {
      return luaL_error(state, "process argv entries must be strings");
    }
    argv.emplace_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }

  std::filesystem::path cwd = current_project_root;
  std::string stdin_text;
  std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
  if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(state, 2, "cwd");
    if (lua_isstring(state, -1)) {
      cwd = ResolveRuntimePath(current_project_root, std::filesystem::path(lua_tostring(state, -1)));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "stdin");
    if (lua_isstring(state, -1)) {
      size_t length = 0;
      const char* text = lua_tolstring(state, -1, &length);
      stdin_text.assign(text, length);
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "env");
    if (!lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      lua_pushnil(state);
      while (lua_next(state, -2) != 0) {
        if (!lua_isstring(state, -2)) {
          return luaL_error(state, "process env keys must be strings");
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
          return luaL_error(state, "process env values must be strings or false");
        }

        environment_overrides.push_back(std::move(override_entry));
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }

  const platform::SubprocessResult result = platform::RunSubprocess(
      argv, platform::SubprocessOptions{
                .cwd = cwd,
                .stdin_text = stdin_text,
                .environment_overrides = std::move(environment_overrides),
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

int LuaProcessRunAsync(lua_State* state,
                       const std::filesystem::path& current_project_root,
                       std::shared_ptr<runtime_types::AsyncProcessState> async_process_state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  luaL_checktype(state, 3, LUA_TFUNCTION);

  std::vector<std::string> argv;
  const lua_Integer argc = static_cast<lua_Integer>(lua_rawlen(state, 1));
  argv.reserve(static_cast<std::size_t>(argc));
  for (lua_Integer i = 1; i <= argc; ++i) {
    lua_rawgeti(state, 1, i);
    if (!lua_isstring(state, -1)) {
      return luaL_error(state, "process argv entries must be strings");
    }
    argv.emplace_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }

  std::filesystem::path cwd = current_project_root;
  std::string stdin_text;
  std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
  if (!lua_isnil(state, 2)) {
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(state, 2, "cwd");
    if (lua_isstring(state, -1)) {
      cwd = ResolveRuntimePath(current_project_root, std::filesystem::path(lua_tostring(state, -1)));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "stdin");
    if (lua_isstring(state, -1)) {
      size_t length = 0;
      const char* text = lua_tolstring(state, -1, &length);
      stdin_text.assign(text, length);
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "env");
    if (!lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      lua_pushnil(state);
      while (lua_next(state, -2) != 0) {
        if (!lua_isstring(state, -2)) {
          return luaL_error(state, "process env keys must be strings");
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
          return luaL_error(state, "process env values must be strings or false");
        }
        environment_overrides.push_back(std::move(override_entry));
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }

  if (!async_process_state) {
    return 0;
  }

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
      .cwd = std::move(cwd),
      .stdin_text = std::move(stdin_text),
      .environment_overrides = std::move(environment_overrides),
      .capture_stdout = true,
      .capture_stderr = true,
  };

  async_process_state->in_flight.fetch_add(1, std::memory_order_relaxed);
  std::thread([async_process_state,
               request,
               argv = std::move(argv),
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

}  // namespace microide::plugin::process_interop

#endif
