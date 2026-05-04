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

- [ ] 3.1 Audit remaining shell-scoped project behavior and move wrap mode, ignored-tree expansion state, divider fractions, and provider-session selection state into project-owned services/persistence
- [ ] 3.2 Replace arbitrary sidebar, editor-split, compare, merge, and bottom-panel resize clamps with content-derived viable minima
- [ ] 3.3 Delete legacy shell-global aliases and helper methods that become redundant after the project-scoped ownership cutover

## 4. AI Provider Runtime Cutover

- [ ] 4.1 Introduce a transport-neutral AI provider runtime interface covering auth, model discovery, streaming responses, tool calls, and cancellation
- [ ] 4.2 Add a direct-provider adapter path for host-managed HTTP or in-process providers without a bridge subprocess
- [ ] 4.3 Migrate chat, inline completion, provider auth, and tool approval flows to the runtime interface
- [ ] 4.4 Keep sidecar-backed providers working through an adapter layer, then remove bridge-first call sites and obsolete `WorkspaceProviderBridge` helpers

## 5. Diff And Merge Presentation

- [ ] 5.1 Tune shared diff/merge palette tokens to low-contrast fills that preserve neutral foreground text
- [ ] 5.2 Update compare and merge divider math to respect the new content-derived minimum pane widths

## 6. Regression Coverage And Performance Evidence

- [ ] 6.1 Add editor regression tests for multi-caret edit grouping, undo/redo, wrapped caret motion, wrapped hit-testing, and ligature-free code rendering
- [ ] 6.2 Add project-tree and search regressions for visible ignored files, one-level ignored-directory expansion, and ignored-content exclusion from background tools
- [ ] 6.3 Add AI runtime regressions for direct-provider flow, sidecar crash recovery, and non-blocking request delivery
- [ ] 6.4 Add resize and render regressions for sidebar freedom, compare/merge divider minima, and low-contrast diff text legibility
- [ ] 6.5 Capture perf evidence for soft-wrap plus multi-caret editing and for ignored-directory initial open/expansion paths

## 7. Docs And Final Validation

- [ ] 7.1 Update product, implementation, and active-work docs to reflect soft wrap in scope, lazy ignored-tree behavior, project-scoped ownership, and the provider-runtime contract
- [ ] 7.2 Update durable specs/docs that still mention bridge-first AI access or soft wrap as a non-goal
- [ ] 7.3 Run the targeted build/test matrix for editor, project tree, diff/merge, AI runtime, persistence, and resize paths, plus relevant sanitizer coverage
