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
  X(EditorFoldBracketLinesScanned, "editor.fold_bracket_lines_scanned")                         \
  X(EditorFoldIndentLinesMeasured, "editor.fold_indent_lines_measured")                         \
  X(EditorFoldPartialResolves, "editor.fold_partial_resolves")                                  \
  X(EditorVisibleLineLayoutEvictions, "editor.visible_line_layout_evictions")                    \
  X(EditorVisibleLineLayoutRecycled, "editor.visible_line_layout_recycled")                      \
  X(EditorContentRevisionBumps, "editor.content_revision_bumps")                                \
  X(EditorSyntaxRevisionBumps, "editor.syntax_revision_bumps")                                  \
  X(EditorLayoutShapeRevisionBumps, "editor.layout_shape_revision_bumps")                       \
  X(EditorPresentationRevisionBumps, "editor.presentation_revision_bumps")                      \
  X(EditorEnsureWrappedRowLayoutsRebuilds, "editor.ensure_wrapped_row_layouts_rebuilds")        \
  X(EditorEnsureWrappedRowLayoutsLineVisits, "editor.ensure_wrapped_row_layouts_line_visits")   \
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
  X(FileFinderCacheBuildCalls, "search.file_finder_cache_build_calls")                          \
  X(FileFinderCacheEntriesBuilt, "search.file_finder_cache_entries_built")                      \
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
  X(RenderClipInvocations, "render.clip_invocations")                                           \
  X(WorkspaceScheduledWakes, "workspace.scheduled_wakes")                                       \
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
