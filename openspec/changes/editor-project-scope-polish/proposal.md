## Why

MicroIDE already has the right product shape, but several daily-use gaps still force avoidable friction: editors still lack multiple cursors and soft wrap, ignored files are hidden instead of being cheaply accessible, compare colors overpower the text, pane dividers stop short of useful limits, and some state still risks living at shell scope instead of project scope. These are no longer isolated papercuts; they are durable workflow and architecture gaps that should be corrected together so the editor, project model, and workspace state contract converge on a cleaner design.

## What Changes

- Add first-class editor multiple-cursor and multiple-selection behavior, including aligned insertion, multi-caret navigation, per-caret edit operations, and predictable interaction with selections, undo/redo, and search results.
- Promote soft wrap into a supported editor capability with deterministic wrapped-line layout, caret movement, hit-testing, scrolling, and persistence of wrap mode as project-owned state.
- Introduce lazy `.gitignore` handling: ignored files and directories remain visible in the project tree, but ignored descendants stay out of background indexing, search, AI context collection, and watcher-driven parsing until the user explicitly opens a file or expands an ignored directory.
- Remove legacy methods and compatibility paths made obsolete by the new editor and project-tree seams.
- Reduce compare and merge contrast by tuning fill colors downward and ensuring neutral foreground text remains readable instead of inheriting a tinted appearance.
- Disable character ligatures in editor-family surfaces so source text, compare text, and merge text always render one codepoint sequence at a time.
- Audit project-owned state and move any remaining shell-scoped project behavior into project services; update divider sizing rules so sidebar, compare, and merge panes can be resized down to the real content minima instead of arbitrary clamps.

## Capabilities

### New Capabilities
- `editor-multicursor-and-wrap`: multiple cursors, soft wrap, ligature-free code rendering, and project-scoped editor presentation state
- `lazy-gitignore-catalog`: visible-but-lazy handling for ignored files and directories so the project tree can surface everything without paying full indexing cost up front

### Modified Capabilities
- `product-vision`: promote soft wrap out of the durable non-goals list and into an explicit supported phase
- `diff-merge-editor`: add low-contrast compare and merge presentation requirements and divider-resize behavior expectations
- `workspace-architecture`: require project-specific UI and data state to live in project-owned services rather than shell-global state, and tighten the ownership rules for divider/layout state
- `performance-budgets`: add explicit evidence requirements for soft-wrap and multiple-cursor hot paths plus lazy ignored-tree visibility and expansion paths

## Impact

- **Editor core**: multi-caret selection model, wrapped layout/cache, hit-testing, caret movement, undo batching, and ligature-free rendering defaults
- **Workspace chrome**: project-scoped persistence and service ownership for wrap mode, divider fractions, compare layout, and other per-project UI state; updated divider minima for sidebar, compare, and merge surfaces
- **Project catalog/tree**: ignored-node discovery, lazy expansion for ignored directories, on-demand child enumeration, and exclusion of ignored descendants from background tools until opt-in access
- **Diff and merge presentation**: shared palette/token updates and regression coverage for readable low-contrast decorations
- **Tests and docs**: new editor, project-tree, and resize regressions; updated product/spec docs; perf evidence for typing, scrolling, tree open, and ignored-directory expansion
