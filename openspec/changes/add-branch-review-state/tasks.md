## 1. State Model

- [ ] 1.1 Define review target, file review entry, hunk review entry, note, and changed-since-reviewed model types.
- [ ] 1.2 Define stable hunk identity using base/head/path/ranges/content hash inputs.
- [ ] 1.3 Add service/helper APIs for mark reviewed, mark unreviewed, add note, delete note, and query status.

## 2. Persistence

- [ ] 2.1 Add typed persisted records for branch review state.
- [ ] 2.2 Route review state load/save through `PersistenceService`.
- [ ] 2.3 Add round-trip, unknown-record, corrupt-record, and pruning tests.

## 3. UI Integration

- [ ] 3.1 Add reviewed/unreviewed/changed markers to branch review file-list view models.
- [ ] 3.2 Add hunk-level reviewed markers and note indicators to compare view models.
- [ ] 3.3 Add commands for mark file reviewed, mark hunk reviewed, clear review, and edit note.

## 4. Verification

- [ ] 4.1 Add tests for review state across tab close/reopen and project reload.
- [ ] 4.2 Add tests for hunk changed-since-reviewed after edit, rebase-like base change, and rename metadata.
- [ ] 4.3 Run persisted-state, branch compare, render view-model, and architecture-lint tests.
