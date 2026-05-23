# Plugin Platform Expansion Plan

Reviewed on 2026-04-20.

This document supersedes the earlier compatibility-first framing.
The top VS Code Marketplace extensions remain a useful demand signal, but they are no longer the
target contract. The target is a rich, fast, `microide`-native plugin platform that keeps the host
small, keeps rendering host-owned, and does not depend on GPU-specific design choices.

Scope:

- use the top-installed VS Code extensions as evidence of the workflows users expect
- define what `microide` would need to change to support those workflows natively
- define what is required for Copilot-style inline and sidebar AI with multiple providers or agents
- incorporate useful lessons from Zed's extension and AI architecture
- account for future macOS and Windows support
- preserve the `AGENTS.md` priority order: correctness first, then speed, CPU, memory, and clarity

Explicitly out of scope for this plan:

- notebook support
- remote or container execution platforms
- broad VSIX or VS Code API compatibility
- file icon themes and icon packs
- GPU-driven rendering changes

## Bottom Line

`microide` does not need a huge “become VS Code” overhaul to become deeply extensible.
It does need a substantial host refactor, but the right refactor is narrower and cleaner:

1. turn built-in features into host-owned registries and services
2. split plugin surfaces into declarative contributions, lightweight script hooks, and
   process-backed tool adapters
3. add a first-class language, task, SCM, and AI platform under those registries
4. let plugins override or hide built-in contributions without exposing coordinator internals

The right north star is not “run top VS Code extensions unchanged”.
The right north star is “support the same important workflows with a smaller, faster, more explicit
host”.

Zed is useful here because it proves several good ideas:

- keep UI ownership in the host
- treat extensions as structured contributions, not arbitrary host mutation
- use capability-scoped permissions
- connect external tools and agents over protocols instead of embedding every integration

Zed's GPU stack and Rust-to-Wasm requirement are not the important parts to copy.

## What The Top VS Code Plugins Still Tell Us

The top Marketplace set is still the best quick signal for what a serious editor needs to expose.
The ranking changes continuously, but the demand clusters are stable.

| Family | Example top extensions | What users are really asking for | In scope |
| --- | --- | --- | --- |
| Formatter and diagnostics workflows | Prettier, ESLint, Code Spell Checker, Error Lens | save hooks, formatter providers, diagnostics, code actions, decorations, output | Yes |
| Small workflow automation | Live Server, Code Runner, Path Intellisense, Markdown All in One | commands, tasks, file watching, process launch, completions, editor actions | Yes |
| Language and debug platform | Python, Pylance, Python Debugger, C/C++, C#, Java Pack, PowerShell, YAML | LSP, DAP, testing, semantic tokens, tasks, toolchain discovery | Yes |
| SCM and review workflows | GitLens, GitHub Pull Requests | annotations, blame, virtual docs, diff comments, auth, secret storage, richer SCM model | Yes, later |
| AI workflows | GitHub Copilot, Copilot Chat, IntelliCode | inline completion, inline edit or explain, sidebar chat, tools, history, multiple providers | Yes |
| Visual asset packs | Material Icon Theme, vscode-icons | icon pipeline, image asset loading, icon association | No |
| Notebook and remote platform | Jupyter, Dev Containers, Remote SSH, Remote Explorer | notebooks, kernels, remote host placement, remote FS and terminal routing | No |

Important implication:

- supporting the top language stacks does not require VS Code compatibility
- it requires a serious LSP, DAP, task, testing, and SCM platform
- supporting Copilot-class workflows does not require Copilot extension compatibility
- it requires a serious native AI platform

## Current `microide` Baseline

Shipped today:

- manual Lua plugin loading from user and project directories
- plugin commands
- a unified host-rendered sidebar view registry spanning built-in Tree, Search, Problems, and Git
  views plus plugin sidebar providers
- diagnostics publication and clearing
- hover providers
- runtime syntax contributions
- project-relative file helpers
- active-buffer metadata
- synchronous argv-based process execution
- a host-owned plugin runtime service for plugin lifecycle, runtime syntax reload bookkeeping,
  plugin asset watching, and plugin output channels

Concrete local references:

- `src/plugin/PluginHost.h`
- `src/plugin/PluginHost.cpp`
- `src/workspace/WorkspaceShellPlugins.cpp`
- `dev-docs/project/active-work.md`
- `dev-docs/plugins/plugin-runtime-research.md`
- `dev-docs/archive/production-tech-debt-review.md`

Important current constraints:

- plugins do not get `WorkspaceShell`
- rendering remains host-owned
- sidebar switching now routes through a unified host view registry, but contributions are still
  limited to one host-owned left-sidebar container without hide, reorder, or override policy
- plugin process execution is synchronous
- there is no generic background task runner for plugins
- there is no settings, keybinding, output, task, test, SCM, or AI registry
- host-owned process, task, app-directory, persistence, output-channel, and file-watching
  services now form the Phase 1 service layer, but higher-level registries are still incomplete
  Non-Linux watcher parity is deferred until target-host build and runtime validation are
  available; snapshot fallback remains the current safe baseline away from Linux.

## What Zed Gets Right

Zed's extension system is much narrower than VS Code's, and that is exactly why it is relevant.

Official Zed docs and source-adjacent posts show that:

- extensions are manifest-based repositories with `extension.toml`
- procedural extension code is compiled to WebAssembly
- extensions primarily provide languages, debuggers, themes, snippets, MCP servers, and agents
- capabilities such as `process:exec`, downloads, and package installation are permission-scoped
- extensions often act as adapters for external LSP, DAP, MCP, and agent processes
- AI UI is host-owned while external tools and agents connect over protocols such as MCP and ACP

The transferable lessons for `microide` are below.

### 1. Host-owned UI beats arbitrary UI hooks

