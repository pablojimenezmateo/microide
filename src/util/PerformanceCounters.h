#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::util {

// Free-running process-wide event counters. Cheap enough (one relaxed atomic
// add) to leave armed in release builds, which is the point: they answer "how
// many times did this actually run?" from a real session, where a sampling
// profiler only says "this was hot".
//
// The id and its wire name are declared once, here. They used to live in two
// parallel lists -- an enum here and a positionally-indexed name table in the
// .cpp -- so inserting an id without inserting its name at the same position
// still compiled and silently relabelled every counter after that point,
// attributing one subsystem's numbers to another. A test existed only to catch
// that; the X-macro removes the failure mode instead of guarding it.
//
// Naming: "<subsystem>.<event>". Counters ending in a plural noun count that
// noun (lines, bytes, cells); everything else counts calls or events.
#define MICROIDE_PERF_COUNTERS(X)                                                              \
  X(FramePrepareCalls, "frame.prepare_calls")                                                  \
  X(FrameRefreshEditorFoldingModelsCalls, "frame.refresh_editor_folding_models_calls")          \
  X(FrameApplyEditorPreferencesAllTabsCalls, "frame.apply_editor_preferences_all_tabs_calls")   \
  X(FrameRefreshStatusBarCalls, "frame.refresh_status_bar_calls")                               \
  X(RenderBuildEditorViewModelCalls, "render.build_editor_view_model_calls")                    \
  X(EditorRefreshEncodingCalls, "editor.refresh_encoding_calls")                                \
  X(EditorInvalidateDerivedCachesCalls, "editor.invalidate_derived_caches_calls")               \
  X(EditorInvalidateDerivedCachesLines, "editor.invalidate_derived_caches_lines")               \
  X(EditorFoldRefreshCalls, "editor.fold_refresh_calls")                                        \
  X(EditorFoldWindowLinesWalked, "editor.fold_window_lines_walked")                             \
  X(EditorFoldBlockWordsApplied, "editor.fold_block_words_applied")                             \
  X(EditorFoldBlockSummariesBuilt, "editor.fold_block_summaries_built")                         \
  X(EditorFoldBlockSummaryLines, "editor.fold_block_summary_lines")                             \
  /* Block word lists stored, and total entries across them. entries/stored is the    */        \
  /* mean word length -- ~32 on the 50k-line C++ fixture, not the handful             */        \
  /* TD-2026-08-06-144 assumed. That measurement is why inlining these into Block     */        \
  /* was measured and rejected: an inline capacity big enough to matter costs        */        \
  /* ~420 KB per open tab. Keep both so nobody has to re-derive it.                   */        \
  X(EditorFoldBlockWordsStored, "editor.fold_block_words_stored")                                \
  X(EditorFoldBlockWordEntries, "editor.fold_block_word_entries")                                \
  X(EditorFoldBracketLinesScanned, "editor.fold_bracket_lines_scanned")                         \
  X(EditorFoldBracketLinesSkippedTooLong, "editor.fold_bracket_lines_skipped_too_long")         \
  X(EditorBracketMatchLineTooLong, "editor.bracket_match_line_too_long")                       \
  X(EditorFoldIndentLinesMeasured, "editor.fold_indent_lines_measured")                         \
  X(EditorFoldPartialResolves, "editor.fold_partial_resolves")                                  \
  X(EditorVisibleLineLayoutEvictions, "editor.visible_line_layout_evictions")                    \
  X(EditorVisibleLineLayoutRecycled, "editor.visible_line_layout_recycled")                      \
  X(EditorVisibleLineLayoutPrefixBytesScanned,                                                   \
    "editor.visible_line_layout_prefix_bytes_scanned")                                           \
  X(EditorVisualColumnWalkBytes, "editor.visual_column_walk_bytes")                              \
  /* Bytes the render-whitespace marker walk visits, summed over visible rows, by    */          \
  /* BOTH producers (RenderViewModelBuilder's CSR run builder and EditorViewRenderer's*/         \
  /* text-iteration fallback). Each walks one logical line per visible row, so under  */         \
  /* soft wrap a walk that restarts at byte 0 is quadratic in the rows of one wrapped */         \
  /* line -- a wrapped megabyte line is thousands of rows. Both resume at the row's   */         \
  /* own start when the bytes before it are plain single-cell ASCII, and this counter */         \
  /* is what makes "resumed" a testable claim rather than a comment                   */          \
  /* (TD-2026-08-12-187). Bumped once per row, not per glyph.                         */          \
  X(EditorWhitespaceMarkerWalkBytes, "editor.whitespace_marker_walk_bytes")                      \
  /* What an open editor tab costs to KEEP open, in retained heap bytes, broken   */             \
  /* down by which cache holds it. Bumped ONLY by the measurement surface         */             \
  /* (WorkspaceShell::TestAccess::EditorDerivedCacheResidency, driven by the      */             \
  /* editor_tab_derived_cache_residency perf scenario and its unit tests) -- no   */             \
  /* production path computes these, so they cost a shipped build nothing.        */             \
  /* Each is individually bounded BY ITS DOCUMENT and none is bounded across      */             \
  /* tabs; a cap for the sum cannot be chosen without knowing which dominates,    */             \
  /* which is what TD-2026-08-06-142 filed and this reports.                      */             \
  X(EditorTabDerivedCacheBytes, "editor.tab_derived_cache_bytes")                                \
  X(EditorTabDerivedCacheLayoutBytes, "editor.tab_derived_cache_layout_bytes")                   \
  X(EditorTabDerivedCacheHighlightTokenBytes,                                                    \
    "editor.tab_derived_cache_highlight_token_bytes")                                            \
  X(EditorTabDerivedCacheHighlightStateBytes,                                                    \
    "editor.tab_derived_cache_highlight_state_bytes")                                            \
  X(EditorTabDerivedCacheCaretBytes, "editor.tab_derived_cache_caret_bytes")                     \
  X(EditorTabDerivedCacheUndoBytes, "editor.tab_derived_cache_undo_bytes")                       \
  X(EditorTabDerivedCacheFoldBytes, "editor.tab_derived_cache_fold_bytes")                       \
  X(EditorTabDerivedCacheTabs, "editor.tab_derived_cache_tabs")                                  \
  X(EditorLineMaterializations, "editor.line_materializations")                                  \
  X(EditorLineMaterializedBytes, "editor.line_materialized_bytes")                               \
  X(EditorLineWidthFullMeasures, "editor.line_width_full_measures")                              \
  X(EditorLineWidthSpliceUpdates, "editor.line_width_splice_updates")                            \
  /* Why the whole-document width table was rebuilt. full_measures alone says how many  */       \
  /* lines were walked but not whether that was one build or three, nor what invalidated */      \
  /* the previous one -- which is how a large-file open paid the O(document) walk twice   */      \
  /* with nothing to point at (TD-2026-08-06-138). Exactly one reason counter is bumped   */      \
  /* per build, so the four sum to table_builds.                                           */     \
  X(EditorLineWidthTableBuilds, "editor.line_width_table_builds")                                \
  /* No table at all: first build for this tab, or the first after an invalidation. */           \
  X(EditorLineWidthRebuildCold, "editor.line_width_rebuild_cold")                                \
  /* A table existed and described the same document, but at a different tab size. */            \
  X(EditorLineWidthRebuildTabSize, "editor.line_width_rebuild_tab_size")                         \
  /* A table existed for a different line count -- an edit the incremental path dropped. */      \
  X(EditorLineWidthRebuildLineCount, "editor.line_width_rebuild_line_count")                     \
  /* A table existed, same tab size and same line count, but was stamped for an older     */     \
  /* content revision -- i.e. the document changed under it and no edit path spliced or   */     \
  /* dropped it. Every edit path in the tree does one or the other, so this MUST read 0;  */     \
  /* a non-zero value names a path that mutates content without maintaining the table     */     \
  /* (TD-2026-08-06-143). Before that entry, MaxVisualColumns did not check the revision  */     \
  /* and stamped the stale table as current, so such a path was silent forever.           */     \
  X(EditorLineWidthRebuildStaleRevision, "editor.line_width_rebuild_stale_revision")             \
  /* The widest-line scan over the width table. Separate from the build because it also  */      \
  /* runs on its own, whenever an edit invalidated the memoized max without invalidating */      \
  /* the table -- an O(document) deque walk that nothing counted. scans minus            */      \
  /* table_builds is exactly how many of those standalone rescans happened.              */      \
  X(EditorLineWidthMaxScans, "editor.line_width_max_scans")                                      \
  X(EditorLineWidthMaxScanLines, "editor.line_width_max_scan_lines")                             \
  X(EditorContentRevisionBumps, "editor.content_revision_bumps")                                \
  X(EditorSyntaxRevisionBumps, "editor.syntax_revision_bumps")                                  \
  X(EditorLayoutShapeRevisionBumps, "editor.layout_shape_revision_bumps")                       \
  X(EditorPresentationRevisionBumps, "editor.presentation_revision_bumps")                      \
  X(EditorEnsureWrappedRowLayoutsRebuilds, "editor.ensure_wrapped_row_layouts_rebuilds")        \
  X(EditorEnsureWrappedRowLayoutsLineVisits, "editor.ensure_wrapped_row_layouts_line_visits")   \
  /* Soft-wrap caret->visual-row resolutions. Each one is a binary search over the       */      \
  /* caret line's rows, but it is called several times per keystroke (preferred column,  */      \
  /* EnsureCursorVisible, the render pass) and per frame, so a regression that turns a   */      \
  /* per-frame resolve into a per-visible-row one shows up here and nowhere else.        */      \
  X(EditorWrapCaretRowResolves, "editor.wrap_caret_row_resolves")                                \
  /* Bytes walked purely to learn a line's visual width while BUILDING a visible row,   */      \
  /* i.e. rows built without the width their caller already knew. Nonzero on a steady   */      \
  /* frame means some render path is re-measuring whole lines per row.                  */      \
  X(EditorLineWidthMeasureBytes, "editor.line_width_measure_bytes")                              \
  X(EditorFiletypeMemoQueries, "editor.filetype_memo_queries")                                  \
  X(EditorFiletypeMemoHits, "editor.filetype_memo_hits")                                        \
  X(LanguageContractViewQueries, "language_contract.view_queries")                              \
  X(LanguageContractViewBuilds, "language_contract.view_builds")                                \
  X(TerminalSnapshotLineRangeIfChangedCalls, "terminal.snapshot_line_range_if_changed_calls")   \
  X(TerminalSnapshotLineRangeIfChangedCopiedLines,                                              \
    "terminal.snapshot_line_range_if_changed_copied_lines")                                     \
  X(TerminalSnapshotLineRangeIfChangedCopiedCells,                                              \
    "terminal.snapshot_line_range_if_changed_copied_cells")                                     \
  X(TerminalOutputBytesParsed, "terminal.output_bytes_parsed")                                  \
  X(TerminalTrimScrollbackCalls, "terminal.trim_scrollback_calls")                              \
  X(TerminalTrimScrollbackLines, "terminal.trim_scrollback_lines")                              \
  X(SearchProjectProgressPublishes, "search.project_progress_publishes")                        \
  X(SearchProjectLowerLineCalls, "search.project_lower_line_calls")                             \
  X(SearchProjectLowerLineBytes, "search.project_lower_line_bytes")                             \
  X(SearchProjectCandidateFilesFromIndex, "search.project_candidate_files_from_index")          \
  X(SearchProjectScopeFilteredFiles, "search.project_scope_filtered_files")                     \
  /* Candidate files the display cap left unclaimed, i.e. never opened or scanned. */           \
  /* The blind spot this fills: candidate-files-from-index says how much work a search */       \
  /* was handed and scope-filtered says how much scoping removed, but neither says how */       \
  /* much the early stop skipped -- so a search that answered from 4% of the project */         \
  /* and one that scanned all of it read identically. Nonzero here is also exactly the */       \
  /* condition that makes a result set truncated (ProjectSearchService::RunSearch). */          \
  X(SearchProjectCapUnscannedFiles, "search.project_cap_unscanned_files")                       \
  /* Replace-all's candidate set and how much of it it actually opened. These are  */           \
  /* the only counters the replace path touches, and nothing else touches them --  */           \
  /* which is the point: the shared ReadFileForTextSearch counter is also bumped   */           \
  /* by watcher-triggered rescans, so measuring replace-all through it meant       */           \
  /* subtracting two windows a background subsystem could perturb (TD-2026-07-26-  */           \
  /* 005). candidates == the matched-file subset on the fast path and the whole    */           \
  /* indexed catalog on the fallback, so candidates >> files_read never happens    */           \
  /* and candidates ~= project size is exactly the regression that path exists to  */           \
  /* prevent. */                                                                                \
  X(SearchProjectReplaceCandidateFiles, "search.project_replace_candidate_files")               \
  X(SearchProjectReplaceFilesRead, "search.project_replace_files_read")                         \
  X(FileFinderCacheBuildCalls, "search.file_finder_cache_build_calls")                          \
  X(FileFinderCacheEntriesBuilt, "search.file_finder_cache_entries_built")                      \
  /* Bytes the finder's candidate blobs hold after a rebuild (path + folded     */              \
  /* path, packed). The finder's resident cost used to be invisible: it was two */              \
  /* std::strings per indexed file and nothing counted them (TD-2026-08-06-154).*/              \
  X(FileFinderCacheBytes, "search.file_finder_cache_bytes")                                     \
  /* Per-keystroke ranking work. `candidates_scanned` is what the scan actually  */             \
  /* looked at, so `scanned / (refreshes * indexed)` is how much the             */             \
  /* forward-typing narrowing is worth; `mask_rejects` is the share of those the */             \
  /* O(1) presence mask killed before any subsequence scan.                      */             \
  X(FileFinderRefreshCalls, "search.file_finder_refresh_calls")                                 \
  X(FileFinderCandidatesScanned, "search.file_finder_candidates_scanned")                       \
  X(FileFinderMaskRejects, "search.file_finder_mask_rejects")                                   \
  X(FileFinderNarrowedRefreshes, "search.file_finder_narrowed_refreshes")                       \
  X(ProjectFileScannerCollectProjectFilesCalls, "project.collect_project_files_calls")          \
  X(RenderTextWidthCacheQueries, "render.text_width_cache_queries")                             \
  X(RenderTextWidthCacheHits, "render.text_width_cache_hits")                                   \
  X(RenderTextTextureCacheHits, "render.text_texture_cache_hits")                               \
  X(RenderTextTextureCacheMisses, "render.text_texture_cache_misses")                           \
  X(RenderTextTextureCacheEvictions, "render.text_texture_cache_evictions")                     \
  X(RenderViewModelBuildFrameSurfaceCalls, "render.view_model_build_frame_surface_calls")       \
  X(RenderViewModelBuildOverlaySurfaceCalls, "render.view_model_build_overlay_surface_calls")   \
  X(EditorHighlightCacheForcedMisses, "editor.highlight_cache_forced_misses")                   \
  X(EditorHighlightCacheEvictions, "editor.highlight_cache_evictions")                          \
  /* Evictions that handed their MAP NODE (and the token vector it owns) straight  */           \
  /* to the line replacing it, instead of freeing one and allocating the other. A  */           \
  /* scroll misses on every newly visible line, so at the cache cap that was two   */           \
  /* allocations per line per frame -- the #1 and #2 sites of several scroll        */          \
  /* phases (TD-2026-08-14-219). Should equal `_evictions` in the steady state; a   */          \
  /* gap means the rekey path stopped being taken.                                 */           \
  X(EditorHighlightCacheNodesRecycled, "editor.highlight_cache_nodes_recycled")                  \
  X(RenderClipInvocations, "render.clip_invocations")                                           \
  X(WorkspaceScheduledWakes, "workspace.scheduled_wakes")                                       \
  /* Damage rects queued per event, and how often the per-event list outgrew its  */             \
  /* inline capacity (kInlineRedrawRects). rects_queued is the input-path work    */             \
  /* nothing else reports -- a hover that repaints four controls and one that     */             \
  /* repaints one read identically in every other counter -- and spills is the    */             \
  /* check on the inline size: nonzero means the common case is paying a heap     */             \
  /* round-trip per event again, which is exactly what SmallVector removed.       */             \
  X(WorkspaceRedrawRectsQueued, "workspace.redraw_rects_queued")                                \
  X(WorkspaceRedrawRectSpills, "workspace.redraw_rect_spills")                                  \
  /* Damage AREA, in logical pixels, and how often an event gave up on rects and  */            \
  /* asked for the whole window. rects_queued counts how many rects an event      */            \
  /* produced; it cannot tell a 40x1000 tab strip from a 1920x1080 window, and    */            \
  /* those are the two outcomes a drag chooses between. Repaint scope is the      */            \
  /* change with the largest real-world effect on a drag and the one a headless   */            \
  /* event-cost measurement cannot see at all (TD-2026-08-14-212); these are how  */            \
  /* a perf scenario sees it. A full request contributes no pixels here -- it is  */            \
  /* counted in its own counter precisely so "many pixels" and "gave up" stay     */            \
  /* distinguishable rather than summing into one number.                          */          \
  X(WorkspaceRedrawRectPixels, "workspace.redraw_rect_pixels")                                  \
  X(WorkspaceFullRedrawRequests, "workspace.full_redraw_requests")                              \
  /* How many times the menu bar was laid out, and how many label widths that     */             \
  /* actually measured. The layout runs from at least three independent chains    */             \
  /* per pointer motion (hit test, cursor kind, chrome redraw rect) -- about ten  */             \
  /* rebuilds an event -- and nothing counted it: the cost was only visible under */             \
  /* an allocation trace, which is how it survived a previous optimisation pass   */             \
  /* on the same function (TD-2026-08-06-149). Layouts is the recompute rate that */             \
  /* a memo would have to beat; label_measures is the check that the width cache  */             \
  /* is holding, and should stay at zero after the first frame.                   */             \
  X(WorkspaceMenuBarLayouts, "workspace.menu_bar_layouts")                                      \
  X(WorkspaceMenuBarLabelMeasures, "workspace.menu_bar_label_measures")                         \
  X(WorkspaceWakeReasonPluginReload, "workspace.wake_reason_plugin_reload")                     \
  X(WorkspaceWakeReasonCaretBlink, "workspace.wake_reason_caret_blink")                         \
  X(WorkspaceWakeReasonNone, "workspace.wake_reason_none")                                      \
  X(TerminalScrollbackLinesAllocated, "terminal.scrollback_lines_allocated")                    \
  X(TerminalEscapeSequencesAborted, "terminal.escape_sequences_aborted")                        \
  X(RenderGlyphAtlasRuns, "render.glyph_atlas_runs")                                            \
  X(RenderGlyphAtlasGlyphs, "render.glyph_atlas_glyphs")                                        \
  X(RenderGlyphAtlasFallbacks, "render.glyph_atlas_fallbacks")                                  \
  /* --- project-change fan-out (shell thread; a branch switch is a burst) - */                 \
  X(EditorConfigResolveQueries, "editorconfig.resolve_queries")                                 \
  X(EditorConfigResolveMisses, "editorconfig.resolve_misses")                                   \
  X(EditorConfigDirectoryReads, "editorconfig.directory_reads")                                 \
  X(EditorConfigInvalidations, "editorconfig.invalidations")                                    \
  X(LspWatchedFileChangesConsidered, "lsp.watched_file_changes_considered")                     \
  X(LspWatchedFileGlobTests, "lsp.watched_file_glob_tests")                                     \
  X(LspWatchedFileEventsSent, "lsp.watched_file_events_sent")                                   \
  X(LspWatchedFileNotifiedServers, "lsp.watched_file_notified_servers")                         \
  /* --- subprocess / external tools ------------------------------------- */                  \
  X(SubprocessSpawns, "subprocess.spawns")                                                      \
  X(SubprocessWaitMs, "subprocess.wait_ms")                                                     \
  X(SubprocessOutputBytes, "subprocess.output_bytes")                                           \
  X(SubprocessTimeouts, "subprocess.timeouts")                                                  \
  /* --- git ------------------------------------------------------------- */                  \
  X(GitCommandsRun, "git.commands_run")                                                         \
  X(GitCommandOutputBytes, "git.command_output_bytes")                                          \
  X(GitCommandFailures, "git.command_failures")                                                 \
  X(GitStatusRefreshCalls, "git.status_refresh_calls")                                          \
  X(GitStatusEntriesParsed, "git.status_entries_parsed")                                        \
  X(GitBlameQueries, "git.blame_queries")                                                       \
  X(GitBlameCacheHits, "git.blame_cache_hits")                                                  \
  /* Blame requests that reached the git probes (rev-parse/ls-files/status) vs.  */             \
  /* those the re-validation throttle answered from cache. The overlay asks once  */            \
  /* per frame, so the ratio is what says whether inline blame is spawning        */            \
  /* processes at frame rate.                                                     */            \
  X(GitBlameValidationProbes, "git.blame_validation_probes")                                    \
  X(GitBlameValidationSkips, "git.blame_validation_skips")                                      \
  /* Blame line-ranges actually merged into the per-file cache. Probes without   */             \
  /* loads is the pathological shape: work is being paid for and thrown away, so */             \
  /* loaded_spans never grows and the throttle above never gets to fire.         */             \
  X(GitBlameSpansLoaded, "git.blame_spans_loaded")                                              \
  X(GitDiffLoads, "git.diff_loads")                                                             \
  X(GitDiffBytesRead, "git.diff_bytes_read")                                                    \
  /* --- compare / merge diff pipeline ------------------------------------ */                 \
  X(CompareModelBuilds, "compare.model_builds")                                                 \
  X(CompareModelInputLines, "compare.model_input_lines")                                        \
  X(CompareModelRowsProduced, "compare.model_rows_produced")                                    \
  X(CompareIntralineDiffLines, "compare.intraline_diff_lines")                                  \
  X(MergeModelBuilds, "merge.model_builds")                                                     \
  X(MergeModelConflictsFound, "merge.model_conflicts_found")                                    \
  /* --- persisted state -------------------------------------------------- */                 \
  X(PersistenceRecordWrites, "persistence.record_writes")                                       \
  X(PersistenceRecordBytesWritten, "persistence.record_bytes_written")                          \
  X(PersistenceRecordReads, "persistence.record_reads")                                         \
  X(PersistenceRecordBytesRead, "persistence.record_bytes_read")                                \
  /* --- plugin runtime --------------------------------------------------- */                 \
  X(PluginLuaCallbackDispatches, "plugin.lua_callback_dispatches")                              \
  X(PluginLuaCallbackErrors, "plugin.lua_callback_errors")                                      \
  /* --- language server / debug adapter ---------------------------------- */                 \
  X(LspMessagesSent, "lsp.messages_sent")                                                       \
  X(LspBytesSent, "lsp.bytes_sent")                                                             \
  X(LspMessagesReceived, "lsp.messages_received")                                               \
  X(LspBytesReceived, "lsp.bytes_received")                                                     \
  X(DapMessagesSent, "dap.messages_sent")                                                       \
  X(DapBytesSent, "dap.bytes_sent")                                                             \
  X(DapMessagesReceived, "dap.messages_received")                                               \
  X(DapBytesReceived, "dap.bytes_received")                                                     \
  /* --- filesystem watch / index ----------------------------------------- */                 \
  X(FileWatcherWakes, "watch.wakes")                                                            \
  X(FileWatcherPollScans, "watch.poll_scans")                                                    \
  X(FileWatcherEventsCoalesced, "watch.events_coalesced")                                       \
  X(FileIndexApplyBatchCalls, "watch.file_index_apply_batch_calls")                             \
  X(FileIndexRebuilds, "watch.file_index_rebuilds")                                             \
  /* --- background work -------------------------------------------------- */                 \
  X(TaskExecutorTasksEnqueued, "task.enqueued")                                                 \
  X(TaskExecutorTasksRun, "task.run")                                                           \
  X(MainThreadMailboxPosts, "task.main_thread_mailbox_posts")                                   \
  X(MainThreadMailboxPostsCoalesced, "task.main_thread_mailbox_posts_coalesced")                \
  X(MainThreadMailboxDrains, "task.main_thread_mailbox_drains")                                 \
  /* --- editor multi-caret ------------------------------------------------ */                 \
  /* Box/column selection builds one caret per spanned line, which is the only  */             \
  /* place a single gesture can allocate carets proportional to the file. The    */             \
  /* 10,000-caret cap in SetBoxSelection bounds it; these say whether a real     */             \
  /* gesture is anywhere near that, and carets_placed/builds is the mean span.   */             \
  X(EditorBoxSelectionBuilds, "editor.box_selection_builds")                                   \
  X(EditorBoxSelectionCaretsPlaced, "editor.box_selection_carets_placed")                      \
  /* Lines scanned to bound a keyboard column selection's virtual column. This   */             \
  /* is O(span) per Right keystroke, so it is the one repeated cost the keyboard */             \
  /* gesture adds over the mouse one.                                            */             \
  X(EditorBoxSelectionSpanLinesScanned, "editor.box_selection_span_lines_scanned")             \
  /* Multi-range undo entries (TD-2026-08-06-157) and the two line counts that   */             \
  /* say what the shape is worth: `lines_kept` is what the entry actually holds  */             \
  /* (both images of every edited range), `lines_spanned` is what the old        */             \
  /* one-contiguous-range model would have held for the same edit. Their ratio   */             \
  /* is the span/edited ratio nothing reported before, and it is what says       */             \
  /* whether real carets sit 30 lines or a whole file apart.                     */             \
  X(EditorMultiRangeUndoEntries, "editor.multi_range_undo_entries")                             \
  X(EditorMultiRangeUndoLinesKept, "editor.multi_range_undo_lines_kept")                        \
  X(EditorMultiRangeUndoLinesSpanned, "editor.multi_range_undo_lines_spanned")                  \
  /* --- text document model ---------------------------------------------- */                 \
  X(DocumentEdits, "document.edits")                                                            \
  X(DocumentAddBufferCompactions, "document.add_buffer_compactions")                            \
  X(DocumentFullTextMaterializations, "document.full_text_materializations")                    \
  X(DocumentFullTextBytes, "document.full_text_bytes")

