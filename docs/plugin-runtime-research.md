# Plugin Runtime Research

Reviewed on 2026-04-15.

Scope:

- current `microide` status relevant to plugins
- plugin architecture patterns in `rxi/lite` and `micro-editor/micro`
- a proposed direction for a manual-install Lua plugin system in `microide`

Phase 0 status:

- implemented on 2026-04-15 in the host codebase
- added `src/workspace/WorkspaceCommandRegistry.*` for built-in command metadata and lookup
- added `src/workspace/WorkspaceSidebarRegistry.*` for built-in sidebar tool metadata, parsing, and menu items
- kept runtime behavior stable; this pass does not add plugin loading yet

Constraints for this pass:

- plugins are installed manually
- plugin manager and marketplace flows are out of scope
- Lua is the scripting language
- the host should stay small and avoid unnecessary runtime bloat
- built-in project features should remain built-in; plugins should extend the shell rather than replace it

## Why This Document Exists

The recent cleanup passes have been moving the codebase toward smaller ownership boundaries:

- `061d5cb` `chore: cleanup`
- `1b2b5b2` `Centralize workspace layout constants`
- `14e9af6` `Consolidate workspace window presentation state`
- `f3e5564` `Split merge rendering from compare rendering`
- `9ccf53a` `Use argv-based git command execution`

That direction is useful because a plugin runtime only stays maintainable if the host already has narrow subsystem seams.

At the same time, the current product docs still treat plugin runtimes as intentionally out of scope:

- `docs/implementation-guide.md`
- `docs/roadmap.md`
- `docs/todo.md`

This note does not change current shipped scope by itself. It records what would need to change if plugin support becomes an intentional project phase.

## External Sources Studied

### lite

- README: <https://github.com/rxi/lite/blob/master/README.md>
- Usage and plugin model: <https://github.com/rxi/lite/blob/master/doc/usage.md>
- Core boot and plugin loading: <https://github.com/rxi/lite/blob/master/data/core/init.lua>
- Command registry: <https://github.com/rxi/lite/blob/master/data/core/command.lua>
- Keymap registry: <https://github.com/rxi/lite/blob/master/data/core/keymap.lua>
- Syntax registry: <https://github.com/rxi/lite/blob/master/data/core/syntax.lua>
- Root view and shared host UI objects: <https://github.com/rxi/lite/blob/master/data/core/rootview.lua>
- Status view: <https://github.com/rxi/lite/blob/master/data/core/statusview.lua>
- Example syntax plugin: <https://github.com/rxi/lite/blob/master/data/plugins/language_python.lua>

### micro

- README: <https://github.com/micro-editor/micro/blob/master/README.md>
- Plugin help: <https://github.com/micro-editor/micro/blob/master/runtime/help/plugins.md>
- Lua bridge registration: <https://github.com/micro-editor/micro/blob/master/cmd/micro/initlua.go>
- Startup and plugin lifecycle order: <https://github.com/micro-editor/micro/blob/master/cmd/micro/micro.go>
- Plugin loading and callback dispatch: <https://github.com/micro-editor/micro/blob/master/internal/config/plugin.go>
- Runtime file registration: <https://github.com/micro-editor/micro/blob/master/internal/config/rtfiles.go>
- Command registration from plugins: <https://github.com/micro-editor/micro/blob/master/internal/action/command.go>
- Example linter plugin: <https://github.com/micro-editor/micro/blob/master/runtime/plugins/linter/linter.lua>
- Example status plugin: <https://github.com/micro-editor/micro/blob/master/runtime/plugins/status/status.lua>
- Example diff plugin: <https://github.com/micro-editor/micro/blob/master/runtime/plugins/diff/diff.lua>
- Example comment plugin: <https://github.com/micro-editor/micro/blob/master/runtime/plugins/comment/comment.lua>

## Current `microide` Status

### Good News

The codebase already has several subsystems that are useful raw material for plugins:

- the working tree was clean when this review was done
- `src/project/*` already holds reusable project services instead of burying everything in rendering code.
- `src/project/FileOperationService.*` is already a standalone file mutation boundary.
- `src/project/ProjectSearchService.*` and `src/workspace/WorkspaceProjectSearchRuntime.*` already show an async worker plus SDL wake-event pattern.
- `src/project/GitBlameService.*` already shows a background cache plus viewport-scoped result model.
- `src/editor/TextViewport.*` is a real editor model, not just a rendering stub.
- `src/editor/RuntimeSyntaxRegistry.*` already isolates syntax detection and tokenization from the editor widget.
- `src/render/Theme.*` centralizes visual colors instead of scattering them through the renderer.