Zed's extension API stays intentionally constrained. Their own writeup explicitly notes that the API
has been limited and does not expose arbitrary panel creation or unconstrained host mutation.

That matches `microide`'s needs well:

- plugins should contribute data, actions, providers, tasks, and structured requests
- the host should continue to render sidebars, editor overlays, diff views, terminal surfaces, and
  chat surfaces
- plugins should not draw pixels, manage raw SDL resources, or reach through coordinators

### 2. Extensions are best when they wire protocols into the host

Zed uses extensions heavily as adapters:

- language extensions define languages and return language-server launch commands
- debugger extensions return DAP launch commands and debug scenarios
- MCP server extensions return commands for tool servers
- agent integrations connect external agents through ACP

That is a very strong fit for `microide`.
The editor does not need plugins to implement every feature in-process.
It needs plugins to declare or configure external protocol participants that the host can manage.

### 3. Capability-scoped permissions are the right security and performance boundary

Zed exposes capabilities such as process execution and downloads through explicit permission grants.
That idea should be adopted.

For `microide`, plugin permissions should be explicit for:

- process execution
- network downloads
- filesystem writes outside plugin work dirs or the current project
- tool registration that spawns background servers
- AI tool execution

This also creates a better performance story because the host can meter and inspect expensive work.

### 4. Installed code and writable work state should be separated

Zed separates installed extension contents from per-extension work data.
That is a strong idea for `microide` too.

`microide` should distinguish:

- installed plugin contents
- plugin work/cache directories
- downloaded tool binaries
- per-project plugin state

This keeps plugin packaging cleaner and simplifies updates, cleanup, and permission policy.

### 5. External AI agents are better behind a protocol than bespoke one-off integrations

Zed's AI stack is not just “call one model API”.
It combines:

- multiple LLM providers
- sidebar thread history
- external agents
- MCP tools
- per-profile tool selection and permissions

The important idea is not ACP specifically.
The important idea is that the editor owns the chat and editing experience while agents stay
replaceable.

For `microide`, this strongly suggests:

- host-owned inline completion and chat UI
- pluggable model providers
- pluggable external agents through a stable protocol
- pluggable tool servers through MCP

## What Not To Copy From Zed

Several Zed choices are interesting but not good first moves for `microide`.

### 1. Do not copy the Rust plus Wasm requirement yet

Zed's Wasm runtime is technically attractive, but it is not the best immediate move here.

Pros:

- isolation
- explicit ABI
- easier capability enforcement
- better crash containment than arbitrary native plugin code

Cons for `microide` right now:

- large implementation cost while the host surface is still moving
- much higher plugin-authoring friction than Lua for local user extensions
- another major runtime and packaging problem before LSP, DAP, tasks, and AI even exist

Recommendation:

- keep Lua as the lightweight local authoring path
- keep heavy integrations out of process
- revisit Wasm later only if extension trust or crash isolation becomes a real blocker

### 2. Do not let GPU questions drive the plugin plan

Zed's GPU acceleration and GPUI work are orthogonal to the plugin architecture choices that matter
here.

`microide` should not change rendering strategy for this plan.
The useful borrowable ideas are:

- host-owned UI
- structured contributions
- protocol-backed tools and agents
- capability gates

### 3. Do not add icon-theme infrastructure

This is both a product and dependency choice.

`dev-docs/project/implementation-guide.md` already says the tree should stay simple and technical rather than
pictorial. Adding icon packs would:

- widen rendering and asset-loading surface area
- encourage an SDL image pipeline the product does not need
- add weight without improving core editing, search, git, compare, merge, or terminal workflows

File and icon themes should stay out of scope.

## Recommended `microide` Plugin Model

The cleanest path is a three-lane extension model.

### Lane 1. Declarative contributions

Manifest-only contributions loaded by the host without running code.

Examples:

- commands and menus
- keybindings
- settings schema and defaults
- sidebar and panel declarations
- syntax and language metadata
- theme data
- formatter, linter, LSP, DAP, test, MCP, and agent declarations
- per-platform tool targets and download metadata

Why this matters:

- zero code execution at startup for many plugins
- cheap indexing, listing, enable or disable, and validation
- stable data contract that built-ins can also use

### Lane 2. Lightweight Lua providers

For local user customization and small extensions that need logic but not a separate runtime
process.

Examples:

- commands
- hover providers
- diagnostics adapters
- view data providers
- save participants
- simple code actions
- small AI prompt transforms or chat participants

Rules:

- no raw UI drawing
- no direct `WorkspaceShell` access
- no synchronous long-running work on the UI path
- capabilities must still be explicit

### Lane 3. Process-backed tool adapters

For the heavy work that should not live in the UI process at all.

Examples:

- LSP servers
- DAP servers
- formatter and linter processes
- task runners
- live preview servers
- MCP servers
- external AI agents

Rules:

- host owns process lifecycle
- host owns cancellation, logs, restart policy, and error presentation
- plugins declare how to launch the tool and how to map it into host services
- protocols stay standard where possible: LSP, DAP, MCP, and preferably ACP for agents

## Let Plugins Change Built-ins Without Exposing Internals

The user goal here is important:

- current `microide` features should remain available
- plugins should still be able to hide, reorder, replace, or supersede some of them

The right way to do that is not to expose coordinator internals.
The right way is to make built-ins use the same registries as plugins.

Recommended policy:

- built-in views register with stable IDs just like plugin views
- built-in commands register with stable IDs just like plugin commands
- built-in SCM, diagnostics, tasks, AI participants, and language adapters register as providers
- user settings can disable or hide contributions by ID
- plugins can supersede host contributions only where the host explicitly marks a slot as
  overridable

Examples:

