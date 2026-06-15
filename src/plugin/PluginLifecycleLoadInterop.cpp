#include "plugin/PluginLifecycleLoadInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <system_error>

#include "platform/AppDirectories.h"
#include "plugin/LuaErrorMessage.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::lifecycle_load_interop {
namespace {

using lua_interop::IsValidIdentifier;
using microide::plugin::LuaErrorString;

// Reads a string capability field ("none" | "project" | "data") from the `fs` table at
// `table_index`, leaving `fallback` in place for absent or unrecognized values.
FsAccess ParseFsAccess(lua_State* state, int table_index, const char* field, FsAccess fallback) {
  lua_getfield(state, table_index, field);
  FsAccess result = fallback;
  if (lua_isstring(state, -1)) {
    const std::string value = lua_tostring(state, -1);
    if (value == "none") {
      result = FsAccess::kNone;
    } else if (value == "project") {
      result = FsAccess::kProjectScoped;
    } else if (value == "data") {
      result = FsAccess::kProjectAndData;
    }
  }
  lua_pop(state, 1);
  return result;
}

// Parses the optional `capabilities` table on the plugin descriptor at `table_index`.
// Absent capabilities keep struct defaults (project-scoped fs, no process, no network).
// Returns false only when `capabilities` is present but not a table; malformed sub-fields
// fall back to defaults leniently rather than failing the whole plugin load.
bool ParseCapabilities(lua_State* state,
                       int table_index,
                       PluginCapabilities* out,
                       std::string* error_message) {
  lua_getfield(state, table_index, "capabilities");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin capabilities must be a table";
    }
    lua_pop(state, 1);
    return false;
  }
  const int caps_index = lua_absindex(state, -1);

  lua_getfield(state, caps_index, "fs");
  if (lua_istable(state, -1)) {
    const int fs_index = lua_absindex(state, -1);
    out->fs_read = ParseFsAccess(state, fs_index, "read", out->fs_read);
    out->fs_write = ParseFsAccess(state, fs_index, "write", out->fs_write);
  }
  lua_pop(state, 1);

  lua_getfield(state, caps_index, "process");
  if (lua_istable(state, -1)) {
    const int process_index = lua_absindex(state, -1);
    lua_getfield(state, process_index, "exec");
    if (lua_isboolean(state, -1)) {
      out->process_exec = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, process_index, "allow");
    if (lua_istable(state, -1)) {
      const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
      for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, i);
        if (lua_isstring(state, -1)) {
          out->process_allowlist.emplace_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);

  lua_getfield(state, caps_index, "network");
  if (lua_isboolean(state, -1)) {
    out->network = lua_toboolean(state, -1) != 0;
  }
  lua_pop(state, 1);

  lua_pop(state, 1);  // capabilities table
  return true;
}

void ConfigurePackage(lua_State* state,
                      const std::filesystem::path& plugin_root,
                      lua_CFunction open_microide) {
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  const std::string prefix = (plugin_root / "?.lua").generic_string() + ";" +
                             (plugin_root / "?" / "init.lua").generic_string();
  lua_pushlstring(state, prefix.c_str(), prefix.size());
  lua_setfield(state, -2, "path");
  lua_pushliteral(state, "");
  lua_setfield(state, -2, "cpath");
  lua_pushnil(state);
  lua_setfield(state, -2, "loadlib");

  lua_getfield(state, -1, "preload");
  if (lua_istable(state, -1)) {
    lua_pushcfunction(state, open_microide);
    lua_setfield(state, -2, "microide");
  }
  lua_pop(state, 2);
}

int ExtractFunctionRef(lua_State* state,
                       int table_index,
                       const char* field_name,
                       const std::filesystem::path& plugin_root,
                       std::string* error_message) {
  lua_getfield(state, table_index, field_name);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  if (!lua_isfunction(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string("expected ") + field_name + " to be a function in " +
                       plugin_root.string();
    }
    lua_pop(state, 1);
    return LUA_NOREF;
  }
  return luaL_ref(state, LUA_REGISTRYINDEX);
}

}  // namespace

