# MicroIDE Product Guide

This file captures the durable product direction for the current C++/SDL3 codebase.
Keep it shorter than `docs/todo.md` and `docs/roadmap.md`.

## Scope

MicroIDE is a compact single-window desktop editor/IDE shell built in C++ with SDL3.
It preserves the IDE-oriented workflows from the older Micro fork without carrying over the TUI
rendering model.

When tradeoffs conflict, prefer this order:

1. startup speed
2. low idle CPU usage
3. low memory footprint

After that, optimize for low input latency, simple architecture, and dense keyboard-first
workflows.

## Product Shape

The shell is intentionally compact:

- a custom in-window menu bar
- project tabs above file tabs
- a persistent left sidebar with tree, search, and source-control modes
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
- a filesystem tree with `.gitignore` support, git markers, and create/rename/delete flows
- a file finder overlay and an async project-search sidebar
- literal-by-default project search with regex, case-mode, and hidden-file controls
- literal replace-in-project tied to literal search mode
- a git sidebar for working-tree changes, conflicts, and outgoing branch files
- compare flows against `HEAD`, arbitrary commits, and base-branch outgoing files
- a PTY-backed terminal panel with tabs, scrollback, selection, and common ANSI handling
- project-local editor preferences, colorscheme persistence, and session restore
- runtime syntax highlighting from an in-tree generated snapshot of the old syntax assets
- an optional `SDL3_ttf` backend with a debug-text fallback

Current implementation status is tracked in `docs/todo.md`.
Forward-looking work is tracked in `docs/roadmap.md`.

## Durable Product Decisions

These decisions should stay fixed unless there is a clear product reason to revisit them:

- startup uses the launch working directory as the initial project root
- automatic project-root detection is out of scope
- the menu bar stays custom and in-window; native OS menu integration is out of scope
- the tree stays simple and technical: chevrons and text cues, not pictorial file icons
- project tabs own the full workspace state below them
- delete actions must use the OS trash or recycle-bin flow and fail clearly if unsupported
- the sidebar stays persistent across tab switches
- a terminal tab should be open by default for loaded projects
- colorscheme and editor preferences remain project-local
- compare, merge, search, git, and terminal workflows are built-in product features, not plugins

## Explicit Non-Goals

These are out of scope unless deliberately added as a separate phase:

- debugging
- plugin runtimes and plugin marketplaces
- cloud or collaboration features
- AI/chat surfaces
- account systems and sync
- native GitHub-style review dashboards

LSP-backed diagnostics are also a separate phase, not an implicit requirement of the current shell.

## Architecture Map

The codebase is organized by responsibility:

- `src/app`: SDL bootstrap, app lifetime, event loop, and startup tracing
- `src/workspace`: shell state, actions, menus, prompts, persistence, and UI routing
- `src/project`: file indexing, ignore handling, project search, git services, and file operations
- `src/editor`: text viewport, layout, syntax state, and editor rendering support
- `src/compare`: side-by-side diff and merge models
- `src/terminal`: PTY session and terminal screen state
- `src/render`: themes and text-renderer backends

`WorkspaceShell` is still the main coordinator, but project, terminal, compare, and rendering work
should continue to move into narrower subsystems rather than accrete more logic in one file.

## External Tool Boundary

Two current implementation details matter for future work:

- project search now uses a built-in PCRE2-backed engine
- git integration currently shells out to the system `git`

Those dependencies should stay behind `src/project/*` service boundaries so the UI does not care
whether the backend is external or built in.

## Documentation Map

- `docs/todo.md`: verified current status and remaining implementation gaps
- `docs/roadmap.md`: forward-looking priorities and feature planning
- `docs/startup-tracing.md`: how to measure startup work
- `docs/best-coding-practices.md`: contributor-side engineering guardrails
