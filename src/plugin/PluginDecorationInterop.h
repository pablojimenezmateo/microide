#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::decoration_interop {

#if MICROIDE_HAS_LUA_PLUGINS
// Parse a `ctx.decorations.set(path, table)` payload and publish it for `plugin_id`.
// `table_index` is the Lua stack index of the decoration table whose optional
// keys are `text_styles`, `gutter_marks`, `inline_text`, and `code_lenses`.
bool PublishDecorations(lua_State* state,
                        std::string_view plugin_id,
                        const std::filesystem::path& current_project_root,
                        std::string_view raw_path,
                        int table_index,
                        const PluginHost::Callbacks& callbacks,
                        std::string* error_message);

bool ClearDecorations(std::string_view plugin_id,
                      const std::optional<std::filesystem::path>& path,
                      const PluginHost::Callbacks& callbacks,
                      std::string* error_message);
#endif

}  // namespace microide::plugin::decoration_interop