enum class PerfCounterId : std::size_t {
#define MICROIDE_PERF_COUNTER_ENUM(id, name) id,
  MICROIDE_PERF_COUNTERS(MICROIDE_PERF_COUNTER_ENUM)
#undef MICROIDE_PERF_COUNTER_ENUM
      Count,
};

constexpr std::size_t kPerfCounterCount = static_cast<std::size_t>(PerfCounterId::Count);

using PerfCounterSnapshot = std::array<std::uint64_t, kPerfCounterCount>;

void ResetPerformanceCounters();
void AddPerformanceCounter(PerfCounterId id, std::uint64_t delta = 1);
std::uint64_t ReadPerformanceCounter(PerfCounterId id);
PerfCounterSnapshot CapturePerformanceCounters();
std::vector<std::pair<std::string_view, std::uint64_t>> NonZeroCounterDelta(
    const PerfCounterSnapshot& before,
    const PerfCounterSnapshot& after);
std::string_view PerformanceCounterName(PerfCounterId id);

// Write every non-zero counter to `out`, sorted by name. This is the live-app
// readout: before it existed the counters could only be observed from the perf
// harness, so a real session's numbers were unreachable.
void WritePerformanceCounters(std::FILE* out);

// True when MICROIDE_PERF_COUNTERS is set. Callers use it to arm a shutdown dump.
bool PerformanceCounterDumpRequested();

// Idempotent shutdown dump, gated on PerformanceCounterDumpRequested().
void DumpPerformanceCountersOnce();

}  // namespace microide::util
