#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::runtime_api_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int LuaDiagnosticsPublish(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks);

int LuaDiagnosticsClear(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const std::optional<std::filesystem::path>& path,
    const PluginHost::Callbacks& callbacks);

int LuaDecorationsSet(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      const std::filesystem::path& current_project_root,
                      const PluginHost::Callbacks& callbacks);

int LuaDecorationsClear(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const std::optional<std::filesystem::path>& path,
    const PluginHost::Callbacks& callbacks);

int LuaSurfaceSet(lua_State* state,
                  const runtime_types::PluginInstance* plugin,
                  const std::filesystem::path& current_project_root,
                  const PluginHost::Callbacks& callbacks);

int LuaSurfaceClear(lua_State* state,
                    const runtime_types::PluginInstance* plugin,
                    const PluginHost::Callbacks& callbacks);

int LuaSidebarShow(lua_State* state, const PluginHost::Callbacks& callbacks);

// ctx.editor.* : host-owned ranged text edits + caret/selection control. Unlike
// the decoration/surface helpers these never raise — they push (true) on success
// or (false, message) so a plugin can branch — keeping all C++ locals destructed
// before any Lua stack write that could OOM.
int LuaEditorApplyEdits(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::filesystem::path& current_project_root,
                        const PluginHost::Callbacks& callbacks);

int LuaEditorSetCursor(lua_State* state,
                       const runtime_types::PluginInstance* plugin,
                       const std::filesystem::path& current_project_root,
                       const PluginHost::Callbacks& callbacks);

int LuaEditorSetSelection(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks);
#endif

}  // namespace microide::plugin::runtime_api_interop