bool InitializeState(runtime_types::PluginInstance* plugin,
                     lua_CFunction open_microide,
                     std::string* error_message) {
  if (plugin == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin state initialization requires a plugin instance";
    }
    return false;
  }
  plugin->runtime = LuaRuntime::Create(error_message);
  if (!plugin->runtime) {
    return false;
  }
  plugin->state = plugin->runtime->state();
  ConfigurePackage(plugin->state, plugin->root, open_microide);
  return true;
}

bool LoadPluginDescriptor(runtime_types::PluginInstance* plugin, std::string* error_message) {
  if (plugin == nullptr || plugin->state == nullptr || !plugin->runtime) {
    if (error_message != nullptr) {
      *error_message = "plugin descriptor load requires an initialized plugin state";
    }
    return false;
  }
  const std::filesystem::path entry_path = plugin->root / "init.lua";
  if (luaL_loadfile(plugin->state, entry_path.string().c_str()) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "failed to load " + entry_path.string() + ": " + LuaErrorString(plugin->state);
    }
    lua_pop(plugin->state, 1);
    return false;
  }
  std::string call_error;
  if (!plugin->runtime->PCall(0, 1, &call_error)) {
    if (error_message != nullptr) {
      *error_message = "failed to evaluate " + entry_path.string() + ": " + call_error;
    }
    return false;
  }
  if (!lua_istable(plugin->state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin entry point must return a table: " + entry_path.string();
    }
    lua_pop(plugin->state, 1);
    return false;
  }

  const int table_index = lua_absindex(plugin->state, -1);
  lua_getfield(plugin->state, table_index, "id");
  if (!lua_isstring(plugin->state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin id must be a string: " + entry_path.string();
    }
    lua_pop(plugin->state, 2);
    return false;
  }
  plugin->id = lua_tostring(plugin->state, -1);
  lua_pop(plugin->state, 1);
  if (!IsValidIdentifier(plugin->id)) {
    if (error_message != nullptr) {
      *error_message = "invalid plugin id: " + plugin->id;
    }
    lua_pop(plugin->state, 1);
    return false;
  }

  if (!ParseCapabilities(plugin->state, table_index, &plugin->capabilities, error_message)) {
    lua_pop(plugin->state, 1);
    return false;
  }

  // Resolve the plugin's writable scratch directory once. It is only reachable when the
  // plugin declares data-scope filesystem access; create it eagerly in that case so the
  // first ctx.files.write_text into it succeeds.
  plugin->data_dir = platform::ResolveAppDirectory(platform::UserDirectoryKind::Data, "microide") /
                     "plugins" / plugin->id;
  if (plugin->capabilities.fs_read == FsAccess::kProjectAndData ||
      plugin->capabilities.fs_write == FsAccess::kProjectAndData) {
    std::error_code data_dir_error;
    std::filesystem::create_directories(plugin->data_dir, data_dir_error);
  }

  plugin->setup_ref = ExtractFunctionRef(plugin->state, table_index, "setup", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }
  plugin->on_project_open_ref =
      ExtractFunctionRef(plugin->state, table_index, "on_project_open", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }
  plugin->on_project_close_ref =
      ExtractFunctionRef(plugin->state, table_index, "on_project_close", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }
  plugin->on_buffer_open_ref =
      ExtractFunctionRef(plugin->state, table_index, "on_buffer_open", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }
  plugin->on_buffer_save_ref =
      ExtractFunctionRef(plugin->state, table_index, "on_buffer_save", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }
  plugin->shutdown_ref =
      ExtractFunctionRef(plugin->state, table_index, "shutdown", plugin->root, error_message);
  if (error_message != nullptr && !error_message->empty()) {
    lua_pop(plugin->state, 1);
    return false;
  }

  lua_pop(plugin->state, 1);
  return true;
}

}  // namespace microide::plugin::lifecycle_load_interop

#endif
