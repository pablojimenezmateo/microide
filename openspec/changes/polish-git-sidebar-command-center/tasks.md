## 1. View Model Shape

- [ ] 1.1 Define Git sidebar section, row, action, stale, refresh, and error view-model fields.
- [ ] 1.2 Map repository snapshot entries into Conflicts, Staged, Changed, Untracked, and Outgoing sections.
- [ ] 1.3 Preserve branch/upstream/ahead/behind summary rendering through the view model.

## 2. Action Routing

- [ ] 2.1 Represent row actions as typed command IDs with repository-relative path payloads.
- [ ] 2.2 Route `Enter`, `d`, `s`, `u`, `x`, `m`, `c`, `r`, and `o` through the same action dispatcher used by mouse/context-menu paths.
- [ ] 2.3 Disable unavailable actions per row kind and show a concise status message instead of failing silently.

## 3. Safety Prompts

- [ ] 3.1 Add discard preview prompt state for modified, staged, and untracked rows.
- [ ] 3.2 Reuse async diff generation or snapshot data for discard previews where possible.
- [ ] 3.3 Require explicit confirmation before file discard, staged discard, or untracked removal.

## 4. Tests And Docs

- [ ] 4.1 Add sidebar grouping tests for conflict, staged, unstaged, untracked, rename, and outgoing snapshots.
- [ ] 4.2 Add keyboard action tests for each supported shortcut.
- [ ] 4.3 Add confirmation tests proving discard cannot execute from a single accidental keypress.
- [ ] 4.4 Update help/command labeling docs for the Git sidebar action grammar.
