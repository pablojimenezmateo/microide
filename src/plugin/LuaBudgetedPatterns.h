#pragma once

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>

namespace microide::plugin {

// Replaces `state`'s string.find / match / gmatch / gsub with Lua's own matcher
// made to honour the plugin call budget (see the .cpp): a pattern match is one
// call into C, which the per-call watchdog's count hook never interrupts.
void InstallBudgetedPatternFunctions(lua_State* state);

}  // namespace microide::plugin
#endif
