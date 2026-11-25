#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "platform/Subprocess.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginCapabilities.h"
#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::runtime_types {

struct PluginInstance {
  std::string id;
  std::filesystem::path root;
  std::filesystem::path data_dir;
  PluginCapabilities capabilities;
#if MICROIDE_HAS_LUA_PLUGINS
  std::unique_ptr<LuaRuntime> runtime;
  lua_State* state = nullptr;
  int setup_ref = LUA_NOREF;
  int on_project_open_ref = LUA_NOREF;
  int on_project_close_ref = LUA_NOREF;
  int on_buffer_open_ref = LUA_NOREF;
  int on_buffer_save_ref = LUA_NOREF;
  int on_buffer_change_ref = LUA_NOREF;
  int on_cursor_move_ref = LUA_NOREF;
  int on_selection_change_ref = LUA_NOREF;
  int on_buffer_close_ref = LUA_NOREF;
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
  int toggle_ref = LUA_NOREF;
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

using LanguageQueryKind = PluginHost::ContributedLanguageProvider::Kind;

struct LanguageQueryRuntime {
  LanguageQueryKind kind = LanguageQueryKind::Definition;
  std::string id;
  std::string language_id;
  std::string plugin_id;
  std::string trigger_characters;
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

}
