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
- `docs/active-work.md`
- `docs/plugin-runtime-research.md`
- `docs/production-tech-debt-review.md`

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

`docs/implementation-guide.md` already says the tree should stay simple and technical rather than
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
`docs/production-tech-debt-review.md`:

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
- continue measuring with `docs/startup-tracing.md` and `docs/runtime-profiling.md`

## Revised Phases

This plan now ends at Phase 5.
There are no Phases 6, 7, or 8 in scope.

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

Status: substantially complete as of 2026-04-20.

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
- full test coverage in `tests/ContributionRegistryTests.cpp`

Remaining wiring (Phase 2 follow-up, not blocking Phase 3):

- wire `WorkspaceKeybindingRegistry` into `WorkspaceKeyInputCoordinator` runtime dispatch
- wire `WorkspaceSettingsRegistry` + persisted settings into `Callbacks::get_setting`
- wire `WorkspaceStatusRegistry` into the shell render / chrome path
- wire `ContributedMenuItems` into the menu coordinator runtime
- wire `SidebarViewPolicy` from persisted project config into `OrderedSidebarViews`

This phase is where `microide` becomes genuinely user-extensible instead of just scriptable.

### Phase 3. Build the language, task, and diagnostics platform

Status: **Complete** — all core infrastructure is shipped.

Shipped:

- `platform/AsyncSubprocess` with bidirectional pipes and poll-based I/O
- `util/JsonValue` recursive JSON parser and serializer
- `WorkspaceLspClient` with JSON-RPC 2.0 and standard LSP requests/notifications
- `WorkspaceLspManager` for multi-language server coordination
- `WorkspaceDapManager` for multi-debugger coordination
- `WorkspaceFormatterRegistry` for subprocess-based formatters
- `WorkspaceSaveParticipants` for Lua save callbacks
- `WorkspaceCompletionRegistry`, `WorkspaceCodeActionRegistry` for language-specific providers
- `WorkspaceTaskRegistry` for runnable tasks
- `WorkspaceToolRegistry` for downloadable tool declarations
- `WorkspaceToolDownloader` for cache management and download orchestration
- `WorkspaceTestController` for test discovery and execution results
- `WorkspaceOutputChannels` for named tool output logs
- eight new PluginHost Lua APIs: formatters, save_participants, completion, code_actions, tasks,
  tools, debuggers, tests

Remaining wiring (post-Phase 3):

- LSP integration into formatter pipeline and diagnostic display
- task execution and background process handling
- completion and code-action UI hookups
- tool download and installation implementation
- test discovery UI and test result display
- DAP session UI and breakpoint management

This phase unlocks the bulk of the practical non-AI plugin demand:

- Prettier
- ESLint
- Code Runner
- Live Server
- Path Intellisense
- YAML
- most language-stack prerequisites

### Phase 4. Build SCM, review, and advanced provider surfaces

Status: **Complete** — all core infrastructure is shipped.

Shipped:

- `WorkspaceScmRegistry` for source control provider registration
- `WorkspaceAnnotationRegistry` for blame, decoration, and margin annotation providers
- `WorkspaceVirtualDocument` for document generation and virtual views (diffs, merges, etc.)
- `WorkspaceReviewComments` for inline code review comments and discussion threads
- `WorkspaceAuthProvider` for authentication provider management and session tracking
- `WorkspaceSecretStorage` for secure credential storage
- four new PluginHost Lua APIs: scm, annotations, auth, plus host-managed virtual documents
  and review comments

This phase provides the foundation for strong GitLens-like and GitHub-review-like extensions
without turning the editor into a browser host.

Remaining wiring (post-Phase 4):

- SCM sidebar UI and source control operations
- Annotation gutter and margin rendering
- Virtual document tab and editor support
- Review comment inline display and threading
- Authentication flow UI (login/logout/session management)
- OS credential manager backend integration

### Phase 5. Build the AI platform

Status: **Complete** — all core infrastructure is shipped.

Shipped:

- `WorkspaceAiProvider` for language model provider registration (cloud, local, external)
- `WorkspaceInlineCompletion` for AI-powered inline completions and inline actions
  (explain, edit, fix, refactor, document)
