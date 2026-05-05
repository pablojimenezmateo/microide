## Purpose

Define the durable product thesis, priority order, ownership boundaries, and non-goals for MicroIDE.
## Requirements
### Requirement: Product Thesis

MicroIDE SHALL be a native desktop IDE built in C++20 with SDL3, distributed as a single-window application with no GPU acceleration, positioned as a compact combination of a VSCode-shaped surface area and Zed-class responsiveness, with AI workflows as a first-class built-in capability.

#### Scenario: Rendering backend does not require a GPU
- **WHEN** MicroIDE is launched on a host without hardware-accelerated OpenGL, Vulkan, or Metal available to the user session
- **THEN** the application SHALL start, render the shell, and accept input using SDL3's CPU-capable render path (including the `SDL3_ttf` text backend or the debug-text fallback) without requiring GPU features

#### Scenario: Single-window shell shape is preserved
- **WHEN** a user opens any built-in workflow (editor, compare, merge, search, git, terminal, chat)
- **THEN** the workflow SHALL render inside the single MicroIDE window, reusing the menu bar, project tabs, file tabs, persistent sidebar, editor surface, and docked bottom panel, and SHALL NOT spawn detached OS windows or native OS menus

### Requirement: Priority Order For Engineering Tradeoffs

When MicroIDE engineering tradeoffs conflict, the project SHALL resolve them in the following order: (1) correctness, (2) speed, (3) low CPU usage, (4) low memory footprint, (5) architectural clarity, (6) compatibility only when explicitly required.

#### Scenario: Correctness beats compatibility
- **WHEN** a fix for an observably wrong behavior requires breaking a non-contractual compatibility shim
- **THEN** the fix SHALL land and the shim SHALL be removed rather than preserved

#### Scenario: Speed beats memory
- **WHEN** a change can either reduce peak memory by a small amount at the cost of added frame-time work on typing or scrolling, or keep memory flat and preserve the frame-time budget
- **THEN** the change SHALL preserve the frame-time budget

### Requirement: Built-in Workflows Remain Host-Owned

Editor, compare, merge, search, git, terminal, diagnostics, chat, inline completion, and MCP tool flows SHALL remain built-in, host-owned product features. Plugins MAY contribute data, commands, providers, registries, and structured requests through narrow host APIs, but SHALL NOT replace or reimplement these workflows.

#### Scenario: Plugin attempts to replace diff rendering
- **WHEN** a plugin is installed that declares itself as a replacement compare or merge renderer
- **THEN** the host SHALL ignore that replacement and continue to render compare and merge through the built-in pipeline, surfacing plugin contributions only through the existing contribution registries

### Requirement: Durable Non-Goals

MicroIDE SHALL treat the following as out of scope unless deliberately promoted into a dedicated phase: full debugger UI beyond first-pass start/stop plus output-channel plumbing, plugin marketplaces and remote install flows, Micro-plugin compatibility, cloud or collaboration features, account systems and sync, recent-project and recent-file surfaces, and native OS menu integration.

#### Scenario: Feature request falls inside a non-goal
- **WHEN** a proposal requests a feature whose primary capability falls inside the non-goal list
- **THEN** the proposal SHALL be rejected or SHALL explicitly declare itself as promoting a non-goal into its own phase, with an updated product-vision delta in the same change

### Requirement: AI Is In Scope

The MicroIDE product SHALL treat AI workflows (chat, inline completion, MCP-backed tool use, provider-bridge–driven model access) as in-scope, host-owned first-class surfaces. The implementation guide and any durable docs SHALL NOT list AI or chat as a non-goal.

#### Scenario: Documentation lists AI as a non-goal
- **WHEN** any durable doc (implementation guide, roadmap, guidelines) is reviewed
- **THEN** it SHALL NOT list AI, chat, or inline completion as a non-goal, and any such entry SHALL be corrected in the same review

