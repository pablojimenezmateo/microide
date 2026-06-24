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

- commands (may return a string to surface as host command-prompt feedback)
- sidebar providers
- diagnostics
- editor decorations via `ctx.decorations.set(path, {...})` / `ctx.decorations.clear(path?)`
  (see below)
- hover providers
- transient notifications via `microide.notify(level, message)` (host-owned toast
  surface; `level` is info/warning/error)
- settings metadata
- status items
- formatters
- save participants
- completions
- code actions
- language servers via `ctx.lsp.add{ id, command, ... }`. Declare the languages
  served with `language_ids = { "c", "c++", "objective-c" }` (a single
  `language_id` string is also accepted and folds into a one-element list). All
  ids in one registration share a **single** subprocess via `LspManager`
  aliasing, so e.g. one clangd serves C/C++/Objective-C. The host sends each
  file's detected filetype as the LSP `languageId`. Optional `initialization_options`
  and `settings` are JSON-string fields forwarded to the server.
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

## Editor decorations (`ctx.decorations`)

Plugins publish per-file editor decorations that the host renders. Like
diagnostics, a publish **replaces** the owner's full decoration set for that
file, and the host owns all drawing — plugins emit data only. Data flows through
`PluginDecorationStore` (mirrors `DiagnosticsStore`) and is resolved per visible
row by the view-model layer, so thousands of decorations across a file cost only
the visible rows per frame. Lines and columns are **1-based**; `end_col` is
exclusive.

```lua
ctx.decorations.set("src/main.cpp", {
  text_styles = {
    -- recolor / background / underline a byte range on one line
    { line = 10, start_col = 5, end_col = 9, fg = "#c8a26d", underline = true },
    -- whole-line background (e.g. an error-lens band); columns are ignored
    { line = 12, whole_line = true, bg = "#3a1f1f40" },
  },
  gutter_marks = {
    -- icon is one of: dot, circle, diamond, triangle, bookmark, check, dash
    { line = 12, icon = "bookmark", color = "#ffcc00", priority = 5 },
  },
  inline_text = {  -- end-of-line virtual text (Error Lens message, blame)
    { line = 12, text = "  TODO: revisit", color = "#7f8c8d", eol = true },
  },
  code_lenses = {  -- clickable end-of-line affordance dispatching a command
    { line = 1, text = "2 references", command = "refs.show" },
  },
})
ctx.decorations.clear("src/main.cpp")  -- or clear() to drop all of this plugin's
```

Colors are `#rrggbb` or `#rrggbbaa`. `fg` recolors the syntax runs that intersect
the range (bracket-pair coloring, rainbow, semantic tokens); `bg` is a fill
layered above selection so a translucent author color blends; `underline`/`strike`
draw lines; `bold`/`italic` are reserved style flags. The text-style render path
stays allocation-free and materializes no strings.

`inline_text` paints virtual text past a line's last glyph (`eol = true`, the
default — this is the render path that powers Error Lens messages and EOL blame);
its optional `bg` draws a band behind the text, and `color` defaults to the
disabled-text tone. Mid-line virtual text (`eol = false` + `col`) is parsed but
not yet rendered — the v1 inline path is end-of-line only. `code_lenses` paint a
clickable affordance at end of line in the accent color; clicking one dispatches
its `command` through the normal command path (`ctx.commands.add` handlers and
built-in actions alike), so a lens can re-publish to update its own label. Both
kinds render once per logical line on its first visual row; the painted rect and
the click hit-test share one geometry helper (`editor::BuildEolDecorationSegments`)
so a click always lands where the affordance was drawn. See the
`plugins/eol-annotations` dogfood plugin for a worked example of both.

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
- Per-plugin enable/disable is host-owned: disabled plugin ids persist in user config
  (`PersistedUserConfigState::disabled_plugin_ids`) and are applied via
  `PluginHost::SetDisabledPlugins` before each reload. Disabled plugins load only far
  enough to learn their id (so the Settings "Plugins" pane can list and re-enable them),
  then skip setup and lifecycle entirely.

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