- `WorkspaceConversation` for chat threads with full message history
- `WorkspaceExternalAgent` for external agents over ACP-like protocols (HTTP, stdio, WebSocket)
- `WorkspaceMcpTool` for Model Context Protocol tool management with per-agent permissions
- `WorkspaceAiContext` for bounded context collection with limits and smart prioritization
- three new PluginHost Lua APIs: ai_providers, external_agents, mcp_tools, plus host-managed
  conversations and inline completions

This phase enables building AI-native workflows without emulating VS Code's Copilot APIs.
Instead, we use direct provider integration (Anthropic, OpenAI, local LLMs) with native context
handling and external agent support via standard protocols.

Remaining wiring (post-Phase 5):

- Provider and model selection UI
- Inline hint rendering in editor
- Sidebar chat interface with message input
- External agent protocol handlers (HTTP, stdio, WebSocket)
- MCP client implementation
- Context inclusion and cancellation in requests
- Streaming response handling

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

All core registries and services (Phases 2-5) are now shipped as infrastructure.
The remaining work is wiring them into the actual workspace UI and coordinators.

### Phase 2 UI Wiring (5 coordinators/paths)

1. **KeyInput Coordinator Integration**
   - Replace hardcoded key dispatch in `WorkspaceKeyInputCoordinator` with calls to `WorkspaceKeybindingRegistry::ResolveKeybindings`
   - Context-aware lookup (global/editor/sidebar/terminal) based on current surface
   - Handle user-disabled keybindings via `PersistedUserConfigState::disabled_keybinding_ids`
   - Test: verify plugin keybindings and user disables take effect at runtime

2. **Settings Persistence Integration**
   - Wire `Callbacks::get_setting` to read from `PersistedUserConfigState::settings`
   - Load settings on project open, apply to UI elements (tab size, indent width, etc.)
   - Hook `WorkspaceSettingsRegistry` into persistence coordinator for save/load round-trips
   - Test: verify settings persist across sessions and plugin settings appear in settings UI

3. **Status Bar Rendering**
   - Extend `WorkspaceShellRenderChrome.cpp` to render plugin-contributed status items
   - Call `WorkspaceStatusRegistry::ResolveStatusItems` to get sorted items
   - Include alignment (left/right) and priority in layout calculation
   - Wire `Callbacks::request_status_redraw` to invalidate chrome on status updates
   - Test: verify status items appear in bottom chrome with correct ordering

4. **Menu Coordinator Integration**
   - Extend menu builder in `WorkspaceMenuCoordinator` to include `WorkspaceMenuRegistry::ContributedMenuItems` alongside built-in entries
   - Call during menu bar, anchored menu, and context menu construction
   - Parse menu IDs ("file", "edit", "view", "search") to group contributions correctly
   - Test: verify plugin menu items appear in correct menus at runtime

5. **Sidebar View Policy**
   - Read `PersistedProjectConfigState::sidebar_policies` on project open
   - Apply to `WorkspaceSidebarRegistry::OrderedSidebarViews` to hide/reorder views
   - Persist user hide/reorder choices back to config on changes
   - Test: verify sidebar view visibility and order survive session restart

### Phase 3 UI Wiring (8 coordinators/features)

1. **LSP-Backed Formatting**
   - On buffer save, query `WorkspaceFormatterRegistry::FindFormatter(language_id)`
   - If not found and LSP exists, use `WorkspaceLspManager::RequestFormatting` on `WorkspaceLspClient`
   - Apply text edits and mark buffer dirty, preserve cursor position
   - Show brief status in status bar ("Formatting...") with timeout fallback
   - Test: verify Prettier and ESLint-like formatters format on save

