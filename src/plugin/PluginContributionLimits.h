#pragma once

#include <cstddef>
#include <string>

namespace microide::plugin {

// Per-kind ceiling on plugin contributions. Registration is setup-only, but a
// plugin's setup() can loop any register verb (`ctx.commands.add(...)`,
// `ctx.completion.add(...)`, ...) without bound; each accepted entry stores a
// host-side C++ struct plus owned strings (and, for provider kinds, a luaL_ref)
// that the Lua per-state 256 MB memory cap does NOT count, so an unbounded loop
// amplifies host RSS far past the intended plugin envelope well within the
// setup-call watchdog. Both register paths — registry_interop (commands, sidebars,
// hovers, menu, keybindings, settings, status) and contribution_interop
// (completions, code-actions, providers, snippets, ...) — enforce this same
// ceiling. Self-correcting: the count is the live container size, which drops when
// the plugin's contributions are torn down on reload/disable.
//
// VALUE DERIVATION (TD-2026-07-17-019, measured 2026-07-24 on perf-runner-v1;
// re-run scenario plugin_keybindings_resolve_at_cap before raising):
// the most expensive per-entry registry rebuild is the keybinding resolve
// (chord parse + hashed conflict index, per plugin reload): committed baseline
// p50 ~12 ms at 16,384 entries, scaling linearly (~75 ms extrapolated at the
// old 100,000 ceiling — and before the hashed conflict index the rescan was
// quadratic, i.e. seconds). 16,384 keeps a worst-legal reload rebuild within a
// user-triggered-action budget, bounds typical per-kind retained host memory to
// single-digit MB (~0.1–0.5 KB/entry measured), and still leaves ~10x headroom
// over the largest real-world contribution counts (snippet packs and command
// sets top out in the low thousands in VSCode's marketplace). The previous
// 100,000 ceiling had no measured basis.
inline constexpr std::size_t kMaxPluginContributionsPerKind = 16384;

// Tighter ceiling for status items: unlike the query-bounded kinds, the status
// vector is fully re-resolved (copy + parse + stable_sort) on the main thread
// on EVERY revision bump — i.e. per ctx.status.update while the status bar is
// visible — so its cap is derived from that refresh budget, not just memory.
//
// VALUE DERIVATION (TD-2026-07-17-019, measured 2026-07-24 on perf-runner-v1;
// re-run scenario plugin_status_items_resolve_at_cap before raising): the
// rebuild costs ~1–2 µs + ~8 allocations per item (string copies dominate) —
// committed baseline p50 ~1.8 ms at 1,024 items, scaling linearly (~160 ms
// measured at the old 100,000 ceiling: ten 60 fps frames of main-thread stall
// per status update). Status updates arrive at animation rates (progress bars,
// spinners), so the budget is a small slice of frame time per rebuild; 1,024
// lands on it while real plugins contribute a handful of items each (VSCode's
// status bar holds dozens total).
inline constexpr std::size_t kMaxPluginStatusItems = 1024;

template <typename Container>
bool ContributionLimitReachedAt(const Container* container,
                                std::size_t cap,
                                std::string* error_message) {
  if (container != nullptr && container->size() >= cap) {
    if (error_message != nullptr) {
      *error_message = "plugin contribution limit reached";
    }
    return true;
  }
  return false;
}

template <typename Container>
bool ContributionLimitReached(const Container* container, std::string* error_message) {
  return ContributionLimitReachedAt(container, kMaxPluginContributionsPerKind, error_message);
}

}  // namespace microide::plugin
