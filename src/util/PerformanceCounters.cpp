#include "util/PerformanceCounters.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace microide::util {
namespace {

using CounterArray = std::array<std::atomic<std::uint64_t>, kPerfCounterCount>;

CounterArray& Counters() {
  static CounterArray counters;
  return counters;
}

constexpr std::array<std::string_view, kPerfCounterCount> kCounterNames = {
    "frame.prepare_calls",
    "frame.refresh_editor_folding_models_calls",
    "frame.apply_editor_preferences_all_tabs_calls",
    "frame.refresh_status_bar_calls",
    "render.build_editor_view_model_calls",
    "editor.refresh_encoding_calls",
    "editor.invalidate_derived_caches_calls",
    "editor.invalidate_derived_caches_lines",
    "editor.content_revision_bumps",
    "editor.syntax_revision_bumps",
    "editor.layout_shape_revision_bumps",
    "editor.presentation_revision_bumps",
    "editor.ensure_wrapped_row_layouts_rebuilds",
    "editor.ensure_wrapped_row_layouts_line_visits",
    "terminal.snapshot_line_range_if_changed_calls",
    "terminal.snapshot_line_range_if_changed_copied_lines",
    "terminal.snapshot_line_range_if_changed_copied_cells",
    "terminal.trim_scrollback_calls",
    "terminal.trim_scrollback_lines",
    "search.project_progress_publishes",
    "search.project_snapshot_results_calls",
    "search.project_snapshot_results_count",
    "search.project_lower_line_calls",
    "search.project_lower_line_bytes",
    "search.project_candidate_files_from_index",
    "search.file_finder_cache_build_calls",
    "search.file_finder_cache_entries_built",
    "project.collect_project_files_calls",
    "render.text_width_cache_queries",
    "render.text_width_cache_hits",
    "render.text_texture_cache_hits",
    "render.text_texture_cache_misses",
    "render.text_texture_cache_evictions",
    "render.view_model_build_frame_surface_calls",
    "render.view_model_build_overlay_surface_calls",
    "editor.highlight_cache_forced_misses",
    "editor.highlight_cache_evictions",
    "render.clip_invocations",
    "workspace.scheduled_wakes",
    "workspace.wake_reason_plugin_reload",
    "workspace.wake_reason_caret_blink",
    "workspace.wake_reason_none",
    "terminal.scrollback_lines_allocated",
    "terminal.escape_sequences_aborted",
    "render.glyph_atlas_runs",
    "render.glyph_atlas_glyphs",
    "render.glyph_atlas_fallbacks",
};

std::size_t ToIndex(PerfCounterId id) {
  return std::min(static_cast<std::size_t>(id), kPerfCounterCount - 1);
}

}  // namespace

void ResetPerformanceCounters() {
  for (std::atomic<std::uint64_t>& counter : Counters()) {
    counter.store(0, std::memory_order_relaxed);
  }
}

void AddPerformanceCounter(PerfCounterId id, std::uint64_t delta) {
  Counters()[ToIndex(id)].fetch_add(delta, std::memory_order_relaxed);
}

std::uint64_t ReadPerformanceCounter(PerfCounterId id) {
  return Counters()[ToIndex(id)].load(std::memory_order_relaxed);
}

PerfCounterSnapshot CapturePerformanceCounters() {
  PerfCounterSnapshot snapshot{};
  for (std::size_t i = 0; i < kPerfCounterCount; ++i) {
    snapshot[i] = Counters()[i].load(std::memory_order_relaxed);
  }
  return snapshot;
}

std::vector<std::pair<std::string_view, std::uint64_t>> NonZeroCounterDelta(
    const PerfCounterSnapshot& before,
    const PerfCounterSnapshot& after) {
  std::vector<std::pair<std::string_view, std::uint64_t>> deltas;
  deltas.reserve(kPerfCounterCount);
  for (std::size_t i = 0; i < kPerfCounterCount; ++i) {
    const std::uint64_t prior = before[i];
    const std::uint64_t next = after[i];
    if (next <= prior) {
      continue;
    }
    const std::uint64_t delta = next - prior;
    if (delta == 0) {
      continue;
    }
    deltas.emplace_back(kCounterNames[i], delta);
  }
  return deltas;
}

std::string_view PerformanceCounterName(PerfCounterId id) {
  return kCounterNames[ToIndex(id)];
}

}  // namespace microide::util
