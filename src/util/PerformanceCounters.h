#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::util {

enum class PerfCounterId : std::size_t {
  FramePrepareCalls = 0,
  FrameRefreshEditorFoldingModelsCalls,
  FrameApplyEditorPreferencesAllTabsCalls,
  FrameRefreshStatusBarCalls,
  RenderBuildEditorViewModelCalls,
  EditorRefreshEncodingCalls,
  EditorInvalidateDerivedCachesCalls,
  EditorInvalidateDerivedCachesLines,
  EditorContentRevisionBumps,
  EditorSyntaxRevisionBumps,
  EditorLayoutShapeRevisionBumps,
  EditorPresentationRevisionBumps,
  EditorEnsureWrappedRowLayoutsRebuilds,
  EditorEnsureWrappedRowLayoutsLineVisits,
  TerminalSnapshotLineRangeIfChangedCalls,
  TerminalSnapshotLineRangeIfChangedCopiedLines,
  TerminalSnapshotLineRangeIfChangedCopiedCells,
  TerminalTrimScrollbackCalls,
  TerminalTrimScrollbackLines,
  SearchProjectProgressPublishes,
  SearchProjectLowerLineCalls,
  SearchProjectLowerLineBytes,
  SearchProjectCandidateFilesFromIndex,
  FileFinderCacheBuildCalls,
  FileFinderCacheEntriesBuilt,
  ProjectFileScannerCollectProjectFilesCalls,
  RenderTextWidthCacheQueries,
  RenderTextWidthCacheHits,
  RenderTextTextureCacheHits,
  RenderTextTextureCacheMisses,
  RenderTextTextureCacheEvictions,
  RenderViewModelBuildFrameSurfaceCalls,
  RenderViewModelBuildOverlaySurfaceCalls,
  EditorHighlightCacheForcedMisses,
  EditorHighlightCacheEvictions,
  RenderClipInvocations,
  WorkspaceScheduledWakes,
  WorkspaceWakeReasonPluginReload,
  WorkspaceWakeReasonCaretBlink,
  WorkspaceWakeReasonNone,
  TerminalScrollbackLinesAllocated,
  TerminalEscapeSequencesAborted,
  RenderGlyphAtlasRuns,
  RenderGlyphAtlasGlyphs,
  RenderGlyphAtlasFallbacks,
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

}  // namespace microide::util
