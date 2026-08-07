# Perf Phase Scaffolding Audit

TD-2026-08-07-163's committed result: for every phase gate in
`tests/perf/baselines/*.json`, how much of what it measures is the **scenario's own
scaffolding** rather than the product path the gate is named after.

Regenerate with:

```bash
tools/audit-perf-phase-scaffolding.py --json out.json --markdown out.md
```

It runs each phase under the allocation-site tracer, resolves every site's stack
with `addr2line -i`, and buckets the site by whether the innermost frame that
lands in this repository is under `src/` (product) or under `tests/`
(scaffolding). Allocation counts are deterministic, so this needs no idle runner.

**Recorded 2026-08-07** against `build/microide-perf-make` at
TD-2026-08-07-165's isolation fix. 115 phases, 0 errors, and **one** phase at or
above the 20 % threshold.

## The one

`value_tree.paging` reads 28 % scaffolding (50 of 180 allocations), and that is
the right answer rather than a defect. The 50 are `MakeVariables` building the
5,000 `DapVariable`s the scenario streams in 25 pages — the adapter payload the
product path exists to consume. It only crosses the threshold because
TD-2026-08-07-162 cut the product side of that phase by 98 % (5,097 → 93), so a
fixed input cost became a large share of a small total. Worth re-reading if the
phase is ever rebaselined upward.

## Unattributed sites

Eight phases have sites whose stack LTO flattened into
`ScenarioContext::Measure`, with no inline record of what was really running.
Those are reported separately rather than charged to either bucket: a
header-defined product template instantiated in the scenario TU resolves there,
and charging it to the enclosing frame is how `assist_merge::RankedUnion` first
read as "100 % scaffolding" when it is 100 % product. The largest are
`dap_protocol.decode` (800), `lsp_message_framing.parse` (560),
`toggle_line_comment.1000_lines` (449) and `multi_tab.open_tabs` (213) — all
small next to those phases' totals.

## Full table

Sorted by scaffolding share. 97 of the 115 phases read exactly 0 % — the tracer
attributed every one of their allocations to `src/`.

