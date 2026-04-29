#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "platform/Subprocess.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::runtime_types {

struct PluginInstance {
  std::string id;
  std::filesystem::path root;
  bool project_local = false;
#if MICROIDE_HAS_LUA_PLUGINS
  std::unique_ptr<LuaRuntime> runtime;
  lua_State* state = nullptr;
  int setup_ref = LUA_NOREF;
  int on_project_open_ref = LUA_NOREF;
  int on_project_close_ref = LUA_NOREF;
  int on_buffer_open_ref = LUA_NOREF;
  int on_buffer_save_ref = LUA_NOREF;
  int shutdown_ref = LUA_NOREF;
#endif
};

struct PluginCommand {
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int function_ref = LUA_NOREF;
#endif
};

struct SidebarProvider {
  PluginHost::SidebarProviderInfo info;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int snapshot_ref = LUA_NOREF;
  int confirm_ref = LUA_NOREF;
#endif
};

struct HoverProvider {
  std::string id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int provide_ref = LUA_NOREF;
#endif
};

struct SaveParticipantRuntime {
  std::string id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int function_ref = LUA_NOREF;
#endif
};

struct CompletionRuntime {
  std::string id;
  std::string language_id;
  std::string trigger_characters;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int provide_ref = LUA_NOREF;
#endif
};

struct CodeActionRuntime {
  std::string id;
  std::string language_id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int provide_ref = LUA_NOREF;
#endif
};

struct TestProviderRuntime {
  std::string id;
  std::string language_id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int discover_ref = LUA_NOREF;
  int run_ref = LUA_NOREF;
#endif
};

struct ScmProviderRuntime {
  std::string id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int snapshot_ref = LUA_NOREF;
#endif
};

struct AnnotationProviderRuntime {
  std::string id;
  std::string language_id;
  std::string type;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int provide_ref = LUA_NOREF;
#endif
};

struct AuthProviderRuntime {
  std::string id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int login_ref = LUA_NOREF;
  int refresh_ref = LUA_NOREF;
  int logout_ref = LUA_NOREF;
#endif
};

struct McpToolRuntime {
  std::string id;
  std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = nullptr;
  int run_ref = LUA_NOREF;
#endif
};

struct AsyncProcessCallback {
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* lua_state = nullptr;
  int callback_ref = LUA_NOREF;
#endif
  platform::SubprocessResult result;
};

struct AsyncProcessRequest {
#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* lua_state = nullptr;
  int callback_ref = LUA_NOREF;
#endif
  bool cancelled = false;
};

struct AsyncProcessState {
  Uint32 event_type = 0;
  std::mutex mutex;
  std::atomic<int> in_flight{0};
  std::vector<std::shared_ptr<AsyncProcessRequest>> active_requests;
  std::vector<AsyncProcessCallback> pending_callbacks;
};

}  // namespace microide::plugin::runtime_types