In short: the project does not need a plugin system to invent services from scratch. It needs a clean way to expose the right ones.

### Current Friction

The same code review debt that makes ordinary feature work expensive would also make a plugin API unstable if exposed too early.

The biggest blockers are:

1. `WorkspaceShell` still owns too much.

   - `docs/production-tech-debt-review.md` already calls this out as the highest-impact remaining debt.
   - `src/workspace/WorkspaceShell.h` still carries sidebar state, action metadata, prompts, projects, tabs, terminal state, hover state, and render helpers in one class.

2. Sidebar modes are hardcoded product state, not registered contributions.

   - `SidebarMode` is an enum with `Tree`, `Search`, and `Git` only.
   - `WorkspaceShellRender.cpp`, `WorkspaceShellSidebar.cpp`, `WorkspaceSidebarCoordinator.cpp`, and `WorkspaceShellActions.cpp` all branch directly on that enum.

3. Commands are still shell-centric.

   - `ActionSpec` lives in `WorkspaceShell`.
   - `WorkspaceShellActions.cpp` resolves command names and sidebar tool names through fixed switches.
   - `WorkspaceShellCommand.cpp` only completes known built-in commands and hardcoded sidebar tool names.

4. Hover and inline overlays are specialized, not generic.

   - Editor hover cards today are blame-specific.
   - `EditorViewRenderer` knows about `EditorBlameOverlay`.
   - `WorkspaceShellBlame.cpp` builds both inline blame text and the blame popup.

5. Syntax is runtime-executed but statically compiled into the binary.

   - `SyntaxHighlighter` calls `runtime_syntax::DetectState` and `HighlightLine`.
   - `RuntimeSyntaxRegistry.cpp` builds the registry from generated arrays in `RuntimeSyntaxGenerated.cpp`.
   - `SyntaxTokenKind` is a fixed enum, and `Theme` has fixed syntax color slots.

6. There is no generic plugin task runner.

   - Search and blame each have their own runtime/service path.
   - That is fine for built-ins, but an ESLint plugin needs the same shape without duplicating host logic again.

### Bottom Line

`microide` is in much better shape than a few weeks ago, but it is not ready for "just expose `WorkspaceShell` to Lua". Doing that now would lock in the wrong boundaries.

## What lite Gets Right

### 1. Deployment is intentionally simple

lite treats plugins as normal Lua modules:

- drop a `.lua` file into `data/plugins`
- lite auto-requires every plugin at startup
- no unload model is expected
- user and project modules are just more Lua loaded after plugins

This is exactly the right baseline for `microide` right now. Manual install is enough.

### 2. Extensibility is registry-first in a few important places

lite exposes simple registries rather than a heavy framework:

- `command.add(...)`
- `keymap.add(...)`
- `syntax.add(...)`

Those APIs are small and legible. A plugin adds data or behavior at stable extension points; it does not need a plugin manager to be useful.

### 3. The host stays small by design

lite pushes a lot of policy into Lua:

- shared command definitions
- keybindings
- syntax definitions
- user and project customizations

That keeps the core compact and makes "drop-in local modifications" easy.

### What Not To Copy From lite

lite's simplicity comes with tradeoffs that are a poor fit for `microide`:

- Plugins operate inside broadly shared mutable Lua state.
- Plugins can effectively patch core behavior by touching shared modules directly.
- There is no strong API boundary between stable host services and internals.
- UI extension is mostly "modify existing host objects" rather than "register a contribution model".

That is acceptable in a mostly-Lua editor. It is not a good long-term contract for a C++ host that wants a stable, minimal plugin surface.

## What micro Gets Right

### 1. Plugins are more structured than lite

micro keeps plugins in folders and gives them:

- lifecycle callbacks such as `preinit`, `init`, `postinit`, `deinit`
- event callbacks such as `onBufferOpen`, `onBufferOptionChanged`, `onSave`, `onAnyEvent`
- command and keybinding registration
- runtime file registration for syntax, help, colorschemes, and plugins

This is closer to what `microide` needs because it makes the host responsible for high-value registries.

### 2. Runtime file categories are a good idea

micro's runtime file categories are one of the strongest design ideas in the whole system:

- syntax files
- help files
- colorschemes
- plugin files
- in-memory runtime files

This lets plugins contribute data without inventing custom loaders for every feature.

