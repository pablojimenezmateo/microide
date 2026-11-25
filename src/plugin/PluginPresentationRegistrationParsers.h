#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::presentation_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool RegisterTheme(lua_State* state,
                   std::string_view plugin_id,
                   std::vector<PluginHost::ContributedTheme>* themes,
                   std::string* error_message);

bool RegisterFileIconTheme(lua_State* state,
                           std::string_view plugin_id,
                           std::vector<PluginHost::ContributedFileIconTheme>* themes,
                           std::string* error_message);
#endif

}
