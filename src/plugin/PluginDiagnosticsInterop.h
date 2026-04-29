#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::diagnostics_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool PublishDiagnostics(lua_State* state,
                        std::string_view plugin_id,
                        const std::filesystem::path& current_project_root,
                        std::string_view raw_path,
                        int diagnostics_index,
                        const PluginHost::Callbacks& callbacks,
                        std::string* error_message);

bool ClearDiagnostics(std::string_view plugin_id,
                      const std::optional<std::filesystem::path>& path,
                      const PluginHost::Callbacks& callbacks,
                      std::string* error_message);
#endif

}  // namespace microide::plugin::diagnostics_interop
