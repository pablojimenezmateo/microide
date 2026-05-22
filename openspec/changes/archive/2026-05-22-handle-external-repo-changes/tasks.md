## 1. Event Normalization

- [x] 1.1 Define typed project file and repository change event records.
- [x] 1.2 Normalize native watcher events and Git metadata changes into the typed records.
- [x] 1.3 Add coalescing/debounce logic with generation IDs.

## 2. Service Fan-Out

- [x] 2.1 Wire repository events into Git snapshot stale/refresh scheduling.
- [x] 2.2 Wire file events into project tree refresh and search index invalidation.
- [x] 2.3 Wire rename/delete/change events into diagnostics and blame stale-path cleanup.

## 3. Open Surface Handling (6A)

- [x] 3.1 Implement clean-buffer reload and dirty-buffer external-change prompt behavior.
- [x] 3.2 Mark compare tabs stale and refresh asynchronously with selected-hunk/scroll anchor preservation.
- [x] 3.3 Mark merge tabs stale after result-file or index changes and require revalidation before mark-resolved.
- [x] 3.4 Invalidate commit draft warnings when staged/unstaged state changes externally.

## 4. Tree/Search/Diagnostics/Blame Follow-Through (6B)

- [x] 4.1 Complete tree/search/diagnostics/blame refresh fan-out with generation-aware stale suppression.
- [x] 4.2 Ensure rename/delete handling does not leave stale diagnostics or blame targets.
- [x] 4.3 Add focused tests for fan-out correctness under watcher bursts.

## 5. Commit-Draft And Commit-Check Invalidation (6C)

- [x] 5.1 Invalidate commit warnings and cached staged summaries when external staged/unstaged state changes.
- [x] 5.2 Add focused tests for commit-surface invalidation on external branch and index changes.

## 6. Verification
- [x] 6.1 Add tests for clean reload, dirty conflict prompt, external rename/delete, stale compare refresh, stale merge validation, and external branch switch.
- [x] 6.2 Add watcher burst tests proving refresh coalescing and stale generation handling.
- [x] 6.3 Run targeted file-watch, Git sidebar, editor reload, compare, merge, search, diagnostics, and idle/perf scenarios.
