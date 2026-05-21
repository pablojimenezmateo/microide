## 1. Scenario Fixtures

- [ ] 1.1 Add deterministic fixture repositories for large status, many untracked, 1000 changed files, large text diff, many conflicts, and large staged set.
- [ ] 1.2 Add helper APIs for scripted Git sidebar refresh, diff navigation, staging, merge accept/edit/scroll, commit open, and watcher refresh.
- [ ] 1.3 Ensure fixtures use isolated app/config/state/cache roots and fixed seeds.

## 2. Scenario Implementation

- [ ] 2.1 Implement `git_sidebar_refresh_large_repo` and `git_sidebar_refresh_many_untracked`.
- [ ] 2.2 Implement `diff_open_1000_file_changes` and `diff_next_hunk_large_file`.
- [ ] 2.3 Implement `diff_stage_hunk_large_patch` and `diff_stage_selected_lines` when patch operations are available.
- [ ] 2.4 Implement `merge_open_many_conflicts`, `merge_next_conflict_large_file`, `merge_accept_hunk_interleaved`, and `merge_edit_result_then_scroll`.
- [ ] 2.5 Implement `commit_open_with_large_staged_set`.
- [ ] 2.6 Implement `external_change_refresh_open_diff` and `external_change_refresh_open_merge`.

## 3. Baselines And Docs

- [ ] 3.1 Capture reference baselines on `perf-runner-v1` for each new scenario.
- [ ] 3.2 Commit baseline JSON files with tolerances and rationale where defaults are insufficient.
- [ ] 3.3 Update `docs/perf-harness.md` to list the new Git workstation scenarios and evidence requirements.

## 4. Verification

- [ ] 4.1 Run local smoke iterations for each scenario.
- [ ] 4.2 Run the reference harness suite or document the required reference run before merge.
- [ ] 4.3 Verify reports include provenance and the full standard metric set.
