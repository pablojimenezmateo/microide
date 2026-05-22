## Why

Merge resolution is the highest-trust part of the Git workstation wedge: users must understand each conflict, resolve without losing context, and avoid marking invalid files as resolved. Existing three-way merge tabs are a strong base, but conflict taxonomy, validation, labeling, and external-change safety need to become explicit contracts.

## What Changes

- Add conflict taxonomy for both-modified, add/add, delete/modify, rename/rename, rename/delete, file/directory, binary, submodule, mode, and whitespace/line-ending-heavy conflicts.
- Improve merge resolver view requirements: unmistakable current/result/incoming/base labels, branch/commit context, remaining conflict counts, dirty/saved/invalid/resolved state, and base pane toggle.
- Add required merge actions for accept current, accept incoming, accept both orders, edit result, reset result hunk, jump unresolved, show raw markers, copy side snippet, and mark resolved.
- Add validation before marking resolved: no conflict markers unless overridden, saved result, expected existence, index state check, external modification detection, and line-ending preservation.

## Capabilities

### New Capabilities

### Modified Capabilities
- `diff-merge-editor`: Adds merge conflict taxonomy, result validation, resolver labeling, and safety requirements.

## Impact

- Affects merge model construction, merge tab state, conflict action routing, result save/mark-resolved logic, Git conflict detection, file lifecycle handling, and merge tests/perf scenarios.
- Coordinates with external-change handling but keeps file-watch plumbing in a separate change.