That matters directly for `microide` because syntax highlighting should be data-driven and loaded by the host, not reimplemented in Lua line by line.

### 3. The linter plugin is a useful reference for diagnostics

micro's built-in `linter` plugin shows a practical pattern:

- register a command
- listen to save events
- run an external tool asynchronously
- parse tool output into host messages
- let the host display those diagnostics

That is directly relevant to the ESLint example for `microide`.

### 4. The status and diff plugins show "host-owned rendering, plugin-owned data"

micro plugins do not generally draw raw pixels or terminal cells themselves.

Instead they:

- register status functions
- set diff bases
- add messages to buffers
- bind commands and actions

That pattern is important. Plugins should supply data and intent. The host should own rendering.

### What Not To Copy From micro

micro's plugin system is powerful, but its exposed surface is much broader than `microide` should start with:

- it exposes a large set of host objects and methods
- it exposes process APIs and shell execution
- it exposes large parts of the Go standard library
- it exposes a wide callback matrix
- it bundles plugin-manager concepts and metadata even when not strictly needed

This is useful in micro, but it is too much API and too much trust surface for a first `microide` pass.

## Comparison Summary

| Area | lite | micro | Best takeaway for `microide` |
| --- | --- | --- | --- |
| Install model | Manual file drop | Folder-based, plugin manager aware | Copy lite's manual-install simplicity |
| Lifecycle | Minimal | Rich callback model | Use a small lifecycle, not a callback explosion |
| Commands/keybindings | Small registries | Host registries and plugin actions | Keep host-owned registries |
| Syntax extension | Very easy, Lua-driven | Runtime file categories | Use host-loaded syntax data, not Lua tokenization |
| Diagnostics | Not a major host model | Buffer messages and async jobs | Build a first-class diagnostics model |
| UI extension | Mutable shared host state | Mostly host-owned UI with plugin hooks | Host should own rendering and layout |
| Trust boundary | Very loose | Broadly trusted plugins | Start with a narrow, curated API |
| Plugin manager | None | Built-in manager | Out of scope for this project |

## Key Design Decision

The right direction for `microide` is:

- lite's deployment model
- micro's host-owned registries
- a much narrower exposed API than micro
- no direct Lua access to `WorkspaceShell`, raw SDL, or arbitrary renderer internals

In one sentence:

> Make plugins simple to install like lite, but make them contribute through explicit host registries like micro.

## Recommended Lua Host Strategy

Because reducing bloat is an explicit goal, the embedding strategy should stay conservative.

Recommended choice:

- embed plain Lua 5.4
- use the Lua C API directly with a thin C++ wrapper layer in `src/plugin/*`
- bind only the specific host facades that are intentionally supported

Recommended non-choices:

- do not expose `WorkspaceShell` directly to Lua
- do not add a heavy C++ binding layer just to avoid writing a small number of hand-made bindings
- do not couple the host to LuaJIT-specific behavior

Practical build recommendation:

- add a focused build option such as `MICROIDE_ENABLE_LUA_PLUGINS`
- resolve Lua the same way the project already resolves optional external libraries: prefer standard package discovery, keep the dependency boundary explicit, and avoid hiding a large third-party framework inside the tree

The important point is not that the bindings are hand-written for their own sake. It is that a small curated API is easier to stabilize than a broad automatic object bridge.

## Proposed Plugin Model For `microide`

### Install Layout

Manual install only.

Recommended search paths:

```text
~/.config/microide/plugins/<plugin-id>/init.lua
<project-root>/.microide/plugins/<plugin-id>/init.lua
```

Optional plugin subdirectories:

```text
<plugin-id>/
  init.lua
  syntax/
  help/
  assets/
  README.md
```

Notes:

- Project-local plugins are important because IDE behavior is often repository-specific.
- A copied folder is enough; no channel metadata or remote fetching should be required.
- Duplicate plugin ids should be treated as an error in v1 instead of adding override rules immediately.

### Load Order

Recommended startup order:

1. Initialize built-in host services.
2. Load global plugins.
3. Load project-local plugins.
4. Activate project-scoped hooks once the project is open.

That keeps the model simple and lets project-local plugins observe the active repository.

### Lifecycle

Do not copy micro's full callback matrix on day 1.

Recommended first-pass lifecycle:

- `setup(host)`: plugin registers commands, sidebar contributions, syntax data, providers
- `on_project_open(project)`
- `on_project_close(project)`
- `on_buffer_open(buffer)`
- `on_buffer_save(buffer)`
- `shutdown()`

