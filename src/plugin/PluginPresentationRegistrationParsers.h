#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

// Phase D presentation contributions: plugin-supplied colour themes and
// file-icon themes. These are data-only (no runtime callbacks), so the seam is a
// pair of parse-and-append registrations mirroring `RegisterSnippet`. Kept in a
// dedicated translation unit so the existing parser TUs stay under the 800-line
// cap. No function here raises a Lua error: each returns false + an error string
// and lets the thin Lua wrapper raise after all C++ locals have destructed.
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

}  // namespace microide::plugin::presentation_interop
