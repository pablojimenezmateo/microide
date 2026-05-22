## 1. State Model

- [x] 1.1 Define review target, file review entry, hunk review entry, note, and changed-since-reviewed model types.
- [x] 1.2 Define stable hunk identity using base/head/path/ranges/content hash inputs.
- [x] 1.3 Add service/helper APIs for mark reviewed, mark unreviewed, add note, delete note, and query status.

## 2. Persistence

- [x] 2.1 Add typed persisted records for branch review state.
- [x] 2.2 Route review state load/save through `PersistenceService`.
- [x] 2.3 Add round-trip, unknown-record, corrupt-record, and pruning tests.

## 3. UI Integration

- [x] 3.1 Add reviewed/unreviewed/changed markers to branch review file-list view models.
- [x] 3.2 Add hunk-level reviewed markers and note indicators to compare view models.
- [x] 3.3 Add commands for mark file reviewed, mark hunk reviewed, clear review, and edit note.
- [x] 3.4 Add explicit clear-state command scoped to the active review target.

## 4. Verification

- [x] 4.1 Add tests for review state across tab close/reopen and project reload.
- [x] 4.2 Add tests for hunk changed-since-reviewed after edit, rebase-like base change, and rename metadata.
- [x] 4.3 Add pruning tests for retention limits and active-target preservation.
- [x] 4.4 Run persisted-state, branch compare, render view-model, and architecture-lint tests.