This is enough for ESLint, syntax contributions, sidebar tools, and many project workflows.

## What The Host Should Expose

### 1. Command Registry

Plugins should be able to register namespaced commands:

- `eslint.run`
- `eslint.show-problems`
- `llm.ask-selection`

Required host behavior:

- command dispatch by name
- command palette completion
- optional keybinding integration
- optional menu exposure later

This is a strong fit for the existing direction in `docs/production-tech-debt-review.md`, which already recommends moving away from shell-centered action dispatch.

### 2. Sidebar Provider Registry

This is the most important missing host seam.

The current sidebar is a hardcoded union of three built-ins. That should become a registry of providers where built-ins also register through the same path.

Recommended provider model:

- host owns layout, focus, scrolling, hover, and drawing
- provider supplies label, optional toolbar actions, row data, and event handlers
- provider never draws raw SDL directly

Suggested provider capabilities:

- `id`
- `label`
- `priority`
- `snapshot()` returning a typed row model
- `activate()`
- `deactivate()`
- `on_select(index)`
- `on_confirm(index)`
- `on_click(hit)`
- `toolbar_actions`

Supported row kinds should stay intentionally small:

- section header
- item
- empty state
- status row

This is enough to build:

- project tree
- project search
- source control
- problems list
- LLM conversation list or prompt history list

It is not enough for arbitrary custom widget trees, and that is good. The first plugin API should not turn the sidebar into a second rendering engine.

### 3. File and Workspace APIs

Plugins need file access, but not raw reach into shell internals.

Recommended APIs:

- `project.root()`
- `workspace.active_file()`
- `workspace.open_file(path, line, column)`
- `workspace.open_scratch(title, content)`
- `fs.read_text(path)`
- `fs.write_text(path, text)`
- `fs.exists(path)`
- `fs.list_dir(path)`
- `fs.mkdir(path)`
- `fs.rename(old, new)`
- `fs.trash(path)`

Important rule:

- file and process services should stay argv-based and path-based, not shell-string based

That matches the recent cleanup around safer git process execution.

### 4. Process and Background Task APIs

An ESLint plugin needs asynchronous external-tool execution.

Do not expose "run arbitrary shell text" as the main primitive.

Preferred primitive:

```text
process.spawn(argv, {
  cwd = project.root(),
  stdin = optional_text,
  timeout_ms = optional_timeout,
  on_stdout = callback,
  on_stderr = callback,
  on_exit = callback
})
```

Why:

- easier to reason about
- avoids shell quoting surprises
- matches the direction already taken in git execution
- works for linting, formatting, and language tools without needing a plugin manager

`micro` exposes raw shell helpers because it is a terminal editor. `microide` should be stricter.

### 5. Diagnostics and Editor Decorations

This is the second critical missing seam after the sidebar.

Today the editor has:

- text tokens
- selections
- search matches
- blame overlay text
- blame hover popup

It does not have a generic model for:

- underlines
- squiggles
- inline problem markers
- gutter markers
- hover content from arbitrary providers

Recommended host model:

- plugins publish diagnostics to a host-owned diagnostics store
- diagnostics are keyed by owner, file path, and document revision
- renderer draws them
- hover requests are routed through registered hover providers

Suggested core types:

- `Diagnostic`
- `DiagnosticSeverity`
- `DecorationSpan`
- `HoverItem`
- `HoverProvider`
- `DiagnosticsStore`

The existing blame overlay is useful prior art. It should become one consumer of a broader overlay/hover system rather than remaining a special case forever.

### 6. Syntax Contributions

Current syntax support is not plugin-ready because it is compiled into generated arrays.

To support plugin-installed syntax data cleanly:

- keep the existing tokenizer engine
- separate syntax data loading from syntax execution
- allow startup registration of additional syntax definitions from plugin directories

This is closer to micro's runtime-file model than lite's `syntax.add(...)`.

That is the right choice for `microide` because:

- syntax tokenization is performance-sensitive
- syntax should remain host-executed C++, not Lua callbacks per line
- plugin installation is manual, so startup-time syntax loading is acceptable

Recommended near-term compromise:

- keep the current token enum for v1 if the goal is only new filetypes and basic theme coloring
- do not use Lua callbacks for tokenization

Recommended medium-term improvement:

- replace `SyntaxTokenKind` with a small interned style-scope system if richer highlighting becomes important

That would avoid locking the editor into the current fixed set of syntax color buckets forever.

### 7. Theme Additions

