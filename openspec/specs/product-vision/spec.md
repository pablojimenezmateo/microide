## Purpose

Define the durable product thesis, priority order, ownership boundaries, and non-goals for MicroIDE.
## Requirements
### Requirement: Product Thesis

MicroIDE SHALL be a native desktop IDE built in C++20 with SDL3, distributed as a single-window
application with no GPU requirement (it MAY use a GPU when available to accelerate rendering, but
MUST remain fully functional on a software renderer), and positioned as a compact, responsive
editor-plus-workflow shell validated through internal methodology-first performance baselines rather
than comparative marketing claims.

#### Scenario: Rendering backend does not require a GPU
- **WHEN** MicroIDE is launched on a host without hardware-accelerated OpenGL, Vulkan, or Metal available to the user session
- **THEN** the application SHALL start, render the shell, and accept input using SDL3's CPU-capable render path (including the `SDL3_ttf` text backend or the debug-text fallback) without requiring GPU features

#### Scenario: Single-window shell shape is preserved
- **WHEN** a user opens any built-in workflow (editor, compare, merge, search, git, terminal)
- **THEN** the workflow SHALL render inside the single MicroIDE window, reusing the menu bar, project tabs, file tabs, persistent sidebar, editor surface, and docked bottom panel, and SHALL NOT spawn detached OS windows or native OS menus

#### Scenario: Detaching a tab launches a separate single-window instance
- **WHEN** a user detaches an editor/compare/merge tab or a project into its own window (via the `detach-tab` / `detach-project` command, the tab context menu, or by dragging a tab out of the strip)
- **THEN** MicroIDE SHALL launch a second MicroIDE process — each remaining a single-window shell — seeded with a handoff of the tab's live state (including unsaved edits), rather than opening a second OS window or floating panel inside one process; and dragging a tab onto another running MicroIDE window SHALL hand the tab to that instance (reattach)

### Requirement: Priority Order For Engineering Tradeoffs

When MicroIDE engineering tradeoffs conflict, the project SHALL resolve them in the following order: (1) correctness, (2) speed, (3) low CPU usage, (4) low memory footprint, (5) architectural clarity, (6) compatibility only when explicitly required.

#### Scenario: Correctness beats compatibility
- **WHEN** a fix for an observably wrong behavior requires breaking a non-contractual compatibility shim
- **THEN** the fix SHALL land and the shim SHALL be removed rather than preserved

#### Scenario: Speed beats memory
- **WHEN** a change can either reduce peak memory by a small amount at the cost of added frame-time work on typing or scrolling, or keep memory flat and preserve the frame-time budget
- **THEN** the change SHALL preserve the frame-time budget

### Requirement: Built-in Workflows Remain Host-Owned

Editor, compare, merge, search, git, terminal, and diagnostics flows SHALL remain built-in, host-owned product features. Plugins MAY contribute data, commands, registries, and structured requests through narrow host APIs, but SHALL NOT replace or reimplement these workflows.

The most validated end-to-end workflow SHALL remain the native diff/merge/git path:
open repository, inspect changes, diff files, resolve merge conflicts, then stage/commit.

#### Scenario: Plugin attempts to replace diff rendering
- **WHEN** a plugin is installed that declares itself as a replacement compare or merge renderer
- **THEN** the host SHALL ignore that replacement and continue to render compare and merge through the built-in pipeline, surfacing plugin contributions only through the existing contribution registries

### Requirement: Durable Non-Goals

MicroIDE SHALL treat the following as out of scope unless deliberately promoted into a dedicated phase: plugin marketplaces and remote install flows, Micro-plugin compatibility, broad plugin security-system hardening work (including plugin sandboxing, per-plugin capability prompts, plugin signing, and plugin marketplace trust), cloud or collaboration features, account systems and sync, recent-project and recent-file surfaces, and native OS menu integration.

Debugger/DAP support was promoted out of this non-goal list on 2026-06-17 and has since shipped to `main` in v2.0.0 (Phases 0–10 complete); see `dev-docs/debugger/dap-integration.md`.

Minimal startup recovery and trust controls for the Git Workstation, specifically `--disable-plugins`, `--safe-mode`, visible plugins-disabled state, and documentation that repo-local plugin code is not loaded by default, SHALL be considered in scope for that workstation and SHALL NOT imply a plugin sandbox or marketplace trust model.

#### Scenario: Feature request falls inside a non-goal
- **WHEN** a proposal requests a feature whose primary capability falls inside the non-goal list
- **THEN** the proposal SHALL be rejected or SHALL explicitly declare itself as promoting a non-goal into its own phase, with an updated product-vision delta in the same change

#### Scenario: Safe-mode request is scoped to workstation trust
- **WHEN** a proposal adds startup flags that disable plugins for recovery or untrusted-repository inspection
- **THEN** the proposal MAY proceed as part of the Git Workstation scope, provided it does not add plugin sandboxing, capability prompts, marketplace, remote install, or project-local plugin loading
