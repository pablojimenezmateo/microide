#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_buffer_project_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void PushProjectTable(lua_State* state, const std::filesystem::path& project_root);

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& path,
                     const std::function<std::optional<std::string>(
                         const std::filesystem::path&)>& relative_path_string);

// Give the buffer table at the stack top the buffer's live text, as of the query
// it is handed to (VS Code's TextDocument): `buffer:text()` (the whole text, the
// lines joined with the document's line ending), `buffer:line(n)` (line n,
// 1-based, without its ending; nil past the end) and `buffer:line_count()`. The
// table keeps `text` alive however long the plugin holds it; nothing is copied
// into Lua until a method is called.
void PushBufferTextMethods(lua_State* state, const std::shared_ptr<const std::string>& text);
#endif

}  // namespace microide::plugin::lua_buffer_project_interop
