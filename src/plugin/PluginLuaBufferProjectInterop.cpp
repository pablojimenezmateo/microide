#include "plugin/PluginLuaBufferProjectInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginPathInterop.h"

namespace microide::plugin::lua_buffer_project_interop {

void PushProjectTable(lua_State* state, const std::filesystem::path& project_root) {
  lua_createtable(state, 0, 2);
  lua_pushstring(state, project_root.generic_string().c_str());
  lua_setfield(state, -2, "root");
  lua_pushstring(state, path_interop::Basename(project_root).c_str());
  lua_setfield(state, -2, "name");
}

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& path,
                     const std::function<std::optional<std::string>(
                         const std::filesystem::path&)>& relative_path_string) {
  lua_createtable(state, 0, 3);
  const std::filesystem::path normalized_path = path.lexically_normal();
  lua_pushstring(state, normalized_path.generic_string().c_str());
  lua_setfield(state, -2, "path");
  lua_pushstring(state, normalized_path.filename().string().c_str());
  lua_setfield(state, -2, "name");
  if (const std::optional<std::string> relative = relative_path_string(normalized_path);
      relative.has_value()) {
    lua_pushstring(state, relative->c_str());
    lua_setfield(state, -2, "relative_path");
  }
}

}  // namespace microide::plugin::lua_buffer_project_interop

#endif
