## Why

Diff review is the main user-facing proof of the Git workstation wedge. Existing compare tabs support important targets, but the workflow needs first-class working-tree, commit, branch, and conflict review modes with richer navigation and presentation details.

## What Changes

- Extend compare behavior to explicitly cover working-tree, commit, branch, and conflict-review modes.
- Add diff model and presentation requirements for file metadata, hunks, rename/mode/binary/submodule summaries, collapsed context, whitespace controls, inline word diff, and stable line mapping.
- Add review-oriented actions: next/previous changed file, next/previous hunk, open corresponding file/line, copy path, copy hunk/patch, mark reviewed hook points, and large-diff progress/fallback UI that does not degrade correctness.
- Keep Git parsing/model construction separate from render presentation.

## Capabilities

### New Capabilities

### Modified Capabilities
- `diff-merge-editor`: Adds explicit diff review modes, model/presentation layering, and review interaction requirements for compare surfaces.

## Impact

- Affects compare model construction, compare tab state, compare keyboard/mouse actions, render view-model builder, syntax/intraline highlight integration, and compare tests/perf scenarios.
- Prepares but does not implement hunk/line staging or persistent review state; those are separate changes.