- Tree, Search, Git, Problems, and future Chat should all be view-container contributions
- a plugin can add a new sidebar view, hide a built-in view, or request that a built-in view move
  position
- a plugin can contribute a formatter or diagnostics provider for a language
- a plugin can contribute an alternate SCM annotation provider or blame provider

Non-goal:

- plugins do not replace the editor widget, diff renderer, merge renderer, or terminal renderer
- plugins do not mutate host state objects directly

## Architecture Changes Required

### 1. Split `WorkspaceShell` pressure before broadening plugins

This was the first structural prerequisite.
The current codebase now satisfies it well enough to treat Phase 1 as complete and move on to
contribution-model work.

Before widening the plugin API, `microide` should continue the work already called out in
`dev-docs/archive/production-tech-debt-review.md`:

- narrow `WorkspaceShell`
- keep the shipped helper seams cohesive across `WorkspaceLayout*`, `WorkspaceTerminalSelection*`,
  `WorkspaceTextSearch*`, `WorkspaceCommandParsing*`, `WorkspacePathUtils*`,
  `WorkspaceProjectPresentation*`, `WorkspaceGitSidebarPresentation*`, and
  `WorkspaceProjectSearchPresentation*` instead of reintroducing a shared helper bucket
- move command handling behind registries and per-domain executors
- reduce shell ownership of sidebar and surface state

This is necessary not because plugins need direct shell access, but because the host needs clear
service seams to expose.

### 2. Add a real host service layer

The current plugin API is callback-shaped.
It needs to grow into named host services.

Minimum service set:

- document and buffer service
- workspace and project service
- filesystem and URI service
- process and task service
- file-watching service
- output and logging service
- command, menu, and keybinding registries
- settings registry and persistence service
- permission and capability service
- plugin install and work-directory service

### 3. Replace the single plugin sidebar with a general contribution system

The current sidebar contribution model has started to move onto the right seam, but it is still
too limited.

Already landed:

- built-in Tree, Search, Problems, and Git views plus plugin sidebar providers now share one
  host-owned sidebar view registry, menu path, command parser, completion path, and project-scoped
  active view ID
- project switching now restores the active sidebar view through stable contribution IDs instead of
  a plugin-only sidebar slot

Needed next:

- multiple host-owned view containers
- tree, list, and flat table view models
- per-item actions and context menus
- refresh and invalidation paths
- visibility, ordering, and user customization

This is the foundation for:

- richer plugin sidebars
- task and output views
- SCM and test views
- chat and tool views

### 4. Build the tool-backed developer platform

This is the platform the top extensions actually depend on.

Deliverables:

- async task runner with cancellation
- save participants
- formatter registry
- diagnostics registry
- code action registry
- completion registry
- LSP client manager
- DAP client manager
- test controller model
- output channels
- tool download and cache manager

This is the real base for Python, C/C++, C#, Java, PowerShell, YAML, Prettier, ESLint, and
similar workflows.

### 5. Build SCM and review surfaces as provider models

GitLens-class workflows are not “just another hover”.

Needed platform pieces:

- SCM provider registry
- blame and annotation providers
- virtual documents
- diff comment threads
- authentication providers
- secret storage

This is enough for strong native SCM and review extensions without a general webview system.

### 6. Build AI as a host platform, not a plugin afterthought

AI needs its own first-class services.

Minimum AI platform:

- model provider registry
- inline completion provider API
- inline transform or explain actions
- sidebar chat threads
- persisted conversation history
- multiple providers and multiple external agents
- tool registry with permissions
- MCP integration
- bounded context collection and summarization

Important direction:

- chat UI remains host-owned
- providers and agents are replaceable
- external agents should use ACP if it fits cleanly, or an ACP-shaped protocol if direct adoption
  proves awkward
- tool servers should use MCP instead of bespoke tool integrations where possible

## macOS And Windows Affect The Plan Now

This is not a later packaging problem.
It changes the host service design immediately.

### Current local code already shows portability pressure

Concrete local examples:

- `src/terminal/TerminalSession.cpp` is POSIX-only and explicitly reports that terminal support is
  only available on POSIX hosts
- `src/platform/Subprocess.cpp` still uses `fork`, `execvp`, pipes, and `waitpid` on POSIX hosts
  and returns “not implemented” on unsupported platforms
- `src/platform/FileWatcher.cpp` now provides Linux `inotify` wakeups, but other hosts still fall
  back to snapshot polling
- `src/project/FileOperationService.cpp` implements Linux and macOS trash flows, but returns “not
  implemented” on unsupported platforms such as Windows

### What this means for the plugin plan

The plugin surface should not hardcode POSIX assumptions.
`microide` needs platform-neutral services for:

- process launch
- environment inheritance and overrides
- path normalization and config directories
- file watching
- trash or recycle-bin behavior
- shell and terminal integration
- secret storage
- browser or URL opening

Plugins should talk to those services, not to platform details.

### Tool packaging must be target-aware from the start

Borrowing from Zed's target-specific agent and tool declarations is the right move.

Plugin manifests should be able to declare per-target binaries such as:

- `darwin-aarch64`
- `darwin-x86_64`
- `linux-x86_64`
- `linux-aarch64`
- `windows-x86_64`
- later, `windows-aarch64`

Each target should support:

- download URL
- checksum
- executable path
- default arguments
- environment overrides

This matters for:

- language servers
- debug adapters
- MCP servers
- external AI agents
- helper tools such as formatters or linters when the user does not already have them installed

## Performance And Anti-Bloat Rules

This is the part that must remain non-negotiable.

