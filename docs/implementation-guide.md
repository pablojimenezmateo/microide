# MicroIDE Product Guide

Reviewed on 2026-04-25.

This file captures the durable product direction for the current C++/SDL3 codebase.
Keep it shorter than `docs/active-work.md`.

## Scope

MicroIDE is a compact single-window desktop editor/IDE shell built in C++ with SDL3.
It preserves the IDE-oriented workflows from the older Micro fork without carrying over the TUI
rendering model.

When tradeoffs conflict after correctness, prefer this order:

1. speed
2. low CPU usage
3. low memory footprint

After that, optimize for low input latency, simple architecture, and dense keyboard-first
workflows.

## Product Shape

The shell is intentionally compact:

- a custom in-window menu bar
- project tabs above file tabs
- a persistent left sidebar with tree, search, chat, and source-control modes
- a central editor workspace with nested splits
- a docked terminal-and-command pane
- centered overlays for file finding and commit picking

This is an editor-first product. Built-in tools should support editing rather than compete with
the editor for space.

## Current Capability Baseline

The current SDL shell already includes:

- multi-project tabs with app-level workspace restore
- normal editor tabs, compare tabs, and merge tabs
- nested shared-buffer splits inside editor tabs
- a shared decorated text-grid render path across editor, compare, and merge surfaces, so row
  fills, syntax runs, and underline semantics do not degrade just because a file or diff is large
- checkpointed syntax-highlight state in `TextViewport`, so random jumps in large files reuse
  coarse line-state checkpoints instead of replaying highlight state from file start
- shared UTF-8 and line-ending utilities across editor, renderer, terminal, and workspace text
  helpers, so text decoding, serialization, and boundary handling follow one host contract
- a filesystem tree with `.gitignore` support, git markers, and create/rename/delete flows
- host-owned platform services for app directories, trash or recycle-bin behavior, external URL or
  reveal-path actions, and bundled runtime asset discovery across Linux, macOS, and Windows
- tree mutations preserve affected editor, compare, and merge state across rename/delete workflows
- a file finder overlay and an async project-search sidebar
- literal-by-default project search with regex, case-mode, hidden-file controls, and capped-result feedback
- literal replace-in-project tied to literal search mode
- a standalone project-search benchmark utility for repeatable larger-repo timing runs
- a git sidebar for working-tree changes, conflicts, outgoing branch files, bulk stage-all, and confirmed discard-all flows
- editor copy-with-context via the Edit menu and right-click editor popup, formatting clipboard text as `relative/path:line` or `relative/path:start-end` plus the selected text
- editor git blame shadow text for tracked on-disk files, including saved but uncommitted content, kept asynchronous and viewport-scoped behind a dedicated project service with caret-local inline annotations and hover commit details
- drag reordering for project tabs, file tabs, and terminal tabs, constrained to reinsert within the existing strip instead of spawning detached windows
- terminal copy-with-context from the terminal-tab context menu, formatting clipboard text as the last submitted command plus its rendered output and falling back to the invoked command while an alternate-screen app owns the terminal
- compare flows against `HEAD`, arbitrary commits, and base-branch outgoing files
- a PTY-backed terminal panel with tabs, scrollback, selection, clipboard paste shortcuts, alternate-screen support, application cursor-key mode, origin mode, autowrap control, bracketed paste mode, basic device/cursor query replies, and common ANSI scroll-region handling
- project-local editor preferences, colorscheme persistence, and session restore
- runtime syntax highlighting from an in-tree generated snapshot of the old syntax assets plus runtime-loaded plugin syntax contributions
- a host-owned plugin runtime service for plugin lifecycle, syntax-asset reload bookkeeping,
  asset watching, and plugin output logging
- repo-owned Lua dogfood plugins for ESLint diagnostics and host-owned LLM chat or inline completion, exercising the same narrow host APIs exposed to user plugins
- a host-owned chat pane with conversation rail, provider/model selector, multiline draft composer, markdown transcript rendering, and project-tab chat status summaries
- ghost-text inline completion with accept/dismiss, backed by the AI provider bridge
- MCP tool invocations with per-agent permission levels, session-scoped approvals, and persisted transcript events
- a native `microide_provider_bridge` binary for direct stdio-backed provider communication
- shared shell render primitives for cards, tooltips, text fields, buttons, list rows, strip tabs,
  and common chrome glyphs across prompts, overlays, sidebar, panel, and editor-empty states
- an optional `SDL3_ttf` backend with a debug-text fallback

Current implementation status and active priorities are tracked in `docs/active-work.md`.

## Durable Product Decisions

The authoritative product thesis — including in-scope capabilities, priority order, and non-goals — lives in `openspec/specs/product-vision/spec.md`. The decisions below are the operational corollaries that govern day-to-day implementation choices.

- startup uses the launch working directory as the initial project root
- automatic project-root detection is out of scope
- the menu bar stays custom and in-window; native OS menu integration is out of scope
- the tree stays simple and technical: chevrons and text cues, not pictorial file icons
- project tabs own the full workspace state below them
- delete actions must use the OS trash or recycle-bin flow and fail clearly if unsupported
- the sidebar stays persistent across tab switches
- a terminal tab should be open by default for loaded projects
- colorscheme and editor preferences remain project-local
- compare, merge, search, git, terminal, chat, and inline completion workflows are built-in product features, not plugins
- manual Lua plugins may extend the shell through narrow host-owned APIs such as commands, sidebars, file/process helpers, diagnostics publication, hover providers, and host-loaded syntax data, but they do not replace built-in editing, search, git, compare, merge, terminal, chat, or inline completion workflows
- plugin dogfooding should continue to favor small repo-owned examples over widening the host API speculatively; add new plugin-facing seams only when a real plugin needs them

