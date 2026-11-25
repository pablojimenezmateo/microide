#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::util {

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
          \
          \
          \
          \
          \
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
            \
            \
            \
            \
            \
            \
            \
            \
            \
            \
            \
            \
            \
  X(EditorPlainPrefixMemoHits, "editor.plain_prefix_memo_hits")                                  \
  X(EditorPlainPrefixMemoExtends, "editor.plain_prefix_memo_extends")                            \
  X(EditorPlainPrefixMemoCold, "editor.plain_prefix_memo_cold")                                  \
  X(EditorVisualColumnWalkFactsUnknown, "editor.visual_column_walk_facts_unknown")               \
  X(EditorVisualColumnWalkNotPlainAscii, "editor.visual_column_walk_not_plain_ascii")            \
            \
           \
           \
           \
           \
           \
            \
            \
  X(EditorWhitespaceMarkerWalkBytes, "editor.whitespace_marker_walk_bytes")                      \
             \
             \
             \
             \
             \
             \
             \
  X(EditorWhitespaceMarkerPrefixBytesScanned,                                                     \
    "editor.whitespace_marker_prefix_bytes_scanned")                                              \
             \
             \
             \
  X(EditorWhitespaceMarkerRowsCarried, "editor.whitespace_marker_rows_carried")                   \
               \
               \
               \
               \
               \
               \
               \
               \
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
         \
        \
        \
        \
       \
  X(EditorLineWidthTableBuilds, "editor.line_width_table_builds")                                \
          \
          \
          \
  X(EditorHistoryCaretCopies, "editor.history_caret_copies")                                     \
             \
  X(EditorLineWidthRebuildCold, "editor.line_width_rebuild_cold")                                \
              \
  X(EditorLineWidthRebuildTabSize, "editor.line_width_rebuild_tab_size")                         \
        \
  X(EditorLineWidthRebuildLineCount, "editor.line_width_rebuild_line_count")                     \
       \
       \
       \
       \
       \
       \
  X(EditorLineWidthRebuildStaleRevision, "editor.line_width_rebuild_stale_revision")             \
        \
        \
        \
        \
  X(EditorLineWidthMaxScans, "editor.line_width_max_scans")                                      \
  X(EditorLineWidthMaxScanLines, "editor.line_width_max_scan_lines")                             \
        \
        \
        \
        \
  X(EditorLineWidthMaxScanBlocks, "editor.line_width_max_scan_blocks")                           \
        \
        \
        \
        \
  X(EditorLineWidthBlockRescans, "editor.line_width_block_rescans")                              \
        \
        \
        \
        \
  X(EditorLineWidthMaxKeptOnReplace, "editor.line_width_max_kept_on_replace")                    \
        \
        \
        \
        \
        \
  X(EditorLineWidthMaxDroppedReplacedNarrower,                                                   \
    "editor.line_width_max_dropped_replaced_narrower")                                           \
  X(EditorLineWidthMaxDroppedAlreadyAbsent,                                                      \
    "editor.line_width_max_dropped_already_absent")                                              \
  X(EditorContentRevisionBumps, "editor.content_revision_bumps")                                \
  X(EditorSyntaxRevisionBumps, "editor.syntax_revision_bumps")                                  \
  X(EditorLayoutShapeRevisionBumps, "editor.layout_shape_revision_bumps")                       \
  X(EditorPresentationRevisionBumps, "editor.presentation_revision_bumps")                      \
  X(EditorEnsureWrappedRowLayoutsRebuilds, "editor.ensure_wrapped_row_layouts_rebuilds")        \
  X(EditorEnsureWrappedRowLayoutsLineVisits, "editor.ensure_wrapped_row_layouts_line_visits")   \
        \
        \
        \
        \
  X(EditorWrapCaretRowResolves, "editor.wrap_caret_row_resolves")                                \
        \
        \
        \
  X(EditorLineWidthMeasureBytes, "editor.line_width_measure_bytes")                              \
  X(EditorFiletypeMemoQueries, "editor.filetype_memo_queries")                                  \
  X(EditorFiletypeMemoHits, "editor.filetype_memo_hits")                                        \
        \
        \
        \
        \
        \
        \
  X(EditorFiletypeSignatureScans, "editor.filetype_signature_scans")                            \
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
              \
              \
              \
              \
              \
              \
              \
                                                            \
  X(SearchBufferRefreshes, "search.buffer_refreshes")                                           \
  X(SearchBufferRefreshesAfterEdit, "search.buffer_refreshes_after_edit")                       \
  X(SearchBufferRefreshLinesScanned, "search.buffer_refresh_lines_scanned")                     \
  X(SearchProjectProgressPublishes, "search.project_progress_publishes")                        \
  X(SearchProjectLowerLineCalls, "search.project_lower_line_calls")                             \
  X(SearchProjectLowerLineBytes, "search.project_lower_line_bytes")                             \
  X(SearchProjectCandidateFilesFromIndex, "search.project_candidate_files_from_index")          \
  X(SearchProjectScopeFilteredFiles, "search.project_scope_filtered_files")                     \
             \
         \
         \
           \
         \
            \
  X(SearchProjectCapUnscannedFiles, "search.project_cap_unscanned_files")                       \
             \
             \
             \
             \
             \
             \
             \
             \
                                                                                  \
  X(SearchProjectReplaceCandidateFiles, "search.project_replace_candidate_files")               \
  X(SearchProjectReplaceFilesRead, "search.project_replace_files_read")                         \
  X(FileFinderCacheBuildCalls, "search.file_finder_cache_build_calls")                          \
  X(FileFinderCacheEntriesBuilt, "search.file_finder_cache_entries_built")                      \
                \
                \
                \
  X(FileFinderCacheBytes, "search.file_finder_cache_bytes")                                     \
               \
               \
               \
               \
  X(FileFinderRefreshCalls, "search.file_finder_refresh_calls")                                 \
  X(FileFinderCandidatesScanned, "search.file_finder_candidates_scanned")                       \
  X(FileFinderMaskRejects, "search.file_finder_mask_rejects")                                   \
  X(FileFinderNarrowedRefreshes, "search.file_finder_narrowed_refreshes")                       \
  X(ProjectFileScannerCollectProjectFilesCalls, "project.collect_project_files_calls")          \
               \
               \
               \
               \
               \
               \
               \
               \
               \
               \
               \
  X(ProjectIgnoreFilterQueries, "project.ignore_filter_queries")                                \
  X(ProjectIgnoreFilterRuleSetEvaluations, "project.ignore_filter_rule_set_evaluations")        \
              \
              \
  X(ProjectIgnoreFilterAncestorScans, "project.ignore_filter_ancestor_scans")                   \
  X(RenderTextWidthCacheQueries, "render.text_width_cache_queries")                             \
  X(RenderTextWidthCacheHits, "render.text_width_cache_hits")                                   \
  X(RenderTextTextureCacheHits, "render.text_texture_cache_hits")                               \
  X(RenderTextTextureCacheMisses, "render.text_texture_cache_misses")                           \
  X(RenderTextTextureCacheEvictions, "render.text_texture_cache_evictions")                     \
               \
               \
               \
                \
  X(RenderTextTextureCacheRecycled, "render.text_texture_cache_recycled")                       \
  X(RenderViewModelBuildFrameSurfaceCalls, "render.view_model_build_frame_surface_calls")       \
  X(RenderViewModelBuildOverlaySurfaceCalls, "render.view_model_build_overlay_surface_calls")   \
  X(EditorHighlightCacheForcedMisses, "editor.highlight_cache_forced_misses")                   \
  X(EditorHighlightCacheEvictions, "editor.highlight_cache_evictions")                          \
             \
             \
             \
            \
            \
             \
  X(EditorHighlightCacheNodesRecycled, "editor.highlight_cache_nodes_recycled")                  \
  X(RenderClipInvocations, "render.clip_invocations")                                           \
              \
              \
              \
              \
              \
              \
              \
              \
             \
  X(RenderSceneFallbackFrames, "render.scene_fallback_frames")                                  \
  X(RenderFramesRetained, "render.frames_retained")                                             \
              \
              \
              \
              \
              \
              \
              \
              \
  X(RenderPartialClips, "render.partial_clips")                                                 \
  X(RenderPartialClipPixels, "render.partial_clip_pixels")                                      \
  X(RenderPromotedFullFrames, "render.promoted_full_frames")                                    \
  X(WorkspaceScheduledWakes, "workspace.scheduled_wakes")                                       \
               \
               \
               \
               \
               \
               \
  X(WorkspaceRedrawRectsQueued, "workspace.redraw_rects_queued")                                \
  X(WorkspaceRedrawRectSpills, "workspace.redraw_rect_spills")                                  \
              \
              \
              \
              \
              \
              \
              \
              \
            \
  X(WorkspaceRedrawRectPixels, "workspace.redraw_rect_pixels")                                  \
  X(WorkspaceFullRedrawRequests, "workspace.full_redraw_requests")                              \
               \
               \
               \
               \
  X(WorkspaceDerivedDamageUnits, "workspace.derived_damage_units")                              \
  X(WorkspaceDerivedDamageChanged, "workspace.derived_damage_changed")                          \
               \
               \
               \
               \
               \
               \
               \
               \
  X(WorkspaceMenuBarLayouts, "workspace.menu_bar_layouts")                                      \
  X(WorkspaceMenuBarLabelMeasures, "workspace.menu_bar_label_measures")                         \
              \
              \
              \
              \
              \
              \
              \
              \
  X(WorkspaceLayoutComputes, "workspace.layout_computes")                                       \
  X(WorkspaceEditorGroupRectBuilds, "workspace.editor_group_rect_builds")                       \
  X(WorkspaceEditorGroupRectTreeWalks, "workspace.editor_group_rect_tree_walks")                \
              \
              \
              \
  X(WorkspaceLayoutMemoHits, "workspace.layout_memo_hits")                                      \
  X(WorkspaceEditorGroupRectMemoHits, "workspace.editor_group_rect_memo_hits")                  \
  X(WorkspaceWakeReasonPluginReload, "workspace.wake_reason_plugin_reload")                     \
  X(WorkspaceWakeReasonCaretBlink, "workspace.wake_reason_caret_blink")                         \
  X(WorkspaceWakeReasonNone, "workspace.wake_reason_none")                                      \
  X(TerminalScrollbackLinesAllocated, "terminal.scrollback_lines_allocated")                    \
              \
              \
              \
  X(TerminalLineBufferPoolHits, "terminal.line_buffer_pool_hits")                               \
  X(TerminalLineBufferPoolMisses, "terminal.line_buffer_pool_misses")                           \
  X(TerminalEscapeSequencesAborted, "terminal.escape_sequences_aborted")                        \
  X(RenderGlyphAtlasRuns, "render.glyph_atlas_runs")                                            \
  X(RenderGlyphAtlasGlyphs, "render.glyph_atlas_glyphs")                                        \
  X(RenderGlyphAtlasFallbacks, "render.glyph_atlas_fallbacks")                                  \
             \
             \
  X(RenderFontFallbackQueries, "render.font_fallback_queries")                                  \
             \
             \
             \
             \
             \
             \
  X(RenderGridCompositeSurfaces, "render.grid_composite_surfaces")                              \
  X(RenderGridCompositeClusterRasterizations,                                                   \
    "render.grid_composite_cluster_rasterizations")                                             \
             \
             \
             \
             \
             \
  X(RenderGridCompositeClusterAtlasBlits, "render.grid_composite_cluster_atlas_blits")          \
                   \
  X(EditorConfigResolveQueries, "editorconfig.resolve_queries")                                 \
  X(EditorConfigResolveMisses, "editorconfig.resolve_misses")                                   \
  X(EditorConfigDirectoryReads, "editorconfig.directory_reads")                                 \
  X(EditorConfigInvalidations, "editorconfig.invalidations")                                    \
  X(LspWatchedFileChangesConsidered, "lsp.watched_file_changes_considered")                     \
  X(LspWatchedFileGlobTests, "lsp.watched_file_glob_tests")                                     \
  X(LspWatchedFileEventsSent, "lsp.watched_file_events_sent")                                   \
  X(LspWatchedFileNotifiedServers, "lsp.watched_file_notified_servers")                         \
                    \
  X(SubprocessSpawns, "subprocess.spawns")                                                      \
  X(SubprocessWaitMs, "subprocess.wait_ms")                                                     \
  X(SubprocessOutputBytes, "subprocess.output_bytes")                                           \
  X(SubprocessTimeouts, "subprocess.timeouts")                                                  \
                    \
  X(GitCommandsRun, "git.commands_run")                                                         \
  X(GitCommandOutputBytes, "git.command_output_bytes")                                          \
  X(GitCommandFailures, "git.command_failures")                                                 \
  X(GitStatusRefreshCalls, "git.status_refresh_calls")                                          \
  X(GitStatusEntriesParsed, "git.status_entries_parsed")                                        \
  X(GitBlameQueries, "git.blame_queries")                                                       \
  X(GitBlameCacheHits, "git.blame_cache_hits")                                                  \
               \
              \
              \
              \
  X(GitBlameValidationProbes, "git.blame_validation_probes")                                    \
  X(GitBlameValidationSkips, "git.blame_validation_skips")                                      \
               \
               \
               \
  X(GitBlameSpansLoaded, "git.blame_spans_loaded")                                              \
  X(GitDiffLoads, "git.diff_loads")                                                             \
  X(GitDiffBytesRead, "git.diff_bytes_read")                                                    \
                   \
  X(CompareModelBuilds, "compare.model_builds")                                                 \
  X(CompareModelInputLines, "compare.model_input_lines")                                        \
  X(CompareModelRowsProduced, "compare.model_rows_produced")                                    \
  X(CompareStructuralHunks, "compare.structural_hunks")                                        \
  X(CompareStructuralGraphVertices, "compare.structural_graph_vertices")                        \
  X(MergeModelBuilds, "merge.model_builds")                                                     \
  X(MergeModelConflictsFound, "merge.model_conflicts_found")                                    \
                   \
  X(PersistenceRecordWrites, "persistence.record_writes")                                       \
  X(PersistenceRecordBytesWritten, "persistence.record_bytes_written")                          \
  X(PersistenceRecordReads, "persistence.record_reads")                                         \
  X(PersistenceRecordBytesRead, "persistence.record_bytes_read")                                \
                   \
  X(PluginLuaCallbackDispatches, "plugin.lua_callback_dispatches")                              \
  X(PluginLuaCallbackErrors, "plugin.lua_callback_errors")                                      \
                   \
  X(LspMessagesSent, "lsp.messages_sent")                                                       \
  X(LspBytesSent, "lsp.bytes_sent")                                                             \
  X(LspMessagesReceived, "lsp.messages_received")                                               \
  X(LspBytesReceived, "lsp.bytes_received")                                                     \
  X(DapMessagesSent, "dap.messages_sent")                                                       \
  X(DapBytesSent, "dap.bytes_sent")                                                             \
  X(DapMessagesReceived, "dap.messages_received")                                               \
  X(DapBytesReceived, "dap.bytes_received")                                                     \
                   \
  X(RemoteFramesSent, "remote.frames_sent")                                                     \
  X(RemoteBytesSent, "remote.bytes_sent")                                                       \
  X(RemoteFramesReceived, "remote.frames_received")                                             \
  X(RemoteBytesReceived, "remote.bytes_received")                                               \
  X(RemoteTreeFileFsyncs, "remote.tree_file_fsyncs")                                             \
  X(RemoteTreeFlushes, "remote.tree_flushes")                                                   \
                   \
  X(FileWatcherWakes, "watch.wakes")                                                            \
  X(FileWatcherPollScans, "watch.poll_scans")                                                    \
  X(FileWatcherEventsCoalesced, "watch.events_coalesced")                                       \
  X(FileIndexApplyBatchCalls, "watch.file_index_apply_batch_calls")                             \
              \
              \
              \
              \
  X(FileIndexScanSupersededByBatch, "watch.file_index_scan_superseded_by_batch")                \
  X(FileIndexRebuilds, "watch.file_index_rebuilds")                                             \
            \
            \
            \
            \
            \
            \
  X(FileIndexSubtreeRemovals, "watch.file_index_subtree_removals")                              \
  X(FileIndexSubtreeEntriesRemoved, "watch.file_index_subtree_entries_removed")                 \
            \
           \
           \
           \
           \
           \
           \
           \
  X(FileWatcherTreeShapeBatches, "watch.tree_shape_batches")                                    \
           \
           \
           \
           \
  X(FileIndexForcedRescans, "watch.file_index_forced_rescans")                                  \
            \
           \
           \
           \
           \
           \
           \
  X(FileIndexRefreshRequests, "watch.file_index_refresh_requests")                               \
           \
           \
           \
  X(FileIndexWatcherStarts, "watch.file_index_watcher_starts")                                   \
                   \
  X(TaskExecutorTasksEnqueued, "task.enqueued")                                                 \
  X(TaskExecutorTasksRun, "task.run")                                                           \
  X(MainThreadMailboxPosts, "task.main_thread_mailbox_posts")                                   \
  X(MainThreadMailboxPostsCoalesced, "task.main_thread_mailbox_posts_coalesced")                \
  X(MainThreadMailboxDrains, "task.main_thread_mailbox_drains")                                 \
                   \
               \
               \
               \
               \
  X(EditorBoxSelectionBuilds, "editor.box_selection_builds")                                   \
  X(EditorBoxSelectionCaretsPlaced, "editor.box_selection_carets_placed")                      \
               \
               \
               \
  X(EditorBoxSelectionSpanLinesScanned, "editor.box_selection_span_lines_scanned")             \
               \
               \
               \
               \
               \
               \
  X(EditorMultiRangeUndoEntries, "editor.multi_range_undo_entries")                             \
  X(EditorMultiRangeUndoLinesKept, "editor.multi_range_undo_lines_kept")                        \
  X(EditorMultiRangeUndoLinesSpanned, "editor.multi_range_undo_lines_spanned")                  \
                   \
  X(DocumentEdits, "document.edits")                                                            \
  X(DocumentAddBufferCompactions, "document.add_buffer_compactions")                            \
  X(DocumentFullTextMaterializations, "document.full_text_materializations")                    \
  X(DocumentFullTextBytes, "document.full_text_bytes")                                          \
                   \
                 \
                 \
                \
                \
  X(ExternalChangeConfirmInlineReads, "external_change.confirm_inline_reads")                   \
                  \
  X(ExternalChangeConfirmPostedReads, "external_change.confirm_posted_reads")