- never run plugin logic on the UI thread when it can block
- activation must be lazy and event-driven
- parse manifests before running code
- treat Lua hooks as small control code, not as a place for indexing or long-lived computation
- put LSP, DAP, formatters, linters, test runners, MCP servers, and AI agents out of process
- bound decorations, overlays, and view refresh rates
- require cancellation for search, diagnostics, language work, and AI context collection
- keep rendering host-owned and declarative
- do not add a Node runtime, webview platform, or icon pipeline just to chase extension parity
- do not poll by default; prefer file watching, debounced activation, and explicit invalidation
- keep startup cheap by deferring extension activation until a command, file type, or view needs it
- continue measuring with `dev-docs/performance/startup-tracing.md` and `dev-docs/performance/runtime-profiling.md`

## Revised Phases

This plan now ends at Phase 5.
There are no Phases 6, 7, or 8 in scope.

Implementation audit on 2026-04-21:

| Phase | Actual state | Why |
| --- | --- | --- |
| Phase 1 | Complete | The host-service and shell-breakdown refactor is real, widely wired, and covered by existing shell and registry tests |
| Phase 2 | Complete | The shell now persists contribution settings and sidebar policy, resolves keybindings and menus through contributed registries, renders status items in chrome, and wires plugin setting and redraw callbacks end to end |
| Phase 3 | Complete | The host now owns save participants, formatter execution, cache-backed local tool installs, completion and code-action runtime queries, task and test execution, output channels, and first-pass debugger command surfaces, all wired through live shell commands, overlays, sidebars, and bottom-panel state |
| Phase 4 | Complete | The shell now rebuilds SCM, annotation, review, and auth registries from plugin contributions, persists secret storage locally, opens and refreshes virtual documents in live tabs, renders first-pass review markers, and exposes auth state through built-in command and sidebar surfaces |
| Phase 5 | Complete | The host now owns AI request runtime, inline completion request and accept or dismiss flow, sidebar chat, external-agent execution, MCP tool invocation, and shell-visible conversation state with dedicated phase coverage |

### Phase 0. Define the contract

Deliverables:

- plugin manifest schema
- contribution kinds
- capability and permission model
- built-in override policy
- install, cache, and work-directory layout
- per-platform target naming
- decision on ACP adoption for external agents

This phase should produce stable design docs and validation tests before more runtime surface is
added.

### Phase 1. Extract host services and portability seams

Status:

- completed on 2026-04-19
- `WorkspaceShell` no longer directly owns plugin host lifecycle, plugin asset watching, plugin
  output channels, or runtime syntax reload bookkeeping; that work now lives in
  `WorkspacePluginRuntime*`
- follow-up cleanup on 2026-04-19 now keeps the active workspace on the shell in the same
  `ProjectWorkspaceState` container shape used by project-catalog entries, removing duplicated
  project-switch and persistence bookkeeping for tabs, search state, terminals, diagnostics,
  command history, colorscheme, and editor preferences
- follow-up cleanup on 2026-04-20 also keeps the active shell surface as a direct alias of the
  `ProjectSurfaceState` stored in persisted project state, and splits project-scoped sidebar,
  overlay, and panel ownership into dedicated `SidebarState`, `OverlayState`, and `PanelState`
  models, removing duplicated sidebar, overlay, command-prompt, focus, width, height, and
  scroll bookkeeping
- follow-up cleanup on 2026-04-20 also makes the stable sidebar `view_id` the shell's single
  source of truth for the active left-sidebar contribution, so built-in and plugin sidebar
  behavior now resolves through `WorkspaceSidebarRegistry*` instead of duplicating enum mode plus
  view id state on `WorkspaceShell`
- follow-up cleanup on 2026-04-20 also moves sidebar enum and state definitions into dedicated
  `WorkspaceSidebarState*` ownership instead of keeping those models nested inside
  `WorkspaceShell`
- follow-up cleanup on 2026-04-20 also makes action dispatch a top-level
  `WorkspaceActionCoordinator` and routes project, sidebar, search, tab, edit, and global shell
  interaction through a dedicated `WorkspaceActionContext*` facade instead of a nested
  shell-owned action coordinator with broad private access
- follow-up cleanup on 2026-04-20 also promotes the project catalog, persistence, command-prompt,
  and menu coordinators to top-level types, so workspace-session restore or save, project
  activation, command feedback, and menu-surface transitions no longer depend on nested
  `WorkspaceShell::*Coordinator` classes
- follow-up cleanup on 2026-04-20 also promotes key and text input handling to top-level
  `WorkspaceKeyInputCoordinator` and `WorkspaceTextInputCoordinator` types, so keyboard routing,
  composition, and terminal text entry no longer depend on nested shell-owned coordinator classes
- follow-up cleanup on 2026-04-20 also promotes tab management, lifecycle, and dirty-path
  mutation to top-level `WorkspaceTabCoordinator`, `WorkspaceLifecycleCoordinator`, and
  `WorkspacePathMutationCoordinator` types, so tab dirtiness, reopen or save flow, startup or
  shutdown sequencing, and prompt-driven rename or delete handling no longer depend on nested
  `WorkspaceShell::*Coordinator` classes
- follow-up cleanup on 2026-04-20 also promotes dirty-save confirmation, compare or merge
  interaction handling, and chrome or editor or compare or merge or tab or sidebar or panel
  mouse routing to top-level `WorkspaceDirtyPromptCoordinator`,
  `WorkspaceCompareInteractionCoordinator`, and `Workspace*MouseCoordinator` types, so the
  interaction layer no longer depends on nested `WorkspaceShell::*Coordinator` classes
- follow-up cleanup on 2026-04-20 also promotes compare-tab lifecycle and sidebar mode or
  refresh or action handling to top-level `WorkspaceDiffTabCoordinator` and
  `WorkspaceSidebarCoordinator` types, so `WorkspaceShell` no longer carries nested coordinator
  declarations and only exposes friend-based access to top-level coordinators
