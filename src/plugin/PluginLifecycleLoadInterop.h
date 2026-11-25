#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lifecycle_load_interop {

inline constexpr std::size_t kMaxProcessAllowEntries = 4096;
inline constexpr std::size_t kMaxProcessAllowItemBytes = 64u * 1024;
inline constexpr std::size_t kMaxProcessAllowAggregateBytes = 8u * 1024 * 1024;

[[nodiscard]] bool ProcessAllowlistEntryAccepted(std::string_view entry,
                                                 std::size_t accepted_bytes);

#if MICROIDE_HAS_LUA_PLUGINS
bool InitializeState(runtime_types::PluginInstance* plugin,
                     lua_CFunction open_microide,
                     std::string* error_message);

bool LoadPluginDescriptor(runtime_types::PluginInstance* plugin, std::string* error_message);
#endif

}
