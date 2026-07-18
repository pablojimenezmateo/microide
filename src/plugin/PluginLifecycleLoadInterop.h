#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lifecycle_load_interop {

// TD-2026-07-17A-121 byte budgets for `capabilities.process.allow`. Mirrors the shared
// string-array reader (`ReadStringArrayField`) so the allowlist inherits the same
// per-item and aggregate ceilings that other plugin string arrays enforce. Defined
// outside the Lua guard so the predicate is unit-testable on every build.
inline constexpr std::size_t kMaxProcessAllowEntries = 4096;
inline constexpr std::size_t kMaxProcessAllowItemBytes = 64u * 1024;          // 64 KiB / entry
inline constexpr std::size_t kMaxProcessAllowAggregateBytes = 8u * 1024 * 1024;  // 8 MiB total

// True when one process-allowlist entry may be retained: it carries no embedded NUL
// (real executable names never do; the C-string path would silently truncate it), is
// within the per-item byte cap, and fits the remaining aggregate byte budget. An
// over-budget or NUL-bearing entry is dropped rather than emptying the whole allowlist,
// since an empty allowlist means "allow any binary" — dropping the field would widen
// the plugin's process capability, not narrow it.
[[nodiscard]] bool ProcessAllowlistEntryAccepted(std::string_view entry,
                                                 std::size_t accepted_bytes);

#if MICROIDE_HAS_LUA_PLUGINS
bool InitializeState(runtime_types::PluginInstance* plugin,
                     lua_CFunction open_microide,
                     std::string* error_message);

bool LoadPluginDescriptor(runtime_types::PluginInstance* plugin, std::string* error_message);
#endif

}  // namespace microide::plugin::lifecycle_load_interop