- follow-up cleanup on 2026-04-20 also keeps transient drag, mouse-selection, and window-focus
  interaction state outside `ProjectSurfaceState`, so project switches clear in-flight gestures
  instead of restoring stale interaction state from another project
- follow-up cleanup on 2026-04-19 also moves menu-bar, anchored-menu, and tree-context popup
  state behind a dedicated `MenuSurfaceState` on the shell instead of flattening that popup state
  into the generic `SurfaceState`
- the next active phase is Phase 2

Deliverables:

- narrower `WorkspaceShell`
- keep host-owned action, menu, and helper code in dedicated modules such as the shipped
  `WorkspaceActionTypes*`, `WorkspaceActionRequests*`, `WorkspaceMenuRegistry*`,
  `WorkspaceKeyInputCoordinator*`, `WorkspaceSidebarCoordinator*`,
  `WorkspaceSidebarMouseCoordinator*`, `WorkspaceShellMouse*`,
  `WorkspacePersistenceCoordinator*`, `WorkspaceShellOverlay*`,
  `WorkspaceShellRedraw*`, `WorkspaceShellInteraction*`, `WorkspaceShellCursor*`,
  `WorkspaceShellEditor*`,
  `WorkspaceProjectCatalogCoordinator*`, `WorkspaceProjectStateCoordinator*`,
  `WorkspaceProjectDialogCoordinator*`,
  `WorkspaceShellHover*`,
  `WorkspaceShellMergeState*`,
  `WorkspacePathMutationCoordinator*`,
  `WorkspaceShellTerminal*`,
  `WorkspaceShellPresentation*`,
  `WorkspaceLayout*`, `WorkspaceShellRenderFrame*`, `WorkspaceShellRenderChrome*`,
  `WorkspaceShellRenderSidebar*`, `WorkspaceShellRenderOverlay*`,
  `WorkspaceShellRenderBottomPanel*`, `WorkspaceShellRenderMenus*`,
  `WorkspaceShellRenderPrompts*`, `WorkspaceShellRenderTextInput*`,
  `WorkspaceTerminalSelection*`, `WorkspaceTextSearch*`, `WorkspaceCommandParsing*`,
  `WorkspacePathUtils*`, `WorkspaceProjectPresentation*`, `WorkspaceGitSidebarPresentation*`,
  and `WorkspaceProjectSearchPresentation*` seams instead of reintroducing
  `WorkspaceShellShared.*`
- process service
- task executor with cancellation
- filesystem and watcher service
- config, state, and cache directory service
- output and logging channels

This is the main structural precondition for everything else.

### Phase 2. Build the contribution and override model

Status: completed on 2026-04-21.

Shipped:

- left-sidebar built-ins and plugin providers now share stable view IDs through one
  `WorkspaceSidebarRegistry*` path for menu wiring, command parsing or completion, and
  project-scoped active view persistence (started 2026-04-19)
- `WorkspaceKeybindingRegistry*` — named keybinding specs with stable IDs, global / editor /
  sidebar / terminal context awareness, `ParseKeyChord` / `FormatKeyChord` round-trip utilities,
  `ResolveKeybindings` merging built-ins with plugin contributions, user-disable override support
- `WorkspaceSettingsRegistry*` — typed setting specs (bool, int, float, string, enum) with
  per-scope defaults; `AllSettingInfos` merges built-ins with plugin-declared settings; parse and
  serialize helpers for round-tripping values
- `WorkspaceStatusRegistry*` — `ResolveStatusItems` resolves and sorts plugin-contributed compact
  status items by alignment and priority
- `WorkspaceMenuRegistry*` extended with `ContributedMenuItems` and `ParseMenuId` so plugin menu
  entries are queryable alongside the static built-in menu specs
- `WorkspaceSidebarRegistry*` extended with `SidebarViewPolicy`, `OrderedSidebarViews`, and
  `EffectiveSidebarViewPolicy` for hide, reorder, and user-policy support
- `PluginHost` extended with four new Lua contribution tables: `ctx.settings` (declare / get),
  `ctx.menus` (add), `ctx.keybindings` (add), `ctx.status` (add / update); C++ accessors and
  `UpdateStatusItem` are available to workspace coordinators; `Callbacks` gains `get_setting`
  and `request_status_redraw`
- `PersistedUserConfigState` gains `settings` and `disabled_keybinding_ids`;
  `PersistedProjectConfigState` gains `settings` and `sidebar_policies`; all new fields
  round-trip through the existing line-based config serialisation
- `WorkspaceKeyInputCoordinator` now resolves runtime keybindings instead of using a hardcoded
  shortcut table, including plugin command dispatch and user-disabled bindings
- `PersistenceCoordinator` now restores and saves contribution-backed user settings,
  disabled keybinding IDs, project settings, and sidebar policies without split-brain copies
- `WorkspaceMenuCoordinator` and the shell menu path now merge built-in menus with
  `ContributedMenuItems(...)`, and sidebar view surfacing now consistently uses
  `OrderedSidebarViews(...)`
- `WorkspaceShell` now wires `PluginHost::Callbacks::get_setting` and
  `request_status_redraw`, and compact status items render on the breadcrumb/header row with
  hover tooltips
- full test coverage in `tests/ContributionRegistryTests.cpp`
- host-level regression coverage in `tests/PluginHostTests.cpp` and
  `tests/WorkspaceShellPluginTests.cpp`

This phase is where `microide` becomes genuinely user-extensible instead of just scriptable.

### Phase 3. Build the language, task, and diagnostics platform

Status: completed on 2026-04-21.

Shipped:

- `platform/AsyncSubprocess` with bidirectional pipes and poll-based I/O
- `util/JsonValue` recursive JSON parser and serializer
- `WorkspaceLspClient` with JSON-RPC 2.0 plus hover, completion, code-action, and formatting
  requests