| phase | scenario | attributed allocations | scaffolding | share | unattributed |
| --- | --- | ---: | ---: | ---: | ---: |
| `value_tree.paging` | `debug_value_tree_paging` | 180 | 50 | **28 %** | 0 |
| `external.refresh_open_merge` | `external_change_refresh_open_merge` | 414 | 18 | 4 % | 0 |
| `terminal.open` | `terminal_alt_screen_toggle` | 73 | 3 | 4 % | 0 |
| `terminal.open` | `terminal_scroll_long_output` | 73 | 3 | 4 % | 0 |
| `value_tree.build_expand` | `debug_value_tree_expand_large` | 4000 | 160 | 4 % | 0 |
| `linter.type_invalid_edit` | `linter_on_save` | 118 | 2 | 2 % | 0 |
| `open_tab.with_indent_detect` | `editor_indent_detect_open` | 581 | 8 | 1 % | 0 |
| `repo_open.idle_500ms` | `repo_open_rss_idle` | 219 | 3 | 1 % | 0 |
| `multi_tab.open_tabs` | `multi_tab_cycle` | 23533 | 240 | 1 % | 213 |
| `snippet.many_mirror_shift` | `snippet_many_mirror_edit` | 7260 | 60 | 1 % | 0 |
| `multi_project.switch_cycles` | `multi_project_switch` | 9730 | 80 | 1 % | 0 |
| `large_file.open_to_first_paint` | `large_file_open_first_paint` | 2759 | 12 | 0 % | 0 |
| `large_file.open_lf_to_first_paint` | `large_file_open_lf_first_paint` | 3700 | 12 | 0 % | 0 |
| `merge.edit_result_typing` | `merge_edit_result_then_scroll` | 5818 | 14 | 0 % | 0 |
| `external.refresh_open_diff` | `external_change_refresh_open_diff` | 8914 | 18 | 0 % | 0 |
| `repo_open.open_and_first_frames` | `repo_open_rss_idle` | 2513 | 4 | 0 % | 0 |
| `switch_and_idle.switch_and_settle` | `switch_and_idle` | 6940 | 8 | 0 % | 0 |
| `search_first_result.search_to_first_result` | `search_first_result` | 22203 | 18 | 0 % | 0 |
| `assist.ranked_union` | `assist_ranked_union_merge` | 80 | 0 | 0 % | 160 |
| `branch_review.presentation_markers` | `branch_review_presentation_markers` | 1444 | 0 | 0 % | 0 |
| `branch_review.presentation_markers_unchanged` | `branch_review_presentation_markers` | 0 | 0 | 0 % | 0 |
| `git.refresh_dispatch` | `commit_open_with_large_staged_set` | 41622 | 0 | 0 % | 0 |
| `commit.open_staged_sidebar` | `commit_open_with_large_staged_set` | 42244 | 0 | 0 % | 4 |
| `compare_large.open_to_first_paint` | `compare_scroll_large_fixture` | 1550 | 0 | 0 % | 0 |
| `compare_large.scroll_burst` | `compare_scroll_large_fixture` | 14057 | 0 | 0 % | 0 |
| `compare_selection.open_to_first_paint` | `compare_scroll_selection` | 9842 | 0 | 0 % | 0 |
| `compare_selection.scroll_burst` | `compare_scroll_selection` | 11956 | 0 | 0 % | 0 |
| `dap_protocol.decode` | `dap_protocol_encode_decode` | 40000 | 0 | 0 % | 800 |
| `dap_protocol.encode` | `dap_protocol_encode_decode` | 613200 | 0 | 0 % | 0 |
| `breakpoints_model.rebuild` | `debug_breakpoints_model_rebuild` | 1026 | 0 | 0 % | 0 |
| `pane.hittest` | `debug_pane_hittest_geometry` | 0 | 0 | 0 % | 0 |
| `value_tree.rebuild` | `debug_value_tree_rebuild` | 0 | 0 | 0 % | 0 |
| `diff.open_large_compare` | `diff_next_hunk_large_file` | 184768 | 0 | 0 % | 0 |
| `diff.next_hunk_burst` | `diff_next_hunk_large_file` | 167938 | 0 | 0 % | 0 |
| `diff.open_first_changed_file` | `diff_open_1000_file_changes` | 49862 | 0 | 0 % | 0 |
| `diff.open_large_patch` | `diff_stage_hunk_large_patch` | 54488 | 0 | 0 % | 0 |
| `diff.stage_hunk` | `diff_stage_hunk_large_patch` | 7909 | 0 | 0 % | 0 |
| `diff.stage_selected_lines` | `diff_stage_selected_lines` | 7978 | 0 | 0 % | 0 |
| `add_cursor_next_match.repeat` | `editor_add_cursor_next_match` | 772 | 0 | 0 % | 0 |
| `buffer_find.find_as_you_type` | `editor_buffer_find_incremental` | 219 | 0 | 0 % | 0 |
| `column_selection.extend_down` | `editor_column_selection_burst` | 30 | 0 | 0 % | 0 |
| `column_selection.extend_right` | `editor_column_selection_burst` | 0 | 0 | 0 % | 0 |
| `editor_fold_viewport_refresh.scroll_frame` | `editor_fold_viewport_refresh` | 23464 | 0 | 0 % | 0 |
| `editor_indent_guides_paint.scroll_paint_frame` | `editor_indent_guides_paint` | 15468 | 0 | 0 % | 0 |
| `long_line_search.rare_query` | `editor_long_line_buffer_search` | 294 | 0 | 0 % | 0 |
| `long_line_search.common_query` | `editor_long_line_buffer_search` | 319 | 0 | 0 % | 0 |
| `long_line_search.next_match_burst` | `editor_long_line_buffer_search` | 2037 | 0 | 0 % | 0 |
| `long_line_scroll.end_home_jumps` | `editor_long_line_horizontal_scroll` | 1833 | 0 | 0 % | 0 |
| `long_line_scroll.word_motion_burst` | `editor_long_line_horizontal_scroll` | 2820 | 0 | 0 % | 0 |
| `long_line_edit.select_all` | `editor_long_line_select_all_edit` | 206 | 0 | 0 % | 0 |
| `long_line_edit.cut` | `editor_long_line_select_all_edit` | 343 | 0 | 0 % | 0 |
| `long_line_edit.paste` | `editor_long_line_select_all_edit` | 318 | 0 | 0 % | 0 |
| `long_line_edit.undo` | `editor_long_line_select_all_edit` | 255 | 0 | 0 % | 0 |
| `long_line_edit.redo` | `editor_long_line_select_all_edit` | 288 | 0 | 0 % | 0 |
| `long_line_edit.replace_selection_burst` | `editor_long_line_select_all_edit` | 1993 | 0 | 0 % | 0 |
| `mouse_selection_drag.160_moves` | `editor_mouse_selection_drag` | 0 | 0 | 0 % | 0 |
| `occurrences.pump_frames` | `editor_occurrences_scan` | 3983 | 0 | 0 % | 0 |
| `editor_render_whitespace_paint.scroll_overlay_frame` | `editor_render_whitespace_paint` | 19722 | 0 | 0 % | 0 |
| `save.normalize_1mb_buffer` | `editor_save_normalization` | 66 | 0 | 0 % | 0 |
| `editor_scroll_only_no_content_bump.scroll_frame` | `editor_scroll_only_no_content_bump` | 14369 | 0 | 0 % | 0 |
| `move_line_down.multi_caret_burst` | `editor_shaping_multi_caret` | 112149 | 0 | 0 % | 0 |
| `snippet.expand_20_placeholders` | `editor_snippet_expand` | 294 | 0 | 0 % | 0 |
| `snippet.linked_placeholder_typings` | `editor_snippet_placeholder_edit` | 950 | 0 | 0 % | 0 |
| `sort_lines_ascending.10000_lines` | `editor_sort_lines_large` | 40165 | 0 | 0 % | 0 |
| `editor_sticky_scroll_scroll.fast_scroll_frame` | `editor_sticky_scroll_scroll` | 26096 | 0 | 0 % | 0 |
| `editor.surround_multi_caret.insert` | `editor_surround_multi_caret` | 557 | 0 | 0 % | 0 |
| `toggle_line_comment.1000_lines` | `editor_toggle_comment_large_selection` | 460635 | 0 | 0 % | 449 |
| `minified_line.type_burst` | `editor_typing_minified_line` | 4693 | 0 | 0 % | 0 |
| `minified_line.backspace_burst` | `editor_typing_minified_line` | 4561 | 0 | 0 % | 0 |
| `file_finder_cold.open_finder` | `file_finder_cold` | 1300 | 0 | 0 % | 0 |
| `file_finder_type_query.type_and_rank` | `file_finder_type_query` | 14458 | 0 | 0 % | 0 |
| `file_finder_type_query.backspace_rescan` | `file_finder_type_query` | 5753 | 0 | 0 % | 0 |
| `first_line_edit.enter_backspace_burst` | `first_line_edit_latency_large_file` | 26149 | 0 | 0 % | 0 |
| `git.refresh_dispatch` | `git_sidebar_refresh_large_repo` | 47704 | 0 | 0 % | 0 |
| `git.refresh_dispatch` | `git_sidebar_refresh_many_untracked` | 104578 | 0 | 0 % | 0 |
| `deep_restore.jump_and_first_paint` | `large_file_restore_deep_scroll_first_paint` | 1224 | 0 | 0 % | 0 |
| `linter.save` | `linter_on_save` | 523 | 0 | 0 % | 0 |
| `linter.wait_diagnostics` | `linter_on_save` | 9496 | 0 | 0 % | 0 |
| `lsp_document_symbols.parse` | `lsp_document_symbols_parse` | 600400 | 0 | 0 % | 0 |
| `lsp_message_framing.parse` | `lsp_message_framing` | 160000 | 0 | 0 % | 560 |
| `lsp_diagnostics.parse` | `lsp_publish_diagnostics_parse` | 400400 | 0 | 0 % | 0 |
| `lsp_semantic_tokens.decode` | `lsp_semantic_tokens_decode` | 400 | 0 | 0 % | 0 |
| `menu_hover_switch.160_moves` | `menu_hover_switch` | 118 | 0 | 0 % | 0 |
| `menu_popup_hover_rows.160_moves` | `menu_popup_hover_rows` | 0 | 0 | 0 % | 0 |
| `merge.accept_interleaved_burst` | `merge_accept_hunk_interleaved` | 8726 | 0 | 0 % | 0 |
| `merge.edit_result_scroll` | `merge_edit_result_then_scroll` | 42148 | 0 | 0 % | 0 |
| `merge_model.build_interleaved` | `merge_model_build_interleaved` | 24594 | 0 | 0 % | 0 |
| `merge.open_interleaved` | `merge_next_conflict_large_file` | 27832 | 0 | 0 % | 0 |
| `merge.next_conflict_burst` | `merge_next_conflict_large_file` | 47086 | 0 | 0 % | 0 |
| `merge.open_many_conflicts` | `merge_open_many_conflicts` | 16123 | 0 | 0 % | 0 |
| `merge_interleaved.open_to_first_paint` | `merge_scroll_interleaved_hunks` | 16739 | 0 | 0 % | 0 |
| `merge_interleaved.scroll_burst` | `merge_scroll_interleaved_hunks` | 8754 | 0 | 0 % | 0 |
| `merge_large.open_to_first_paint` | `merge_scroll_large_fixture` | 56296 | 0 | 0 % | 0 |
| `merge_large.scroll_burst` | `merge_scroll_large_fixture` | 31650 | 0 | 0 % | 0 |
| `mid_file_edit.enter_backspace_burst` | `mid_file_edit_latency_large_file` | 21999 | 0 | 0 % | 0 |
| `multi_caret.remap_fast` | `multi_caret_remap_burst` | 0 | 0 | 0 % | 80 |
| `multi_caret.remap_fallback` | `multi_caret_remap_burst` | 0 | 0 | 0 % | 40 |
| `multi_tab.cycle_tabs` | `multi_tab_cycle` | 20186 | 0 | 0 % | 0 |
| `plugin_caps.keybinding_resolve` | `plugin_keybindings_resolve_at_cap` | 696 | 0 | 0 % | 0 |
| `status_registry.apply_update` | `plugin_status_item_update` | 16002 | 0 | 0 % | 0 |
| `plugin_caps.status_resolve` | `plugin_status_items_resolve_at_cap` | 8196 | 0 | 0 % | 0 |
| `reference_snippet.read_window` | `reference_snippet_file_window` | 3200 | 0 | 0 % | 0 |
| `settings.apply_cheap_family_all_tabs` | `settings_change_many_tabs` | 11962 | 0 | 0 % | 0 |
| `settings.apply_contract_family_all_tabs` | `settings_change_many_tabs` | 12712 | 0 | 0 % | 0 |
| `settings_overlay.rebuild` | `settings_rows_rebuild` | 6054 | 0 | 0 % | 0 |
| `settings_overlay.category_walk` | `settings_rows_rebuild` | 0 | 0 | 0 % | 0 |
| `syntax.advance_state_cpp_lines` | `syntax_advance_state_cpp_lines` | 136 | 0 | 0 % | 0 |
| `syntax.highlight_cpp_lines` | `syntax_highlight_cpp_lines` | 8132 | 0 | 0 % | 0 |
| `syntax.highlight_python_lines` | `syntax_highlight_python_lines` | 8136 | 0 | 0 % | 0 |
| `terminal.fill_scrollback` | `terminal_alt_screen_toggle` | 6415 | 0 | 0 % | 0 |
| `terminal.alt_toggle_burst` | `terminal_alt_screen_toggle` | 907 | 0 | 0 % | 0 |
| `terminal.feed_output` | `terminal_scroll_long_output` | 12796 | 0 | 0 % | 0 |
| `terminal.scroll_burst` | `terminal_scroll_long_output` | 4032 | 0 | 0 % | 0 |
| `persistence.user_config_decode` | `user_config_record_decode` | 2119200 | 0 | 0 % | 0 |
| `resize.compact_to_regular` | `window_resize_stress` | 2400 | 0 | 0 % | 0 |
