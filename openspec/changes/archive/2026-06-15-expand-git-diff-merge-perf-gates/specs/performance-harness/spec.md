## ADDED Requirements

### Requirement: Git Workstation Scenarios Are Covered
The performance harness SHALL include deterministic scenarios for Git/diff/merge workstation workflows: `git_sidebar_refresh_large_repo`, `git_sidebar_refresh_many_untracked`, `diff_open_1000_file_changes`, `diff_next_hunk_large_file`, `diff_stage_hunk_large_patch`, `diff_stage_selected_lines`, `merge_open_many_conflicts`, `merge_next_conflict_large_file`, `merge_accept_hunk_interleaved`, `merge_edit_result_then_scroll`, `commit_open_with_large_staged_set`, `external_change_refresh_open_diff`, and `external_change_refresh_open_merge`.

#### Scenario: Workstation scenario set runs
- **WHEN** `microide_perf_tests` runs the full reference suite
- **THEN** each Git workstation scenario SHALL execute with isolated state, fixed fixtures or fixed seeds, and committed baseline comparison

#### Scenario: Scenario reports standard metrics
- **WHEN** any Git workstation scenario completes
- **THEN** the report SHALL include wall time, frame-time percentiles, CPU time, allocation count, RSS movement, redraw counts, SDL wake-up count, and background-task count

### Requirement: Git Workstation Fixtures Are Deterministic
Git workstation performance scenarios SHALL use deterministic fixture repositories and scripted UI/event drivers. Fixtures SHALL include large status sets, many untracked files, many changed files, large text diffs, many merge conflicts, and large staged sets.

#### Scenario: Large repo refresh fixture
- **WHEN** `git_sidebar_refresh_large_repo` runs
- **THEN** it SHALL open a deterministic repository fixture with enough tracked changes to exercise status parsing, grouping, and view-model construction

#### Scenario: External refresh fixture
- **WHEN** `external_change_refresh_open_merge` runs
- **THEN** it SHALL simulate an external result or conflict-state change through the watcher/event seam rather than relying on local user state

### Requirement: New Workstation Hot Paths Ship With Perf Coverage
Changes that add or materially modify Git repository refresh, diff hunk navigation, hunk/line staging, merge conflict acceptance, merge result editing, commit workflow open, or external-change refresh SHALL add or update at least one Git workstation performance scenario in the same change.

#### Scenario: Hunk staging implementation lands
- **WHEN** a change implements hunk or selected-line staging
- **THEN** it SHALL include green `diff_stage_hunk_large_patch` or `diff_stage_selected_lines` evidence, or add the scenario and baseline if it does not yet exist
