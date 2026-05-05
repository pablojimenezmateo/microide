## 1. Editor Core

- [x] 1.1 Add a multi-selection editor state model with one primary selection, secondary carets, and atomic undo/redo grouping for multi-caret commands
- [x] 1.2 Implement multi-caret insert, delete/backspace, paste, indent/outdent, and line-wise edit behavior on top of the shared editor command path
- [x] 1.3 Introduce a wrapped-line layout/cache shared by render, hit-testing, caret movement, and scroll math
- [x] 1.4 Wire soft-wrap commands and project-scoped persistence so each project restores its own wrap mode
- [x] 1.5 Disable ligatures across editor, compare, and merge code-text rendering paths

## 2. Ignored Tree And Background Eligibility

- [x] 2.1 Refactor the project-tree model so ignored-node visibility, ignored status, and child materialization are tracked independently
- [x] 2.2 Implement opaque ignored-directory nodes that enumerate only one level on expansion and direct-open behavior for ignored files
- [x] 2.3 Keep file indexing, project search, AI context collection, diagnostics discovery, and watcher-driven parsing excluded from ignored descendants by default
- [x] 2.4 Remove legacy hide-and-skip ignored-tree helpers that no longer match the lazy catalog contract

## 3. Project-Scoped State And Pane Layout

- [x] 3.1 Audit remaining shell-scoped project behavior and move wrap mode, ignored-tree expansion state, divider fractions, and equivalent per-project presentation state into project-owned services/persistence
- [x] 3.2 Replace arbitrary sidebar, editor-split, compare, merge, and bottom-panel resize clamps with content-derived viable minima
- [x] 3.3 Delete legacy shell-global aliases and helper methods that become redundant after the project-scoped ownership cutover

## 4. Diff And Merge Presentation

- [x] 4.1 Tune shared diff/merge palette tokens to low-contrast fills that preserve neutral foreground text
- [x] 4.2 Update compare and merge divider math to respect the new content-derived minimum pane widths

## 5. Regression Coverage And Performance Evidence

- [x] 5.1 Add editor regression tests for multi-caret edit grouping, undo/redo, wrapped caret motion, wrapped hit-testing, and ligature-free code rendering
- [x] 5.2 Add project-tree and search regressions for visible ignored files, one-level ignored-directory expansion, and ignored-content exclusion from background tools
- [x] 5.3 Add resize and render regressions for sidebar freedom, compare/merge divider minima, and low-contrast diff text legibility
- [x] 5.4 Capture perf evidence for soft-wrap plus multi-caret editing and for ignored-directory initial open/expansion paths

## 6. Docs And Final Validation

- [x] 6.1 Update product, implementation, and active-work docs to reflect soft wrap in scope, lazy ignored-tree behavior, and project-scoped ownership
- [x] 6.2 Update durable specs/docs that still mention soft wrap as a non-goal
- [x] 6.3 Run the targeted build/test matrix for editor, project tree, diff/merge, persistence, and resize paths, plus relevant sanitizer coverage
