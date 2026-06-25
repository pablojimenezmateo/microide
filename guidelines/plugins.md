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
- sidebar providers (flat lists or **tree views** — see below)
- language providers: go-to-definition (`ctx.definition.add`), find references
  (`ctx.references.add`), signature help (`ctx.signature_help.add`), and document
  symbols / outline (`ctx.document_symbols.add`) — see below
- diagnostics
- editor decorations via `ctx.decorations.set(path, {...})` / `ctx.decorations.clear(path?)`
  (see below)
- content surfaces (charts / previews) via `ctx.surface.set(id, {...})` /
  `ctx.surface.clear(id)` (see below)
- hover providers
- transient notifications via `microide.notify(level, message)` (host-owned toast
  surface; `level` is info/warning/error)
- settings metadata
- status items (rich: `icon`, `tone`, click `command`, `progress` — see below)
- colour themes via `ctx.themes.add{ id, label, colors = {...} }` (see below)
- file-icon themes via `ctx.file_icons.add{ id, rules = {...} }` (see below)
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

## Tree sidebars (`ctx.sidebar.add`)

A sidebar `snapshot` may return a **flattened tree** instead of a flat list. Each
returned row is still a `{ label, detail, path, line, column }` item and may add:

- `id` — stable node id (string), passed back to `on_toggle`/`on_confirm`
- `depth` — indentation depth (integer ≥ 0; 0 = root)
- `collapsible` — `true` to draw a host-owned disclosure twisty
- `collapsed` — current state of a collapsible row

The host owns all drawing (indentation + twisty) and routing. The **plugin owns
the expand/collapse state**: clicking a twisty (or pressing Right/Left/Space on a
collapsible row) calls the provider's optional `on_toggle(item)` callback, after
which the host re-runs `snapshot` to pull the reshaped visible rows. Plain lists
(no `depth`/`collapsible`) render exactly as before. Confirming a row
(Enter/click on the label) still routes through `on_confirm` / opens `path`.

```lua
ctx.sidebar.add({
  id = "symbols", label = "Symbols",
  snapshot = function() return {
    { id = "g1", label = "Types", depth = 0, collapsible = true, collapsed = false },
    { label = "Widget", detail = "class", depth = 1, path = "src/widget.lua", line = 4 },
  } end,
  on_toggle = function(item) --[[ flip your own state for item.id ]] end,
  on_confirm = function(item) ctx.workspace.open_file(item.path, item.line) end,
})
```

## Language providers (`ctx.definition`, `ctx.references`, `ctx.signature_help`, `ctx.document_symbols`)

Plugin-native language intelligence that the host wires into existing surfaces.
Each registration requires `id`, `language_id`, and a `provide` function; the host
queries every provider matching the active buffer's detected filetype. A non-empty
plugin result short-circuits the built-in LSP path (mirroring completions / code
actions). All line/column values are **1-based**.

- `ctx.definition.add{ id, language_id, provide = function(buffer, position) }` →
  returns an array of `{ path, line, column }`; the host opens the first target.
- `ctx.references.add{ id, language_id, provide = function(buffer, position, include_declaration) }`
  → returns an array of `{ path, line, column }`; rendered into the References
  output channel with file:line context.
- `ctx.signature_help.add{ id, language_id, provide = function(buffer, position) }`
  → returns `{ active_signature, signatures = { { label, documentation,
  active_parameter, parameters = { { label, documentation }, ... } } } }`. The
  `signature-help` command (default `Ctrl+Shift+Space`) renders the active
  signature in a caret-anchored popup; the active parameter and docs form the
  supporting block. The popup self-dismisses on Escape, caret movement, or edit.
- `ctx.document_symbols.add{ id, language_id, provide = function(buffer) }` →
  returns a nested array of `{ name, detail, kind, line, column, children = {...} }`.
  The built-in **Outline** sidebar view (`sidebar-show outline`, default
  `Ctrl+Alt+O`) flattens the tree into an indented, host-drawn list; confirming a
  row navigates the active editor to the symbol (host-bounded depth/count).

All plugin-supplied tables are bounded before allocation; oversize input is
truncated, never trusted. These providers emit data only — the host owns
navigation, the output channel, the signature popup, and the outline surface.

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

## Presentation contributions (`ctx.themes`, `ctx.file_icons`, rich status items)

These Phase D surfaces are **data-only**: the plugin emits values, the host owns
all drawing. They are parsed in `PluginPresentationRegistrationParsers` and synced
into host-owned registries by `WorkspaceShell::RebuildPresentationRegistries()` on
plugin load/reload (parallel to `RebuildPhase4Registries`).

