# Tech Debt Review

Reviewed on 2026-04-14.

Scope:
- `src/project/*`
- `src/workspace/*`
- `tests/*`
- recent compare, terminal, window-chrome, and source-control changes

## Highest Impact Debt

### 1. Source Control outgoing files were not reliably PR-base-aware

Impact:
- High
- The `Outgoing files` section could drift from the actual PR target branch and show the wrong file set.
- This is user-visible and directly affects review confidence in the Source Control pane.

Previous behavior:
- Base resolution preferred `origin/HEAD`, then local `main` or `master`, and only later fell back to upstream.
- That worked for default-branch PRs, but it was wrong for PRs targeting a non-default base branch.

Addressed in this pass:
- `ResolveGitBaseReference()` now prefers `branch.<current>.gh-merge-base` when present and resolves it to a local or remote ref before falling back to default-branch heuristics.
- The Source Control pane continues to use that resolved base for `Outgoing files`.
- Coverage now includes a non-default PR base case.
- Shared git-test repository bootstrap helpers now live in `tests/TestSupport.*` instead of being copied across individual test files.

Files changed:
- `src/project/GitCompareService.cpp`
- `tests/GitServiceTests.cpp`

Remaining caveat:
- If the local branch has no explicit PR-base metadata, the fallback is still heuristic.
- This is much safer than before, but not a complete PR integration layer.

## Next Debt Items

### 2. `WorkspaceShell` remains a high-blast-radius object

Impact:
- High
- Small UI fixes routinely require coordinated edits across `WorkspaceShell.cpp`, `WorkspaceShellRender.cpp`, input coordinators, and test access helpers.
- This increases regression risk and slows iteration.

Evidence:
- `src/workspace/WorkspaceShell.cpp`: 1895 lines
- `src/workspace/WorkspaceShellRender.cpp`: 1895 lines

Recommendation:
- Continue extracting subsystem-owned layout and render code, starting with compare and window-chrome state.
- Replace cross-file primitive state plumbing with narrower structs owned by the relevant subsystem.

### 3. Workspace-shell tests are too monolithic

Impact:
- High
- Review and maintenance cost are rising because unrelated behaviors share the same large fixture files.
- This makes failures harder to localize and encourages broad helper access into internals.

Evidence:
- `tests/WorkspaceShellProjectTests.cpp`: 1260 lines
- `tests/WorkspaceShellCompareTests.cpp`: 341 lines
- `tests/WorkspaceShellChromeTests.cpp`: 115 lines
- `tests/WorkspaceShellSourceControlTests.cpp`: 119 lines
- `tests/WorkspaceShellTerminalTests.cpp`: 570 lines

Recommendation:
- Split by behavior domain instead of by legacy file history.
- Recommended first split:
  - compare and merge tests
  - git sidebar and source-control tests
  - chrome and menu interaction tests

Addressed in this pass:
- Compare and merge interaction coverage now lives in `tests/WorkspaceShellCompareTests.cpp`.
- Menu and window-chrome interaction coverage now lives in `tests/WorkspaceShellChromeTests.cpp`.
- Git sidebar and source-control interaction coverage now lives in `tests/WorkspaceShellSourceControlTests.cpp`.
- `tests/WorkspaceShellProjectTests.cpp` is now narrower and focused on general project and editor behavior.
- The next split should separate remaining editor-blame-focused coverage from general project interaction tests.

### 4. Git service execution is still shell-string based

Impact:
- Medium
- The current `popen` + shell-string layer is compact, but quoting, portability, and error reporting remain fragile.
- Each new git feature increases the chance of subtle command-construction bugs.

Evidence:
- `src/project/GitCommandUtil.h` constructs shell commands directly.
- `GitCompareService.cpp` still composes git queries as raw shell strings.

Recommendation:
- Introduce a small argument-vector command runner for git operations.
- Keep shell fallback only where absolutely necessary.

### 5. Window chrome state is spread across app and workspace layers

Impact:
- Medium
- Recent fullscreen and maximize fixes required touching both `Application` and `WorkspaceShell` state.
- That duplication makes the custom chrome easier to regress.

Recommendation:
- Consolidate chrome state into a small shared struct with explicit `maximized`, `fullscreen`, and `custom_enabled` fields.
- Keep rendering dependent on one state object instead of recomputing combined flags at call sites.

## Recommended Order

1. Done in this pass: make `Outgoing files` align with configured PR base.
2. Next: reduce `WorkspaceShell` blast radius around compare and chrome state.
3. In progress: split the large workspace-shell test files by behavior domain, starting with compare and merge coverage.
4. After that: replace shell-string git execution with a safer command layer.