2. **Background Task Execution**
   - Create `WorkspaceBackgroundTaskRunner` wrapper around existing task executor
   - Queue formatter, save-participant, and tool-runner tasks without blocking UI
   - Wire cancellation via `WorkspaceDirtyPromptCoordinator` (don't save if task running)
   - Display task progress in bottom panel or status bar
   - Test: verify formatters and save hooks run non-blocking and can be cancelled

3. **Completion UI**
   - Extend command-prompt coordinator to call `WorkspaceCompletionRegistry::FindProvider` for path completion
   - Query LSP completion if available via `WorkspaceLspClient::RequestCompletion`
   - Display completions in overlay above prompt or in-editor when editing
   - Allow plugin Lua completion hooks via callbacks
   - Test: verify completion works in command prompt and editor

4. **Code Actions Popup**
   - On diagnostic hover or user request, call `WorkspaceCodeActionRegistry::FindProvider` and LSP `RequestCodeAction`
   - Build list of (title, command) pairs from results
   - Show in-editor popup with keyboard and mouse selection
   - Execute selected action via `ActionCoordinator`
   - Test: verify code actions appear for diagnostics and can be executed

5. **Task Runner**
   - Create sidebar view or command-palette interface for `WorkspaceTaskRegistry` tasks
   - Spawn task subprocess via `AsyncSubprocess`, pipe stdout/stderr to output channel
   - Wire into bottom-panel output display with scrollback
   - Allow task stop via SIGTERM/SIGKILL through `WorkspaceBackgroundTaskRunner`
   - Test: verify tasks run, output appears, and can be stopped

6. **Tool Download and Installation**
   - Trigger `WorkspaceToolDownloader::Download` on first use if tool missing
   - Show download progress overlay ("Downloading tool... 45%")
   - Verify SHA256 checksum before marking as ready
   - Cache in project-local or user-local tool directory
   - Test: verify tool download, caching, and checksum verification

7. **Test Controller UI**
   - Add gutter icons for test discovery via `WorkspaceTestController::DiscoverTests`
   - Show test tree in bottom panel or sidebar view
   - Run/debug single test or test suite via `WorkspaceTestController::ExecuteTest`
   - Display pass/fail results with stack traces on failure
   - Test: verify test discovery, execution, and result display

8. **DAP Manager UI**
   - Extend breakpoint gutter rendering to support DAP breakpoints
   - Create simple "Launch Debugger" command or sidebar panel
   - Wire `WorkspaceDapManager::StartSession` on launch request
   - Display call stack, locals, and watch variables in bottom panel
   - Step over/into/out/continue via DAP protocol
   - Test: verify breakpoints, stepping, and variable inspection

### Phase 4 UI Wiring (6 coordinators/features)

1. **SCM Sidebar**
   - Add `WorkspaceScmRegistry` as a sidebar view option alongside Tree/Git/Problems
   - Display provider-specific SCM status (branches, commits, etc.)
   - Wire actions (commit, push, pull) via command dispatch
   - Allow switching between SCM providers if multiple registered
   - Test: verify SCM sidebar appears and provider-specific UI renders

2. **Annotation Gutter and Margin**
   - Query `WorkspaceAnnotationRegistry::FindProviders(language_id)` on buffer open
   - Request annotations (blame info, decoration, margin text) from each provider
   - Render in gutter next to line numbers or in margin area
   - Show provider tooltip on hover over annotation
   - Test: verify annotations appear and tooltips work

3. **Virtual Document Support**
   - Extend tab system to recognize virtual URIs ("virtual://...")
   - Query `WorkspaceVirtualDocument::GetDocument(uri)` to fetch content
   - Open in editor with language detection from `language_id`
   - Render as read-only or editable based on `editable` flag
   - Apply edits via `WorkspaceVirtualDocument::UpdateContent`
   - Test: verify virtual docs open, render, and can be edited if editable

4. **Review Comments UI**
   - In diff/merge tabs, query `WorkspaceReviewComments::GetComments(uri, line)`
   - Render comment threads inline next to changed lines
   - Show comment text, author, timestamp, and resolution state
   - Allow adding/editing/resolving comments via `UpdateCommentState`
   - Display in sidebar view or inline in diff
   - Test: verify review comments appear inline and state changes persist

5. **Auth Provider UI**
   - Create login flow for `WorkspaceAuthProvider::GetProvider(id)`
   - Show "Sign In" prompt in status bar or sidebar for plugins requiring auth
   - Prompt for credentials, call auth provider, store session via `AddSession`
   - Display active account in status bar with logout option
   - Retrieve credentials from `WorkspaceSecretStorage` on session restore
   - Test: verify login flow, session persistence, and logout

6. **OS Credential Manager Backend**
   - Implement macOS Keychain backend for `WorkspaceSecretStorage`
   - Implement Windows Credential Manager backend
   - Implement Linux `pass` or `libsecret` backend
   - Fall back to in-memory storage if backend unavailable
   - Integrate with system login/logout lifecycle
   - Test: verify credentials persist across sessions and are secure

### Phase 5 UI Wiring (7 features)

1. **Provider and Model Selection UI**
   - Create modal or sidebar panel for model selection
   - Populate from `WorkspaceAiProvider::AllModels()` — show (provider_id, model_name) pairs
   - Allow setting default provider/model for different contexts (inline vs chat)
   - Persist selection in project config
   - Test: verify provider/model selection UI and defaults

2. **Inline Completion Rendering**
   - Hook into editor typing path: call `WorkspaceInlineCompletion::GetCompletions(line, column)` after typing pause
   - Render ghost text in lighter color at cursor position
   - Tab to accept, Escape to dismiss, configurable debounce delay
   - Display provider and model name in corner for context
   - Test: verify inline completions render and can be accepted/dismissed

3. **Inline Actions Menu**
   - On inline completion accept, query `WorkspaceInlineCompletion::GetActions(line)`
   - Show (Explain, Edit, Fix, Refactor, Document) actions as quick buttons or menu
   - Apply transformation based on action type, request via appropriate API
   - Test: verify inline actions appear and work correctly

4. **Sidebar Chat Interface**
   - Create new sidebar view for AI chat
   - Display `WorkspaceConversation` messages from `GetAllConversations()`
   - Message input box at bottom with send button
   - Show conversation history with timestamps and model name
   - Create new conversation via "New Chat" button
   - Auto-save conversation on new message via persistence coordinator
   - Test: verify chat displays, persists, and can be sent/received

5. **External Agent Protocol Handlers**
   - Implement HTTP POST handler for agent requests (JSON body with context + prompt)
   - Implement stdio protocol (JSON-RPC 2.0 like LSP) for agent subprocesses
   - Implement WebSocket handler for streaming agent responses
   - Parse agent response format, insert into chat or inline completion
   - Test: verify agents can be invoked and respond correctly

6. **MCP Tool Client**
   - Implement MCP protocol client (JSON-RPC 2.0 for tool discovery and execution)
   - Query `WorkspaceMcpTool::GetAvailableTools(agent_id)` based on agent permissions
   - Check `WorkspaceMcpTool::CheckPermission` before tool execution
   - Prompt user if permission is `PromptRequired`
   - Execute tool and capture output for agent
   - Test: verify tools are discovered, permissions enforced, and execution works

7. **Context and Streaming**
   - Before sending chat/completion request, collect `WorkspaceAiContext` items via `AddItem`/`GetContext`
   - Apply prioritization (current file > selection > diagnostics > git > others)
   - Include in API request payload up to `max_total_bytes` limit
   - Cancel pending requests via `RequestCancel` on user action
   - Stream response chunks into chat message in real-time
   - Test: verify context is collected, prioritized, and streamed correctly

### Wiring Implementation Workflow

Each phase builds on prior wiring:

- **Phase 2** enables user customization of keybindings, settings, menus, and status bar
- **Phase 3** enables language-aware formatting, completion, testing, and debugging
- **Phase 4** enables SCM and code review workflows with auth
- **Phase 5** enables AI-powered features with external agents and tools

The wiring follows the existing pattern throughout the codebase:
1. Registries provide data structures and query methods
2. Coordinators query registries at runtime
3. Shell render paths display results
4. Persistence coordinator saves and restores state
5. Tests validate end-to-end behavior

### Estimated Scope

- ~40-50 coordinator changes/extensions (mostly in existing `Workspace*Coordinator.cpp` files)
- ~30-40 UI render path modifications (mostly in `WorkspaceShellRender*.cpp` files)
- Integration with 18+ existing registries and services
- Full test coverage required for each wiring point

## Sources

Local `microide` references:

- `AGENTS.md`
- `docs/active-work.md`
- `docs/implementation-guide.md`
- `docs/plugin-runtime-research.md`
- `docs/production-tech-debt-review.md`
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