- `WorkspaceLspManager` for multi-language server coordination
- `WorkspaceDapManager` for multi-debugger coordination
- `WorkspaceFormatterRegistry` for subprocess-based formatters
- `WorkspaceSaveParticipants` for save-participant registration
- `WorkspaceCompletionRegistry`, `WorkspaceCodeActionRegistry` for language-specific providers
- `WorkspaceTaskRegistry` for runnable tasks
- `WorkspaceToolRegistry` for downloadable tool declarations
- `WorkspaceToolDownloader` for cache management and download orchestration
- `WorkspaceTestController` for test item and result storage
- `WorkspaceOutputChannels` for named tool output logs
- eight new PluginHost Lua APIs: formatters, save_participants, completion, code_actions, tasks,
  tools, debuggers, tests
- `PluginHost` runtime APIs now query completion and code-action providers, run save
  participants, and discover or execute test providers instead of stopping at registration
- the editor save path is now host-owned: save participants run before formatter execution, and
  formatter output is applied back into the live viewport before disk write
- `WorkspaceToolDownloader::Download(...)` now handles local file and cache-backed installs with
  SHA validation instead of returning `nullopt`
- built-in commands, menus, and keybindings now surface completion, code actions, inline
  completion requests, tasks, test discovery or execution, output channels, and debug start or
  stop through the live shell
- `WorkspaceTaskRuntime` now streams task output into named bottom-panel output channels without
  blocking input, and the tests sidebar reflects discovery plus first-pass run results
- completion and code-action providers now surface through host-owned editor overlays instead of
  stopping at registry storage
- focused host and shell coverage now exercises runtime provider invocation and save sequencing in
  `tests/PluginHostTests.cpp` and `tests/WorkspaceShellPluginTests.cpp`

Follow-on work after this phase:

- `WorkspaceLspManager` and `WorkspaceDapManager` still need broader real-server validation and a
  richer session model before advertising full language-server or debugger parity
- the current completion and code-action overlays are intentionally minimal and should stay
  host-owned rather than turning into a command-prompt fork
- tests now have a first-pass sidebar and run flow, but richer tree, gutter, and per-test debug
  UX remain follow-on product work
- tool installation is currently a cache-backed local-file path with SHA validation; remote fetch
  transports should only be added when a real workflow requires them

Tests today:

- `tests/Phase3Tests.cpp` covers JSON parsing and simple registry behavior
- `tests/PluginHostTests.cpp` covers save-participant, completion, code-action, and test-provider
  runtime behavior
- `tests/WorkspaceShellPluginTests.cpp` covers the live save pipeline and related shell wiring
- `tests/Phase5Tests.cpp` covers the command-surface integration for completion, code actions,
  tasks, tests, and output channels

This phase unlocks the bulk of the practical non-AI plugin demand:

- Prettier
- ESLint
- Code Runner
- Live Server
- Path Intellisense
- YAML
- most language-stack prerequisites

### Phase 4. Build SCM, review, and advanced provider surfaces

Status: completed on 2026-04-21.

Shipped:

- `WorkspaceScmRegistry` for source control provider registration
- `WorkspaceAnnotationRegistry` for blame, decoration, and margin annotation providers
- `WorkspaceVirtualDocument` for document generation and virtual views (diffs, merges, etc.)
- `WorkspaceReviewComments` for inline code review comments and discussion threads
- `WorkspaceAuthProvider` for authentication provider management and session tracking
- `WorkspaceSecretStorage` as the host secret-storage abstraction
- four new PluginHost Lua APIs: scm, annotations, auth, plus host-managed virtual documents
  and review comments
- `WorkspaceShell` now rebuilds SCM, annotation, and auth registries from plugin contributions on
  plugin reload and project open
- virtual documents are now openable through the live tab model and refresh open tabs when the
  provider content changes
- `WorkspaceSecretStorage` now persists host-managed secrets in the config directory instead of
  using an in-memory-only map
- the existing Git sidebar now shows first-pass SCM and auth summary lines instead of growing a
  second competing source-control surface
- review comment threads now render as gutter markers in editor and virtual-document views, and
  auth login, refresh, or logout flows surface through built-in commands plus host-owned output
  channels
- dedicated coverage now exists in `tests/WorkspaceShellPluginTests.cpp` and
  `tests/Phase4Tests.cpp`

Follow-on work after this phase:

- the built-in Git compare and stage workflows remain host-owned and are only partially informed
  by provider models; broad provider replacement should happen only with a cohesive source-control
  design
- review comments now render markers, but compose, edit, resolve, and richer thread panels are
  still follow-on work
- auth providers now support login, refresh, and logout commands with persisted host-managed
  sessions, but `WorkspaceSecretStorage` is still not an OS credential backend
- `WorkspaceSecretStorage` is durable local storage, but it is still not an OS credential backend
- virtual documents currently reuse editor tabs in read-only mode; editable virtual documents and
  richer dedicated tab UX are still undefined

Tests today:

- `tests/WorkspaceShellPluginTests.cpp` covers registry rebuild and virtual-document shell wiring
- `tests/Phase4Tests.cpp` covers secret storage persistence, virtual-document change callbacks,
  and review-comment state tracking

### Phase 5. Build the AI platform

Status: completed on 2026-04-21.

Shipped:

- `WorkspaceAiProvider` for language model provider registration (cloud, local, external)
- `WorkspaceInlineCompletion` for inline-completion and inline-action state storage
- `WorkspaceConversation` for conversation-thread storage
- `WorkspaceExternalAgent` for external-agent registry and selection policy
- `WorkspaceMcpTool` for MCP tool metadata and permission policy
- `WorkspaceAiContext` for bounded context collection with limits and smart prioritization
- three new PluginHost Lua APIs: ai_providers, external_agents, mcp_tools, plus host-managed
  conversations and inline completions
