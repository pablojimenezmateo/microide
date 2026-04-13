# Refactoring Plan: Code Deduplication and Architectural Cleanup

This document outlines a plan to reduce code duplication and improve the architectural integrity of the `microide` project.

## 1. UI Componentization & WorkspaceShell Refactoring

### Problem
`WorkspaceShell` is currently a "God Object" split across approximately 20 files. UI rendering in `WorkspaceShellRender.cpp` is largely procedural, with significant duplication in how scrollable lists, buttons, and tabs are handled across Sidebar, Overlays, and the Bottom Panel.

### Action Plan
- **Introduce `View` or `Panel` Abstraction**: Create a base class or interface for UI panels.
    - Common functionality: `Render`, `HandleEvent`, `Layout`, `Focus`.
    - Specific implementations: `SidebarPanel`, `EditorPanel`, `TerminalPanel`, `OverlayPanel`.
- **Unified List Renderer**: Extract the duplicated scrollable list logic from `WorkspaceShellRender.cpp` into a reusable `ListWidget` or similar component.
    - Should handle: layout computation, scrollbar integration, item rendering via a delegate/callback, and selection management.
- **Componentize UI Elements**: Move procedural drawing of buttons, tabs, and dividers into dedicated component classes or a `UiRenderer` utility.
- **Deduplicate Cursor Logic**: Centralize the calculation of `TextInputVisual` (cursor position and metrics) into a single helper that takes a `TextViewport` and layout metrics.

## 2. Git Service Consolidation

### Problem
`GitStatusService`, `GitBlameService`, and `GitCompareService` each implement their own high-level parsing, path normalization, and in some cases, thread management.

### Action Plan
- **Create `GitRepository` Abstraction**:
    - Centralize path normalization (Absolute <-> Relative to root).
    - Provide a unified interface for executing git commands and handling errors.
- **Unify Porcelain Parsing**: Extract the common logic for parsing `git status --porcelain` and `git diff` output into a shared parser utility.
- **Standardize Worker Threads**: Use a common task runner or thread pool for asynchronous git operations instead of each service managing its own `std::thread`.

## 3. Diff & Merge Model Unification

### Problem
`CompareModel` and `MergeDisplayModel` share very similar structures (rows and hunks) but are implemented independently.

### Action Plan
- **Unified Diff Data Structure**: Create a shared `DiffGrid` structure that can represent 2-way and 3-way diffs.
- **Shared Diff Rendering**: Use a single rendering component for both Compare and Merge views, parameterized by the number of columns and the specific diff metadata.

## 4. Persistence & State Management

### Problem
`PersistedEditorTabState` is a "fat" struct containing fields for all tab types. Serialization logic is repetitive.

### Action Plan
- **Polymorphic Persistence**: Refactor `PersistedEditorTabState` to use a variant or polymorphism (e.g., `EditorTabState`, `CompareTabState`, `MergeTabState`).
- **Generic Serialization**: Explore a more generic way to handle key-value state persistence to reduce boilerplate in `WorkspaceShellPersistence.cpp`.

## 5. Rendering Abstractions

### Problem
Logic for text layout and viewport management is scattered across `TextViewport`, `EditorViewRenderer`, and various `WorkspaceShell` render methods.

### Action Plan
- **Consolidate Viewport Logic**: Ensure all text-based views (Editor, Terminal, Diff) use a consistent `Viewport` abstraction for scrolling and coordinate mapping.
- **Reusable Text Grid**: Create a `TextGridRenderer` that can handle any grid-based text display, including line numbers and markers, to be used by both the Editor and Diff views.

## Implementation Phases

1.  **Phase 1: Git Consolidation**: Implement `GitRepository` and unify porcelain parsing. This is low-risk and provides immediate cleanup.
2.  **Phase 2: UI Componentization (Lists & Buttons)**: Extract `ListWidget` and common UI drawing logic. This will significantly shrink `WorkspaceShellRender.cpp`.
3.  **Phase 3: Diff/Merge Unification**: Unify the models and rendering logic for Compare and Merge views.
4.  **Phase 4: WorkspaceShell Decomposition**: Gradually move Sidebar and Panel logic into separate `View` classes.
