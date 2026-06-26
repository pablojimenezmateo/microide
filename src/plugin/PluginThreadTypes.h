#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace microide::plugin {

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
  // Declared plugin settings pre-resolved to their current values. Unknown keys
  // resolve to "absent" on the worker (callers gate on declared settings).
  std::vector<std::pair<std::string, std::string>> settings;
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
