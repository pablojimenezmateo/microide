#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::surface_interop {

#if MICROIDE_HAS_LUA_PLUGINS

std::uint64_t ComputeRasterContentHash(std::string_view bytes, bool is_rgba, int width,
                                       int height);

bool PublishSurface(lua_State* state,
                    std::string_view plugin_id,
                    const std::filesystem::path& current_project_root,
                    std::string_view surface_id,
                    int spec_index,
                    const PluginHost::Callbacks& callbacks,
                    std::string* error_message);

bool ClearSurface(std::string_view plugin_id,
                  std::string_view surface_id,
                  const PluginHost::Callbacks& callbacks,
                  std::string* error_message);
#endif

}