Diagnostics require theme support that does not exist yet.

At minimum the theme should grow:

- `diagnostic_error`
- `diagnostic_warning`
- `diagnostic_info`
- `diagnostic_hint`

Possibly also:

- `diagnostic_underline_error`
- `diagnostic_underline_warning`
- `diagnostic_gutter_error`

Without this, ESLint integration will end up reusing unrelated colors such as diff markers, which will look wrong and couple features that should stay separate.

## Recommended Base Project Changes Before The First Serious Plugin

### Required

1. Convert the sidebar from enum-based branching to a provider registry.

   Why:

   - plugins must be able to add new left-pane modes
   - built-ins should dogfood the same interface

   Files most directly affected:

   - `src/workspace/WorkspaceShell.h`
   - `src/workspace/WorkspaceShellRender.cpp`
   - `src/workspace/WorkspaceShellSidebar.cpp`
   - `src/workspace/WorkspaceSidebarCoordinator.cpp`
   - `src/workspace/WorkspaceShellActions.cpp`

2. Move command registration behind a registry independent of `WorkspaceShell::ActionSpec`.

   Why:

   - plugin commands should not require editing a host enum and multiple shell switches

3. Add a generic editor-decoration and hover-provider layer.

   Why:

   - ESLint needs underline ranges and hover text
   - blame should become a provider in the same system

4. Make syntax data runtime-extensible.

   Why:

   - "plugins can change syntax highlight" requires host-loaded syntax contributions

5. Introduce a generic async task/process service.

   Why:

   - search, blame, and future diagnostics should not all invent their own wake-event plumbing

6. Add a narrow plugin host facade instead of exposing `WorkspaceShell`.

   Why:

   - plugins need stability
   - `WorkspaceShell` is still under active debt reduction

### Strongly Recommended Soon After

1. Split `WorkspaceShellShared.*` into cohesive modules.

   This reduces unrelated coupling when registries are added.

2. Add plugin-oriented tests.

   At minimum:

   - sidebar provider registration and switching
   - command registration and completion
   - diagnostics store and hover rendering
   - syntax contribution loading
   - process service callback ordering and cancellation

3. Add a `plugins-reload` developer command.

   Full hot unload is not required. A full plugin-host rebuild is enough.

## What Should Stay Out Of Scope In V1

- plugin manager
- plugin marketplaces
- remote install/update flows
- Micro-plugin compatibility
- arbitrary Lua access to renderer internals
- arbitrary raw SDL drawing from Lua
- per-keystroke Lua syntax tokenization
- full unload safety for every plugin object
- custom window/panel layout engines defined by plugins

The first plugin system should solve concrete editor extension problems, not become a second application framework inside the editor.

## Example: ESLint Plugin

### What It Should Be Able To Do

- detect JavaScript or TypeScript buffers
- run ESLint asynchronously
- underline lint ranges in the editor
- show diagnostic text on hover
- optionally contribute a "Problems" sidebar mode

### Host Capabilities It Needs

- file and buffer access
- process spawning with argv and cwd
- diagnostics publication
- hover integration
- optional sidebar provider registration

### Example Flow

1. Plugin registers a diagnostics owner: `eslint`.
2. Plugin listens to `on_buffer_save(buffer)` or a debounced text-change signal later.
3. Plugin calls something like:

   ```text
   process.spawn(
     ["eslint", "--stdin", "--stdin-filename", buffer.path, "--format", "json"],
     cwd = project.root(),
     stdin = buffer.text()
   )
   ```

4. Plugin parses JSON output.
5. Plugin publishes diagnostics with file path, range, severity, and message.
6. Host redraws:
   - underline in editor
   - hover text through the hover-provider path
   - optional sidebar row entries in a problems view

### Why This Is A Good First Plugin

- it exercises the process API
- it exercises diagnostics
- it exercises hover
- it can optionally exercise the sidebar registry
- it does not require arbitrary custom rendering

If the architecture can support ESLint cleanly, the plugin system is probably on the right track.

## Example: LLM Plugin

An LLM integration can fit the same architecture, but it should not define the architecture.

What it can reasonably use early:

- commands such as `llm.ask-selection`
- scratch buffers for prompt/response output
- sidebar contribution for conversation history or prompts
- file and selection access

What it should not force too early:

- arbitrary sidebar widget trees
- custom rendering loops
- built-in chat product assumptions

This matters because the shell should not grow plugin surface area around the most complex plugin idea first.

## Recommended Directory And Module Layout

