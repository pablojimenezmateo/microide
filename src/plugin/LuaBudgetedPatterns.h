#pragma once

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>

namespace microide::plugin {

void InstallBudgetedPatternFunctions(lua_State* state);

}
#endif
