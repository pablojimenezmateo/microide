## ADDED Requirements

### Requirement: Git Workstation Interaction Budgets
Git workstation interactions SHALL remain responsive under budgeted scenarios. Git sidebar refresh, changed-file selection, next/previous hunk navigation, hunk/line staging, merge conflict navigation, merge result typing, commit workflow open, and external diff/merge refresh SHALL not block render or input hot paths.

#### Scenario: Git sidebar refresh on large repo
- **WHEN** `git_sidebar_refresh_large_repo` runs
- **THEN** the UI SHALL remain responsive while refresh work runs in the background, and the scenario SHALL pass its committed wall-time, frame-time, CPU, wake-up, allocation, and RSS budgets

#### Scenario: Next hunk in large diff
- **WHEN** `diff_next_hunk_large_file` runs
- **THEN** hunk navigation SHALL meet the committed interaction budget without rebuilding unrelated diff or render state per keypress

#### Scenario: Typing in merge result after accepting hunks
- **WHEN** `merge_edit_result_then_scroll` runs
- **THEN** merge result typing and subsequent scrolling SHALL stay within the same frame-budget class as normal editor typing and merge scrolling

#### Scenario: External refresh of open merge
- **WHEN** `external_change_refresh_open_merge` runs
- **THEN** external change handling SHALL not create an idle wake storm or block the main thread while the merge tab refreshes

### Requirement: Workstation Performance Evidence Is Required
Changes touching Git repository refresh, Git sidebar row construction, diff model/presentation, hunk/line staging, merge model/presentation, commit workflow open, or external-change refresh SHALL include green harness evidence for the relevant Git workstation scenarios or a justified `perf-baseline:` update.

#### Scenario: Reviewer receives unmeasured diff interaction change
- **WHEN** a change modifies diff hunk navigation or presentation state and provides no relevant harness evidence
- **THEN** review SHALL require the matching Git workstation performance scenario before merge
