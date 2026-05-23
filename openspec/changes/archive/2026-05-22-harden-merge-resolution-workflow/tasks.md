## 1. Conflict Model

- [x] 1.1 Add explicit merge conflict kind enum and metadata to merge model construction.
- [x] 1.2 Classify both-modified, add/add, delete/modify, rename/rename, rename/delete, binary, mode, submodule, and line-ending-heavy conflicts where Git data supports it.
- [x] 1.3 Add fixture helpers for creating real conflict repositories.

## 2. Resolver Presentation

- [x] 2.1 Add current/result/incoming/base labels with branch/ref context to merge view models.
- [x] 2.2 Add collapsible base pane state while preserving selection and scroll anchors.
- [x] 2.3 Add conflict index, total count, remaining count, remaining file count, and result state display.

## 3. Merge Actions

- [x] 3.1 Implement accept current, incoming, both-current-first, and both-incoming-first actions.
- [x] 3.2 Implement reset result hunk and jump-next-unresolved actions.
- [x] 3.3 Add show-raw-markers, copy-side-snippet, reopen-result-file, and mark-resolved actions.

## 4. Validation

- [x] 4.1 Implement saved/dirty, conflict-marker, expected-existence, external-modification, index-generation, and line-ending validation.
- [x] 4.2 Add explicit override confirmation for remaining conflict markers.
- [x] 4.3 Invalidate resolved state after stale index or external result-file changes.

## 5. Verification

- [x] 5.1 Add merge tests for both-modified, add/add, delete/modify, rename/rename, rename/delete, binary, UTF-8, CRLF, and large-file conflicts.
- [x] 5.2 Add interaction tests for accept both orders, reset hunk, base toggle, manual edit, validation failure, and mark-resolved success.
- [x] 5.3 Run focused merge, compare/merge render, Git conflict, and perf scenarios covering many conflicts.
