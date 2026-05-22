## 1. View Model Shape

- [x] 1.1 Define Git sidebar section, row, action, stale, refresh, and error view-model fields.
- [x] 1.2 Map repository snapshot entries into Conflicts, Staged, Changed, Untracked, and Outgoing sections.
- [x] 1.3 Preserve branch/upstream/ahead/behind summary rendering through the view model.

## 2. Action Routing

- [x] 2.1 Represent row actions as typed command IDs with repository-relative path payloads.
- [x] 2.2 Route `Enter`, `d`, `s`, `u`, `x`, `m`, `c`, `r`, and `o` through the same action dispatcher used by mouse/context-menu paths.
- [x] 2.3 Disable unavailable actions per row kind and show a concise status message instead of failing silently.

## 3. Safety Prompts

- [x] 3.1 Add discard preview prompt state for modified, staged, and untracked rows.
- [x] 3.2 Reuse async diff generation or snapshot data for discard previews where possible.
- [x] 3.3 Require explicit confirmation before file discard, staged discard, or untracked removal.

## 4. Tests And Docs

- [x] 4.1 Add sidebar grouping tests for conflict, staged, unstaged, untracked, rename, and outgoing snapshots.
- [x] 4.2 Add keyboard action tests for each supported shortcut.
- [x] 4.3 Add confirmation tests proving discard cannot execute from a single accidental keypress.
- [x] 4.4 Update help/command labeling docs for the Git sidebar action grammar.
