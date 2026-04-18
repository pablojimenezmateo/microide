# Plugin Platform Expansion Plan

Reviewed on 2026-04-18.

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
- one host-rendered plugin sidebar mode
- diagnostics publication and clearing
- hover providers
- runtime syntax contributions
- project-relative file helpers
- active-buffer metadata
- synchronous argv-based process execution

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
- the current sidebar contribution path is one special-case surface, not a general view system
- plugin process execution is synchronous
- there is no generic background task runner for plugins
- there is no settings, keybinding, output, task, test, SCM, or AI registry
- host-owned process, app-directory, and file-watching services exist, but native watcher coverage
  and higher-level plugin registries are still incomplete

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

This remains the first structural prerequisite.

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

The current sidebar contribution model is too special-case.

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

Deliverables:

- narrower `WorkspaceShell`
- keep host-owned action, menu, and helper code in dedicated modules such as the shipped
  `WorkspaceActionTypes*`, `WorkspaceActionRequests*`, `WorkspaceMenuRegistry*`,
  `WorkspaceLayout*`, `WorkspaceShellRenderFrame*`, `WorkspaceShellRenderChrome*`,
  `WorkspaceShellRenderTextInput*`,
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

Deliverables:

- general view containers
- menus and context menus
- keybinding registry
- settings registry
- compact status and notification surfaces
- built-ins re-registered as contributions with stable IDs
- user-level enable, disable, hide, and reorder support

This phase is where `microide` becomes genuinely user-extensible instead of just scriptable.

### Phase 3. Build the language, task, and diagnostics platform

Deliverables:

- async formatter and linter pipeline
- save participants
- completion, hover, code action, and decoration registries
- LSP manager
- DAP manager
- test controller model
- output channels
- tool download and cache manager

This phase unlocks the bulk of the practical non-AI plugin demand:

- Prettier
- ESLint
- Code Runner
- Live Server
- Path Intellisense
- YAML
- most language-stack prerequisites

### Phase 4. Build SCM, review, and advanced provider surfaces

Deliverables:

- SCM provider registry
- blame and code annotation providers
- virtual document support
- inline diff comments and review threads
- auth provider model
- secret storage

This phase is the floor for strong GitLens-like and GitHub-review-like extensions without turning
the editor into a browser host.

### Phase 5. Build the AI platform

Deliverables:

- model provider registry
- inline completion
- inline explain, edit, and fix actions
- sidebar chat threads with history
- multiple provider and agent selection
- external agents over ACP or ACP-like protocol
- MCP tool integration
- tool permissions and profiles
- bounded context collection, summarization, and cancellation

Recommendation:

- build this natively in the host
- do not try to support Copilot and Copilot Chat by emulating VS Code's AI APIs
- target the workflows, not the vendor-specific extension contract

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