## Explicit Non-Goals

These are out of scope unless deliberately added as a separate phase:

- full debugger UI beyond first-pass start/stop and output-channel plumbing
- plugin marketplaces, remote install flows, and Micro-plugin compatibility
- cloud or collaboration features
- account systems and sync
- native GitHub-style review dashboards
- recent-project and recent-file surfaces
- soft wrap
- native OS menu bar integration

AI, chat, and inline completion are **in scope** as built-in, host-owned workflows. See
`openspec/specs/ai-workflows/spec.md` for the durable contract.

Broad LSP coverage is not an implicit requirement of the current shell; only validated workflows
should stay in scope.

## Architecture Map

The codebase is organized by responsibility:

- `src/app`: SDL bootstrap, app lifetime, event loop, and startup tracing
- `src/platform`: app directories, runtime asset paths, host integration, file watching, and
  subprocess-facing OS seams
- `src/workspace`: shell facade, workspace state models, actions, menus, prompts, persistence,
  and UI routing
- `src/project`: file indexing, ignore handling, project search, git services, and file operations
- `src/editor`: text viewport, layout, syntax state, and editor rendering support
- `src/compare`: side-by-side diff and merge models
- `src/terminal`: PTY session and terminal screen state
- `src/render`: themes and text-renderer backends

`WorkspaceShell` is still the app-facing facade, but its core workspace state now lives under a
dedicated `WorkspaceContext`, and project, tab, prompt, menu, interaction, and text-input models
live in dedicated workspace headers instead of being defined inline on the shell. The shell no
longer keeps the old active-project reference-alias member block. Project catalog, persistence,
lifecycle, dirty-prompt, menu, command-prompt, diff-tab, compare-interaction, path-mutation,
action-context, action dispatch, tab, key-input, text-input, and mouse coordination now bind
through explicit context-or-state plus callback dependencies rather than taking `WorkspaceShell&`,
and both production and test-only friend access on the shell are gone. Top-level action
enablement now runs through `WorkspaceActionAvailability`, top-level SDL event routing runs
through `WorkspaceEventDispatcher`, scheduled wake handling runs through `WorkspaceWakeController`,
and a dedicated `WorkspaceShell::Bootstrapper` owns the shell's action, render, and event
composition. The shell's `Render` or `RenderPrepared` entry points now delegate the ordered frame
composition path to `WorkspaceRootView`, which composes dedicated active-surface, chrome, sidebar,
overlay, panel, menu, and prompt views. Shell tests use the `MICROIDE_TESTING`-gated public
`WorkspaceShell::TestAccess` API from `workspace/WorkspaceShellTestAccess.h`. The 2026-04-29
`comprehensive-tech-debt-cleanup` change locked the workspace into a service-oriented model:
coordinators consume narrow service interfaces (`EditorTabService`, `ProjectCatalogService`,
`PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`,
`PluginRuntimeService`, `PersistenceService`) instead of `WorkspaceShell&`, render functions
consume view-model structs built by `RenderViewModelBuilder`, and the architectural-lint test in
`tests/ArchitectureInvariantsTests.cpp` enforces the shell-size, no-friend, no-shell-in-coordinator,
no-throwing-numeric-parse, and render-view-model-only invariants on every `ctest` run. New plugin
runtime, project, terminal, compare, and rendering work should continue to move into narrower
subsystems and services rather than accrete more logic on the shell or in one file.

Within `src/editor`, `TextViewport` still owns the byte-oriented text model and viewport behavior,
but file I/O now routes through the shared text-file helper and undo or redo now stores changed
line ranges plus view state instead of whole-buffer snapshots.

## External Tool Boundary

Two current implementation details matter for future work:

- project search now uses a built-in PCRE2-backed engine
- git integration currently shells out to the system `git`

Those dependencies should stay behind `src/project/*` service boundaries so the UI does not care
whether the backend is external or built in.

## Documentation Map

- `openspec/specs/product-vision/spec.md`: authoritative product thesis, in-scope capabilities, priority order, and non-goals
- `openspec/specs/diff-merge-editor/spec.md`: durable behavioral contract for compare and merge tabs
- `openspec/specs/ai-workflows/spec.md`: durable contract for chat, inline completion, MCP tools, and provider bridges
- `openspec/specs/performance-budgets/spec.md`: durable latency, CPU, and measurement policy
- `openspec/specs/host-platform-support/spec.md`: durable supported-host contract for Linux,
  macOS, and Windows host services
- `docs/active-work.md`: shipped baseline, active priorities, and accepted scope cuts
- `AGENTS.md`: repo-level engineering policy, best practices, and iteration loop
- `docs/host-platform-audit.md`: current host-service seam map and the remaining POSIX-only gaps
- `docs/host-platform-bringup.md`: local build, launch, and focused validation flow for supported hosts
- `docs/plugin-runtime-research.md`: plugin architecture notes and external references
- `docs/known-tech-debt.md`: concrete open debt that still matters after recent refactors
- `docs/macos-support-plan.md`: host-platform plan for bringing `microide` to macOS
- `docs/performance-findings.md`: shipped performance work worth preserving
- `docs/startup-tracing.md`: how to measure startup work
- `docs/runtime-profiling.md`: runtime and redraw profiling workflow
