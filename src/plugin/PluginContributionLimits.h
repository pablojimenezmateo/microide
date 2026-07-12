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
inline constexpr std::size_t kMaxPluginContributionsPerKind = 100000;

template <typename Container>
bool ContributionLimitReached(const Container* container, std::string* error_message) {
  if (container != nullptr && container->size() >= kMaxPluginContributionsPerKind) {
    if (error_message != nullptr) {
      *error_message = "plugin contribution limit reached";
    }
    return true;
  }
  return false;
}

}  // namespace microide::plugin
