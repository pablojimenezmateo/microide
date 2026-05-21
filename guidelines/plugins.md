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
- test providers
- SCM providers
- annotation providers
- syntax data and related assets that the host knows how to load
- **Editor language contract data** via Lua tables merged by
  `WorkspaceLanguageContract` (pairs, comment markers, indent hints, snippet
  definitions). These tables only feed the host contract; they do not register
  commands or render surfaces.

The exact mechanism should remain host-owned and registry-first.

## Editor language contract tables (`ctx.brackets`, `ctx.comments`, `ctx.indents`, `ctx.snippets`)

Plugins register these from `init.lua` using the same `ctx.<surface>.add(...)`
pattern as other contribution modules (`PluginLuaContextInterop.cpp`).
Registration is parsed in `PluginRegistrationParsers.cpp` and stored on
`PluginHost` until teardown on reload.

### `ctx.brackets.add { … }`

Required: `language_id` (string).

Arrays of UTF-8 string pairs `{ open, close }` (each pair is a two-element Lua
array):

- `pairs` → language bracket pairs (matching / folding consumption as implemented)
- `auto_close` → auto-close / skip-over-close pairs
- `surround` → surround-selection pairs

Omitting an array means “no contribution” for that sub-field.

### `ctx.comments.add { … }`

Required: `language_id`.

Optional string fields — any mixture may be present:

- `line` — line-comment leader (e.g. `//`)
- `block_open` / `block_close` — block comment delimiters

### `ctx.indents.add { … }`

Required: `language_id`.

Optional arrays of strings:

- `indent_after_open` — suffix tokens on the **previous** logical line that trigger
  an extra indent unit on newline (host matches trimmed line endings)
- `dedent_on_close` — typed characters that trigger dedent-on-close on
  indent-only lines (see smart-indent implementation)

### `ctx.snippets.add { … }`

Required: `id`, `language_id`, `prefix`, `body` (all strings). Optional `label`.

The host forms the stable snippet id as `<plugin_id>.<id>` for merges and
`editor.snippets.user_disabled` filtering. Snippet **bodies** are stored in the
resolved `LanguageContract`; expansion UI / completion routing is not wired
until the snippet engine work in `openspec/changes/editor-essential-capabilities`
lands.

After changes to these tables, the workspace refreshes `WorkspaceLanguageContract`
and reapplies `LanguageContractView` to open editor tabs (same revision path as
plugin reload).

## What Must Remain Host-Owned

- frame composition and render order
- window chrome, menus, prompts, overlays, and panel presentation
- core editor model behavior
- built-in compare and merge flows
- built-in search, git, and terminal workflows
- redraw invalidation and performance policy
- workspace persistence and top-level project state ownership (routed through `PersistenceService` and the typed record format only)
- Lua runtime lifecycle (`lua_State*` create/suspend/destroy and protected-call error capture); this lives in `plugin/LuaRuntime` and no extension-surface module holds a raw `lua_State*`

Do not expose `WorkspaceShell` wholesale to plugins.

## API Design Principles

- Prefer stable contribution records over callback soup.
- Model the capability directly. If a plugin needs a sidebar provider, expose sidebar registration; do not hand out a large shell object.
- Keep plugin APIs explicit about ownership, sync or async behavior, and failure cases.
- Favor host validation and host rendering so plugins cannot silently corrupt shell behavior.

## Lifecycle And Reload

- Plugin discovery, runtime ownership, and reload behavior belong to the host.
- Plugins load only from `~/.config/microide/plugins/<plugin-id>/init.lua`. Project-local
  `.microide/plugins/` directories and in-tree repo `plugins/` directories are not scanned
  automatically; copy or symlink dogfood examples into the user config directory instead.
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
- Keep plugin translation units modular and focused. The 2026-04-29 cleanup decomposed `PluginHost` into a runtime core plus per-surface modules (`PluginCommandRegistry`, `PluginSidebarRegistry`, `PluginSyntaxRegistry`, `PluginDiagnosticsRegistry`, `PluginHoverRegistry`, `PluginProviderRegistry`, `PluginLifecycle`); do not re-merge those concerns or grow any single `src/plugin/*.cpp` translation unit beyond 800 lines.
