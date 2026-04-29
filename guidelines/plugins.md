# Plugin Guide

Purpose: define the durable plugin-extension rules for `microide`.

## Quick Scan

- Plugins extend the host through narrow, host-owned registries and services.
- Built-in editor, compare, merge, search, git, and terminal workflows remain host-owned.
- Plugins contribute data, commands, providers, or structured actions; they do not take over frame composition or raw shell internals.
- Add new plugin-facing seams only when a real plugin needs them.
- Prefer simple lifecycle and execution rules over speculative plugin infrastructure.

## What Plugins May Contribute

Plugins may contribute capabilities such as:

- commands
- sidebar providers
- diagnostics
- hover providers
- settings metadata
- status items
- formatters
- save participants
- completions
- code actions
- language servers
- tasks
- tools
- debuggers
- test providers
- SCM providers
- annotation providers
- syntax data and related assets that the host knows how to load

The exact mechanism should remain host-owned and registry-first.

## What Must Remain Host-Owned

- frame composition and render order
- window chrome, menus, prompts, overlays, and panel presentation
- core editor model behavior
- built-in compare and merge flows
- built-in search, git, and terminal workflows
- redraw invalidation and performance policy
- workspace persistence and top-level project state ownership
- Lua runtime lifecycle (`lua_State*` create/suspend/destroy and protected-call error capture)

Do not expose `WorkspaceShell` wholesale to plugins.

## API Design Principles

- Prefer stable contribution records over callback soup.
- Model the capability directly. If a plugin needs a sidebar provider, expose sidebar registration; do not hand out a large shell object.
- Keep plugin APIs explicit about ownership, sync or async behavior, and failure cases.
- Favor host validation and host rendering so plugins cannot silently corrupt shell behavior.

## Lifecycle And Reload

- Plugin discovery, runtime ownership, and reload behavior belong to the host.
- The host should own asset watching and reload bookkeeping.
- Plugin lifecycle hooks should stay simple and predictable.
- If reload is supported, define what state is preserved, what state is rebuilt, and what side effects are replayed.

## Sync And Async Policy

- Default to synchronous plugin APIs when the work is short and deterministic.
- Add async execution only for real workloads that would otherwise block typing, redraw, or startup.
- When async work exists, the host should own task lifetime, wakeup, and result delivery.
- Avoid speculative background APIs that widen the contract without current need.

## Stability Expectations

- Narrow contracts are easier to preserve than broad object access.
- If a plugin requirement reveals a bad host boundary, fix the host boundary rather than layering a compatibility escape hatch around it.
- Dogfood new seams with repo-owned plugins before widening them further.
- Keep plugin translation units modular and focused; avoid re-growing monolithic `PluginHost`-style files.