enum class PerfCounterId : std::size_t {
#define MICROIDE_PERF_COUNTER_ENUM(id, name) id,
  MICROIDE_PERF_COUNTERS(MICROIDE_PERF_COUNTER_ENUM)
#undef MICROIDE_PERF_COUNTER_ENUM
      Count,
};

constexpr std::size_t kPerfCounterCount = static_cast<std::size_t>(PerfCounterId::Count);

using PerfCounterSnapshot = std::array<std::uint64_t, kPerfCounterCount>;

namespace detail {

inline constinit std::array<std::atomic<std::uint64_t>, kPerfCounterCount> g_perf_counters{};

constexpr std::size_t PerfCounterIndex(PerfCounterId id) {
  const auto index = static_cast<std::size_t>(id);
  return index < kPerfCounterCount ? index : kPerfCounterCount - 1;
}

}

void ResetPerformanceCounters();
inline void AddPerformanceCounter(PerfCounterId id, std::uint64_t delta = 1) {
  detail::g_perf_counters[detail::PerfCounterIndex(id)].fetch_add(delta, std::memory_order_relaxed);
}
std::uint64_t ReadPerformanceCounter(PerfCounterId id);
PerfCounterSnapshot CapturePerformanceCounters();
std::vector<std::pair<std::string_view, std::uint64_t>> NonZeroCounterDelta(
    const PerfCounterSnapshot& before,
    const PerfCounterSnapshot& after);
std::string_view PerformanceCounterName(PerfCounterId id);

void WritePerformanceCounters(std::FILE* out);

bool PerformanceCounterDumpRequested();

void DumpPerformanceCountersOnce();

}