### `ctx.themes.add { id, label, colors = { … } }`

Contributes a colour scheme as a highlight-group style map (the same vocabulary a
`.microide` file uses: `default`, `comment`, `statement`, `type`, `constant`,
`constant.string`, `preproc`, …). Each value is either `"#rrggbb"` /
`"#fg,#bg"` / an ANSI-256 index / a named colour, or a table
`{ fg = …, bg = …, reverse = true }`. The host derives a full `render::Theme` via
`render::BuildThemeFromStyles` (identical contrast-correction to file themes) and
caches it. The scheme is selectable in the colorscheme picker as
`"<plugin>.<id>"`; selecting it routes through `PersistenceCoordinator::ApplyColorscheme`,
which consults plugin themes before built-in/filesystem `.microide` schemes.

### `ctx.file_icons.add { id, rules = { … } }`

Maps files to a built-in gutter-icon shape + colour, drawn in the file-tree
leading slot. Each rule is `{ ext = "csv", icon = "diamond", color = "#80c080" }`
(extension match) or `{ name = "Makefile", icon = "square", color = "#888" }`
(exact filename match). `icon` is a name from the built-in vocabulary
(`dot`/`circle`/`diamond`/`triangle`/`bookmark`/`check`/`dash`/`square`);
unknown names are dropped. Filename rules win over extension rules, and plugin
rules win over the host's built-in extension defaults. No raster icons yet — those
arrive with the Phase E texture cache.

### Rich status items (`ctx.status.add` / `ctx.status.update`)

Status items (rendered in the breadcrumb) accept, in addition to `text`/`tooltip`/
`alignment`/`priority`: `icon` (a gutter-icon name drawn leading), `tone`
(`default`/`info`/`warning`/`error`, tints the background), `command` (a command
run on click via the normal command path), and `progress` (`< 0` = no bar, else a
`[0, 1]` sub-bar). `ctx.status.update(id, { … })` mutates any of `text`, `tooltip`,
`icon`, `tone`, `progress` live.

## Content surfaces (`ctx.surface.set` / `ctx.surface.clear`)

Phase E lets a plugin emit a whole **rendered content surface** — a chart, a REST
response view, a markdown/mermaid preview — that the host draws. As everywhere
else, the plugin emits **data only**; the host owns all drawing, caching, clipping,
and hit-testing. The core never opens a socket or fetches a URL: a markdown/mermaid
plugin runs its CLI in its own sandboxed subprocess (`ctx.process.run_async`, the
`network` capability) and hands the host the resulting **local** bytes.

`ctx.surface.set(id, spec)` is a single atomic publish (it replaces the whole
surface for that id). `spec` carries exactly one body:

- `display_list = { width, height, ops = { … } }` — a flat op buffer the host
  replays. Ops: `{op="rect", x,y,w,h, color}`, `{op="line", x1,y1,x2,y2, color}`,
  `{op="polyline", points={{x,y},…}, color}`, `{op="text", x,y, text, color}`,
  `{op="clip_push", x,y,w,h}` / `{op="clip_pop"}`. Coordinates are content-local.
- `raster = { format = "png"|"rgba8", bytes, width, height }` — encoded image
  (PNG/JPEG, decoded off-thread) or raw RGBA8. Pixels live in the host's
  `SurfaceTextureCache`, keyed by content hash, LRU-evicted by a VRAM budget.

Optional fields: `title`, `preview = "bottom"|"side"` (open it as a panel preview),
`hit_regions = { { x,y,w,h, command } … }` (a click runs `command` through the
normal command path — no bespoke callback), and `anchor = { path, line }` (render
it as an inline inset; see below). `ctx.surface.clear(id)` removes it.

All inputs are bounded before allocation (op/point/text/raster caps); oversize or
malformed input is rejected, never clamped. The display-list op-buffer and the
PNG decoder each have a libFuzzer target (`PluginDisplayListParseFuzz`,
`SurfaceRasterDecodeFuzz`).

**Inline insets are experimental and off by default.** With `plugins.inline_surfaces`
enabled, an anchored surface renders as an inert vertical gap below its anchor
line. The gap is inert (the caret skips it, a selection cannot enter it) and every
row→y lookup goes through the single `editor::EditorRowYLayout` mapping, so with
the setting off the editor geometry is byte-for-byte unchanged. Caret-vertical and
click geometry are **not yet fully gap-aware** under the flag — that, and an
above-line code-lens inset reusing the same gap machinery, are the remaining
follow-ups for this gated path.

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
