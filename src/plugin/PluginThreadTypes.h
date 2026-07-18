#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microide::plugin {

// Plugin-contributed settings pre-resolved to their current values. Shared as an
// immutable block so many per-call snapshots reference the same resolved surface
// (rebuilt only when the host's settings revision changes) instead of each
// re-copying every value.
using ResolvedPluginSettings = std::vector<std::pair<std::string, std::string>>;

// Immutable, point-in-time view of the host state a plugin call may read, captured
// on the UI thread when a job is dispatched and resolved against on the worker so
// read verbs (ctx.workspace.active_buffer / project_root / data_dir,
// ctx.settings.get) never touch live shell state.
struct PluginHostSnapshot {
  struct ActiveBuffer {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    bool present = false;
  };

  std::filesystem::path project_root;
  ActiveBuffer active_buffer;
  // Declared plugin settings pre-resolved to their current values, shared as an
  // immutable block across snapshots. Unknown keys resolve to "absent" on the
  // worker (callers gate on declared settings). Null when no settings are declared.
  std::shared_ptr<const ResolvedPluginSettings> settings;
  // Shell edit generation at capture time; a write verb's mutation is dropped on
  // the main thread if the live buffer has advanced past it.
  std::uint64_t generation = 0;
};

// A closure marshalled from the plugin worker thread back to the UI thread and
// run during the scheduled-wake drain.
//
// Hard rule: a PluginMainThreadAction must never capture a `lua_State*` or any
// Lua registry ref. All Lua-side extraction happens on the worker thread before
// the action is posted, so the action carries only owning, plain result data.
using PluginMainThreadAction = std::function<void()>;

}  // namespace microide::plugin
