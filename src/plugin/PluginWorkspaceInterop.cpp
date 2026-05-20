#include "plugin/PluginWorkspaceInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <optional>
#include <system_error>

#include "plugin/PluginPathInterop.h"
#include "util/TextFileIO.h"

namespace microide::plugin::workspace_interop {
namespace {

using path_interop::ResolveRuntimePath;

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& current_project_root,
                     const std::filesystem::path& path) {
  lua_createtable(state, 0, 3);
  const std::filesystem::path normalized_path = path.lexically_normal();
  lua_pushstring(state, normalized_path.generic_string().c_str());
  lua_setfield(state, -2, "path");
  lua_pushstring(state, normalized_path.filename().string().c_str());
  lua_setfield(state, -2, "name");

  if (!current_project_root.empty()) {
    const std::filesystem::path relative =
        normalized_path.lexically_relative(current_project_root.lexically_normal());
    const bool starts_with_parent =
        relative.begin() != relative.end() &&
        *relative.begin() == std::filesystem::path("..");
    if (!relative.empty() && !starts_with_parent) {
      lua_pushstring(state, relative.generic_string().c_str());
      lua_setfield(state, -2, "relative_path");
    }
  }
}

}  // namespace

int LuaWorkspaceProjectRoot(lua_State* state, const std::filesystem::path& current_project_root) {
  if (current_project_root.empty()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushstring(state, current_project_root.generic_string().c_str());
  return 1;
}

int LuaWorkspaceOpenFile(lua_State* state,
                         const std::filesystem::path& current_project_root,
                         const PluginHost::Callbacks& callbacks) {
  const char* raw_path = luaL_checkstring(state, 1);
  if (!callbacks.open_file) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  if (path.empty()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const lua_Integer line = luaL_optinteger(state, 2, 0);
  const lua_Integer column = luaL_optinteger(state, 3, 0);
  lua_pushboolean(state, callbacks.open_file(PluginHost::OpenFileRequest{
                              .path = path,
                              .line = line > 0 ? static_cast<std::size_t>(line) : 0,
                              .column = column > 0 ? static_cast<std::size_t>(column) : 0,
                          })
                              ? 1
                              : 0);
  return 1;
}

int LuaWorkspaceActiveBuffer(lua_State* state,
                             const std::filesystem::path& current_project_root,
                             const PluginHost::Callbacks& callbacks) {
  if (!callbacks.active_buffer) {
    lua_pushnil(state);
    return 1;
  }
  const std::optional<PluginHost::ActiveBuffer> active_buffer = callbacks.active_buffer();
  if (!active_buffer.has_value() || active_buffer->path.empty()) {
    lua_pushnil(state);
    return 1;
  }
  PushBufferTable(state, current_project_root, active_buffer->path);
  lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->line));
  lua_setfield(state, -2, "line");
  lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->column));
  lua_setfield(state, -2, "column");
  return 1;
}

int LuaFilesReadText(lua_State* state, const std::filesystem::path& current_project_root) {
  const char* raw_path = luaL_checkstring(state, 1);
  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  const std::optional<std::string> text = util::ReadTextFile(path);
  if (!text.has_value()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushlstring(state, text->c_str(), text->size());
  return 1;
}

int LuaFilesWriteText(lua_State* state, const std::filesystem::path& current_project_root) {
  const char* raw_path = luaL_checkstring(state, 1);
  size_t text_length = 0;
  const char* text = luaL_checklstring(state, 2, &text_length);
  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  lua_pushboolean(state,
                  util::WriteTextFileAtomically(path, std::string_view(text, text_length)) ? 1 : 0);
  return 1;
}

int LuaFilesExists(lua_State* state, const std::filesystem::path& current_project_root) {
  const char* raw_path = luaL_checkstring(state, 1);
  std::error_code error;
  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  lua_pushboolean(state, std::filesystem::exists(path, error) && !error ? 1 : 0);
  return 1;
}

}  // namespace microide::plugin::workspace_interop

#endif