If plugin work starts, a new production area is justified:

```text
src/plugin/
  PluginHost.h/.cpp
  PluginRegistry.h/.cpp
  PluginApi.h/.cpp
  LuaEngine.h/.cpp
  PluginManifest.h/.cpp     # optional if a manifest is added later
  PluginProcessService.h/.cpp
```

Likely supporting extractions:

```text
src/workspace/sidebar/
  SidebarRegistry.h/.cpp
  SidebarProvider.h

src/editor/decorations/
  DiagnosticsStore.h/.cpp
  HoverRegistry.h/.cpp
  EditorDecorationModel.h/.cpp

src/editor/syntax/
  SyntaxDefinitionLoader.h/.cpp
```

That separation matters because plugin code should depend on stable, narrow modules, not on the full shell coordinator.

## Recommended API Shape

Rough Lua-side shape:

```lua
local ide = require("microide")

return ide.plugin({
  id = "eslint",

  setup = function(ctx)
    ctx.commands.add("eslint.run", function()
      -- run lint
    end)

    ctx.sidebar.add({
      id = "problems",
      label = "Problems",
      snapshot = function()
        return ctx.diagnostics.snapshot()
      end,
      on_confirm = function(item)
        ctx.workspace.open_file(item.path, item.line, item.column)
      end
    })
  end,

  on_buffer_save = function(ctx, buffer)
    -- lint the file and publish diagnostics
  end
})
```

Important point:

- plugins register contributions and callbacks
- the host owns the actual rendering, persistence, focus, and redraw behavior

## A Small But Important Product Decision

`microide` should not try to copy micro's plugin compatibility.

Why:

- different host language
- different UI model
- different renderer
- different file, buffer, and workspace ownership
- current docs already mark Micro-plugin compatibility as intentionally cut

However, borrowing some data formats is still reasonable.

Good candidate:

- syntax-file compatibility at the data level, if it can be loaded into the C++ syntax engine without dragging in the rest of micro's API model

Bad candidate:

- trying to emulate micro's full callback and object API surface in Lua

## Phased Plan

### Phase 0: Host Cleanup

- finish the next round of `WorkspaceShell` ownership reduction
- extract sidebar registry
- extract command registry

### Phase 0: Landed Host Cleanup

This pass completed the smallest useful version of Phase 0:

- built-in command definitions now live in `src/workspace/WorkspaceCommandRegistry.*`
- built-in sidebar tool definitions now live in `src/workspace/WorkspaceSidebarRegistry.*`
- command completion and command dispatch now read from the command registry instead of shell-local tables
- sidebar command parsing, sidebar menu items, and the sidebar mode label now read from the sidebar registry instead of shell-local switches

This is intentionally narrower than a full provider model. The shell still owns:

- `SidebarMode` as a built-in enum
- mode-specific rendering branches in `WorkspaceShellRender.cpp`
- mode-specific behavior in `WorkspaceSidebarCoordinator.cpp` and related mouse/input code

That remaining work is still the right seam for Phase 2 sidebar providers. Phase 0 only needed to stop burying built-in registrations inside `WorkspaceShell`.

### Phase 1: Core Plugin Infrastructure

- add `src/plugin/*`
- load manual plugins from global and project-local directories
- support `setup`, `on_project_open`, `on_buffer_open`, `on_buffer_save`, `shutdown`
- add `plugins-reload`

### Phase 2: High-Value Contribution Points

- command registration
- sidebar providers
- file/workspace APIs
- process service

### Phase 3: Editor Extensibility

- diagnostics store
- decoration renderer
- hover providers
- theme additions for diagnostics

### Phase 4: Syntax Contributions

- runtime syntax loading from plugin data directories
- startup registration into the syntax engine

### Phase 5: Dogfood With Real Plugins

- ESLint plugin first
- one smaller sidebar plugin second
- LLM integration only after the first two feel clean

## Final Recommendation

The clean path for `microide` is not to "embed Lua and expose the shell".

It is to:

1. make the sidebar and command systems registry-based
2. add a generic diagnostics and hover model
3. make syntax data runtime-extensible
4. expose a small, curated Lua host API over those registries and services
5. keep installation manual and local

If that is done, plugins can:

- add new left-pane modes
- access files safely through host facades
- contribute syntax definitions
- publish diagnostics for editor underlines and hover cards
- run external project tools such as ESLint

without turning the codebase into a second copy of micro's plugin surface or a Lua-patched version of `WorkspaceShell`.