- `WorkspaceAiRuntime` now owns request IDs, cancellation, background execution, and shell wake
  delivery for external-agent requests
- the shell now supports sidebar chat, active-conversation tracking, ghost-text inline
  completion with accept or dismiss flow, and built-in `chat`, `inline-complete`, and `mcp`
  commands
- first-pass external-agent execution is wired through stdio subprocesses, and MCP tool
  invocation flows through host-owned permission-checked command surfaces and output channels
- dedicated end-to-end coverage now lives in `tests/Phase5Tests.cpp`

This phase enables building AI-native workflows without emulating VS Code's Copilot APIs.
Instead, we use direct provider integration (Anthropic, OpenAI, local LLMs) with native context
handling and external agent support via standard protocols.

Follow-on work after this phase:

- provider and model selection are still intentionally simple and should stay secondary to the
  request runtime and shell-owned conversation flow
- the external-agent runtime currently ships one stdio transport; HTTP, WebSocket, or ACP should
  only land when a concrete agent integration needs them
- streamed partial updates, richer tool permission prompts, and broader `WorkspaceAiContext`
  collection from diagnostics, SCM, and search remain follow-on work
- conversations are host-owned live state today; durable restore policy should wait until the
  schema and UX are stable

Tests today:

- `tests/Phase5Tests.cpp` covers chat, inline completion, auth-summary-adjacent AI command flow,
  and MCP tool invocation through the live shell

Recommendation (from planning phase, now validated by implementation):

- ✅ Built natively in the host (not as a VS Code extension bridge)
- ✅ Targets workflows, not Copilot API emulation
- ✅ Supports multiple providers and agents via registries
- ✅ Includes tool permission model for safety

## Feasibility Summary

| Target area | Feasibility | Notes |
| --- | --- | --- |
| Prettier, ESLint, Code Runner, Live Server, Path Intellisense, Markdown helpers | High | Needs Phases 1-3, not VS Code compatibility |
| Python, C/C++, C#, Java, PowerShell, YAML | Medium to High | Needs Phases 1-3 and some Phase 4 seams |
| GitLens-class experience | Medium | Needs Phase 4 provider and review model |
| Copilot-style inline completion and sidebar chat | High | Best done with native host AI plus external providers or agents |
| Multiple AI clients and tool ecosystems | High | Use provider registries plus ACP and MCP instead of bespoke one-offs |
| Copilot and Copilot Chat as unchanged VS Code extensions | Out of scope | Wrong contract for this plan |
| Jupyter, Dev Containers, Remote SSH, Remote Explorer | Out of scope | Deliberately removed from the plan |
| Material Icon Theme and vscode-icons | Out of scope | Product direction and dependency cost both say no |

## Recommendation

The best route for `microide` is not “be more like VS Code”.
It is closer to “borrow the good boundaries from Zed, but keep a simpler local-user story”.

Recommended high-level direction:

1. keep Lua for fast local extension authoring
2. make most heavy integrations declarative plus process-backed
3. treat LSP, DAP, SCM, and AI as host platforms
4. let plugins change built-in contributions through registries, not through coordinator access
5. design the service layer so macOS and Windows are first-class from the start

If this is executed well, `microide` can become deeply extensible without:

- shipping a JS runtime
- shipping a webview framework
- shipping an icon pipeline
- changing the renderer to GPU-first assumptions
- pretending to support arbitrary VS Code extensions

That is the path most aligned with the current product shape and the performance rules in
`AGENTS.md`.

## UI Wiring Implementation

The first-pass Phase 2-5 wiring is now landed.
This section is no longer a “to do” list for the plan; it is the implementation shape to keep as
future polish or expansion work continues.

The rule that made the landing stable should remain the rule for follow-on work:

1. runtime and persistence first
2. shell state and command surfaces second
3. the smallest useful host-owned UI third
4. only then broader feature depth

That matters because the shipped Phase 3-5 systems are now real runtimes, but they are still
deliberately thin and should not be re-expanded into coordinator-coupled special cases.

### Phase 2 UI Wiring (landed shape)

- contribution-backed settings, disabled keybindings, and sidebar policies now round-trip through
  the persistence coordinator without split-brain copies
- `WorkspaceKeyInputCoordinator` resolves runtime keybindings by context and dispatches both
  built-in actions and plugin commands
- menu surfacing, sidebar ordering, and compact status items all render on existing host-owned
  chrome instead of inventing separate surfaces
- keep built-in settings mapped to their canonical persisted fields; do not reintroduce duplicate
  generic-setting storage for values such as colorscheme or editor preferences

### Phase 3 UI Wiring (landed shape)

- one save pipeline now owns save participants, formatting, and final disk write
- bottom-panel output channels are the shared host surface for tasks, formatter logs, debugger
  output, auth flows, and MCP tools
- completion and code actions use dedicated editor overlays rather than the command prompt
- task and test commands drive host-owned runtime state first, then reuse the existing bottom
  panel and sidebar surfaces
- keep debugger UX minimal until breakpoint, stack, scope, and variable state exist as real
  host-owned models

### Phase 4 UI Wiring (landed shape)

- the existing Git sidebar remains the single source-control surface; provider state augments it
  instead of creating a second SCM pane
- annotation and review integrations start in host-owned editor lanes and gutter markers before
  any richer thread UI
- virtual documents stay read-only and tab-backed until update and persistence rules are explicit
- auth remains a host request flow first and a widget second; summary state belongs in existing
  chrome or SCM surfaces, not a bespoke account panel

### Phase 5 UI Wiring (landed shape)

