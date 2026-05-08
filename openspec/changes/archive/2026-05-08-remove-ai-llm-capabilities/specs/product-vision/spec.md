## MODIFIED Requirements

### Requirement: Product Thesis

MicroIDE SHALL be a native desktop IDE built in C++20 with SDL3, distributed as a single-window application with no GPU acceleration, positioned as a compact combination of a VSCode-shaped surface area and Zed-class responsiveness.

#### Scenario: Rendering backend does not require a GPU
- **WHEN** MicroIDE is launched on a host without hardware-accelerated OpenGL, Vulkan, or Metal available to the user session
- **THEN** the application SHALL start, render the shell, and accept input using SDL3's CPU-capable render path (including the `SDL3_ttf` text backend or the debug-text fallback) without requiring GPU features

#### Scenario: Single-window shell shape is preserved
- **WHEN** a user opens any built-in workflow (editor, compare, merge, search, git, terminal)
- **THEN** the workflow SHALL render inside the single MicroIDE window, reusing the menu bar, project tabs, file tabs, persistent sidebar, editor surface, and docked bottom panel, and SHALL NOT spawn detached OS windows or native OS menus

### Requirement: Built-in Workflows Remain Host-Owned

Editor, compare, merge, search, git, terminal, and diagnostics flows SHALL remain built-in, host-owned product features. Plugins MAY contribute data, commands, registries, and structured requests through narrow host APIs, but SHALL NOT replace or reimplement these workflows.

#### Scenario: Plugin attempts to replace diff rendering
- **WHEN** a plugin is installed that declares itself as a replacement compare or merge renderer
- **THEN** the host SHALL ignore that replacement and continue to render compare and merge through the built-in pipeline, surfacing plugin contributions only through the existing contribution registries
