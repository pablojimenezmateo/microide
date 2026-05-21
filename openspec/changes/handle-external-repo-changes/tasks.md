## 1. Event Normalization

- [ ] 1.1 Define typed project file and repository change event records.
- [ ] 1.2 Normalize native watcher events and Git metadata changes into the typed records.
- [ ] 1.3 Add coalescing/debounce logic with generation IDs.

## 2. Service Fan-Out

- [ ] 2.1 Wire repository events into Git snapshot stale/refresh scheduling.
- [ ] 2.2 Wire file events into project tree refresh and search index invalidation.
- [ ] 2.3 Wire rename/delete/change events into diagnostics and blame stale-path cleanup.

## 3. Open Surface Handling

- [ ] 3.1 Implement clean-buffer reload and dirty-buffer external-change prompt behavior.
- [ ] 3.2 Mark compare tabs stale and refresh asynchronously with selected-hunk/scroll anchor preservation.
- [ ] 3.3 Mark merge tabs stale after result-file or index changes and require revalidation before mark-resolved.
- [ ] 3.4 Invalidate commit draft warnings when staged/unstaged state changes externally.

## 4. Verification

- [ ] 4.1 Add tests for clean reload, dirty conflict prompt, external rename/delete, stale compare refresh, stale merge validation, and external branch switch.
- [ ] 4.2 Add watcher burst tests proving refresh coalescing and stale generation handling.
- [ ] 4.3 Run targeted file-watch, Git sidebar, editor reload, compare, merge, search, diagnostics, and idle/perf scenarios.