- one host-owned AI runtime owns request IDs, cancellation, and agent invocation; state holders
  such as conversations and inline completions do not own transport logic
- inline completion renders as ghost text in the editor with accept or dismiss flow only
- first-pass chat lives in the left sidebar, with dedicated transcript and composer surfaces
- tool invocation and permission decisions stay host-owned and surface through output channels or
  prompt flows instead of ad hoc plugin UI
- keep provider or model selection simple until the runtime, context collection, and transport
  breadth justify more configuration

### Wiring Implementation Workflow

Each phase builds on prior wiring:

- **Phase 2** enables user customization of keybindings, settings, menus, and status items
- **Phase 3** enables language-aware formatting, completion, testing, and debugging
- **Phase 4** enables SCM and code review workflows with auth
- **Phase 5** enables AI-powered features with external agents and tools

The wiring follows the existing pattern throughout the codebase:
1. Registries provide data structures and query methods
2. Runtime services own query, execution, and persistence seams
3. Coordinators query those services at runtime
4. Shell render paths display results
5. Persistence coordinator saves and restores durable state
6. Tests validate end-to-end behavior

### Estimated Scope

- The plan itself is now landed; follow-on work is feature depth, protocol breadth, and UX polish
- The right implementation pattern remains registry tests plus runtime-service tests plus
  shell-level regression tests for each new slice
- Broad new UI should only be added where the host already owns a stable runtime and persistence
  seam

## Sources

Local `microide` references:

- `AGENTS.md`
- `dev-docs/project/active-work.md`
- `dev-docs/project/implementation-guide.md`
- `dev-docs/plugins/plugin-runtime-research.md`
- `dev-docs/archive/production-tech-debt-review.md`
- `src/plugin/PluginHost.h`
- `src/plugin/PluginHost.cpp`
- `src/project/FileOperationService.cpp`
- `src/project/GitCommandUtil.h`
- `src/terminal/TerminalSession.cpp`
- `src/workspace/WorkspaceShellPlugins.cpp`

Current Marketplace pages reviewed on 2026-04-17:

- Python: <https://www.marketplace.visualstudio.com/itemdetails?itemName=ms-python.python>
- Pylance: <https://marketplace.visualstudio.com/items?itemName=ms-python.vscode-pylance>
- Python Debugger: <https://marketplace.visualstudio.com/items?itemName=ms-python.debugpy>
- Jupyter: <https://marketplace.visualstudio.com/items?itemName=ms-toolsai.jupyter>
- C/C++: <https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools>
- Live Server: <https://marketplace.visualstudio.com/items?itemName=ritwickdey.LiveServer>
- GitHub Copilot: <https://marketplace.visualstudio.com/items?itemName=GitHub.copilot>
- GitHub Copilot Chat: <https://marketplace.visualstudio.com/items?itemName=github.copilot-chat>
- Prettier: <https://marketplace.visualstudio.com/items?itemName=esbenp.prettier-vscode>
- IntelliCode: <https://marketplace.visualstudio.com/itemdetails?itemName=VisualStudioExptTeam.vscodeintellicode>
- Docker: <https://marketplace.visualstudio.com/items?itemName=ms-azuretools.vscode-docker>
- ESLint: <https://marketplace.visualstudio.com/items?itemName=dbaeumer.vscode-eslint>
- GitLens: <https://marketplace.visualstudio.com/items?itemName=eamodio.gitlens>
- Extension Pack for Java: <https://marketplace.visualstudio.com/items?itemName=vscjava.vscode-java-pack>
- C#: <https://marketplace.visualstudio.com/items?itemName=ms-dotnettools.csharp>
- Code Runner: <https://marketplace.visualstudio.com/items?itemName=formulahendry.code-runner>
- Dev Containers: <https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers>
- GitHub Pull Requests: <https://marketplace.visualstudio.com/items?itemName=GitHub.vscode-pull-request-github>
- Remote - SSH: <https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-ssh>
- Material Icon Theme: <https://www.marketplace.visualstudio.com/itemdetails?itemName=PKief.material-icon-theme>
- YAML: <https://marketplace.visualstudio.com/itemdetails?itemName=redhat.vscode-yaml>
- Remote Explorer: <https://marketplace.visualstudio.com/items?itemName=ms-vscode.remote-explorer>
- vscode-icons: <https://marketplace.visualstudio.com/items?itemName=vscode-icons-team.vscode-icons>
- PowerShell: <https://marketplace.visualstudio.com/itemdetails?itemName=ms-vscode.PowerShell>
- Path Intellisense: <https://marketplace.visualstudio.com/itemdetails?itemName=christian-kohler.path-intellisense>

Official Zed references:

- Developing Extensions: <https://zed.dev/docs/extensions/developing-extensions>
- Installing Extensions: <https://zed.dev/docs/extensions/installing-extensions>
- Extension Capabilities: <https://zed.dev/docs/extensions/capabilities>
- Language Extensions: <https://zed.dev/docs/extensions/languages>
- Debugger Extensions: <https://zed.dev/docs/extensions/debugger-extensions>
- MCP Server Extensions: <https://zed.dev/docs/extensions/mcp-extensions>
- Model Context Protocol in Zed: <https://zed.dev/docs/ai/mcp>
- AI Overview: <https://zed.dev/docs/ai/overview>
- Agent Panel: <https://zed.dev/docs/ai/agent-panel>
- External Agents: <https://zed.dev/docs/ai/external-agents>
- Agent Client Protocol: <https://zed.dev/acp>
- Life of a Zed Extension: Rust, WIT, Wasm: <https://zed.dev/blog/zed-decoded-extensions>
- Zed for Windows: <https://zed.dev/blog/zed-for-windows-is-here>
- Installation and System Requirements: <https://zed.dev/docs/installation>
