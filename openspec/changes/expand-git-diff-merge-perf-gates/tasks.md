## 1. Phase 9A: Early Skeletons And Fixtures

- [x] 1.1 Add deterministic fixture repositories for large status, many untracked, 1000 changed files, large text diff, many conflicts, and large staged set.
- [x] 1.2 Add helper APIs for scripted Git sidebar refresh, diff navigation, staging, merge accept/edit/scroll, commit open, and watcher refresh.
- [x] 1.3 Ensure fixtures use isolated app/config/state/cache roots and fixed seeds.
- [x] 1.4 Add scenario skeleton entries and harness seams before feature-complete UI flows so regressions can be measured early.

## 2. Phase 9B: Feature-Adjacent Scenario Activation

- [x] 2.1 Implement `git_sidebar_refresh_large_repo` and `git_sidebar_refresh_many_untracked`.
- [x] 2.2 Implement `diff_open_1000_file_changes` and `diff_next_hunk_large_file`.
- [x] 2.3 Implement `diff_stage_hunk_large_patch` and `diff_stage_selected_lines` when patch operations are available.
- [x] 2.4 Implement `merge_open_many_conflicts`, `merge_next_conflict_large_file`, `merge_accept_hunk_interleaved`, and `merge_edit_result_then_scroll`.
- [x] 2.5 Implement `commit_open_with_large_staged_set`.
- [x] 2.6 Implement `external_change_refresh_open_diff` and `external_change_refresh_open_merge`.

## 3. Phase 9C: Baselines And Docs Consolidation

- [ ] 3.1 Capture reference baselines on `perf-runner-v1` for each new scenario. (deferred: use workflow_dispatch `capture_git_workstation_baselines=true` when the self-hosted queue is available)
- [x] 3.2 Commit baseline JSON files with tolerances and rationale where defaults are insufficient.
- [x] 3.3 Update `docs/perf-harness.md` to list the new Git workstation scenarios and evidence requirements.

## 4. Verification

- [x] 4.1 Run local smoke iterations for each scenario.
- [x] 4.2 Run the reference harness suite or document the required reference run before merge.
- [x] 4.3 Verify reports include provenance and the full standard metric set.
