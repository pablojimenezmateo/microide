#include "workspace/RenderViewModelBuilder.h"

#include "editor/FoldingModel.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"

#include "workspace/StatusBarService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace microide::workspace {

namespace {

thread_local struct OccurrenceSeedDetectCache {
  std::uintptr_t viewport = 0;
  std::uint64_t layout_revision = 0;
  std::size_t caret_line = 0;
  std::size_t caret_column = 0;
  bool case_sensitive = false;
  bool has_seed = false;
  std::size_t seed_line = 0;
  std::size_t seed_start = 0;
  std::size_t seed_end = 0;
  std::string needle;
  std::string lowered_needle;
} g_occurrence_seed_cache;

thread_local struct OccurrenceViewportScanCache {
  std::uintptr_t viewport = 0;
  std::uint64_t layout_revision = 0;
  std::size_t scroll_line = 0;
  std::size_t visible_rows = 0;
  bool case_sensitive = false;
  std::string needle_key;
  std::vector<editor::OccurrenceRange> ranges;
} g_occurrence_scan_cache;

thread_local std::uint64_t g_occurrence_seed_hits = 0;
thread_local std::uint64_t g_occurrence_seed_misses = 0;
thread_local std::uint64_t g_occurrence_scan_hits = 0;
thread_local std::uint64_t g_occurrence_scan_misses = 0;

thread_local struct StickyScrollViewportCache {
  std::uintptr_t viewport = 0;
  std::size_t scroll_line = 0;
  std::uint64_t fold_revision = 0;
  bool enabled = false;
  int max_depth = 0;
  std::vector<std::size_t> lines;
} g_sticky_scroll_cache;

thread_local std::uint64_t g_sticky_scroll_hits = 0;
thread_local std::uint64_t g_sticky_scroll_misses = 0;

bool OccurrenceSeedCacheMatches(const editor::TextViewport& viewport,
                                std::uintptr_t viewport_key,
                                bool case_sensitive_flag) {
  return g_occurrence_seed_cache.viewport == viewport_key &&
         g_occurrence_seed_cache.layout_revision == viewport.layout_revision() &&
         g_occurrence_seed_cache.caret_line == viewport.cursor_line() &&
         g_occurrence_seed_cache.caret_column == viewport.cursor_column() &&
         g_occurrence_seed_cache.case_sensitive == case_sensitive_flag;
}

void RefillOccurrenceSeedCache(const editor::TextViewport& viewport,
                               std::uintptr_t viewport_key,
                               bool occurrences_case_sensitive) {
  auto& seed_cache = g_occurrence_seed_cache;
  seed_cache.viewport = viewport_key;
  seed_cache.layout_revision = viewport.layout_revision();
  seed_cache.caret_line = viewport.cursor_line();
  seed_cache.caret_column = viewport.cursor_column();
  seed_cache.case_sensitive = occurrences_case_sensitive;
  seed_cache.has_seed = false;

  const auto seed = viewport.OccurrenceSeedSpanForHighlight();
  if (!seed.has_value() || seed->start.line != seed->end.line) {
    seed_cache.lowered_needle.clear();
    return;
  }

  std::size_t seed_line = seed->start.line;
  std::size_t seed_start = seed->start.column;
  std::size_t seed_end = seed->end.column;
  if (seed_start >= seed_end || seed_line >= viewport.lines().size()) {
    seed_cache.lowered_needle.clear();
    return;
  }

  seed_cache.has_seed = true;
  seed_cache.seed_line = seed_line;
  seed_cache.seed_start = seed_start;
  seed_cache.seed_end = seed_end;

  seed_cache.needle = viewport.lines()[seed_line].substr(seed_start, seed_end - seed_start);

  seed_cache.lowered_needle.clear();
  if (!occurrences_case_sensitive) {
    const std::string_view needle_view(seed_cache.needle.data(), seed_cache.needle.size());
    seed_cache.lowered_needle.resize(needle_view.size());
    std::transform(needle_view.begin(), needle_view.end(), seed_cache.lowered_needle.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   });
  }
}

bool OccurrenceScanCacheMatches(const editor::TextViewport& viewport,
                                std::uintptr_t viewport_key,
                                std::size_t visible_rows,
                                const std::string& needle_compare) {
  if (needle_compare.empty()) {
    return false;
  }
  const std::size_t scroll_line = viewport.scroll_line();
  return g_occurrence_scan_cache.viewport == viewport_key &&
         g_occurrence_scan_cache.layout_revision == viewport.layout_revision() &&
         g_occurrence_scan_cache.scroll_line == scroll_line &&
         g_occurrence_scan_cache.visible_rows == visible_rows &&
         g_occurrence_scan_cache.case_sensitive ==
             g_occurrence_seed_cache.case_sensitive &&
         g_occurrence_scan_cache.needle_key == needle_compare;
}

void RefillOccurrenceScanCache(const editor::TextViewport& viewport,
                               std::uintptr_t viewport_key,
                               std::size_t visible_rows,
                               std::size_t seed_line,
                               std::size_t seed_start,
                               std::size_t seed_end,
                               const std::string& needle,
                               const std::string& lowered_needle,
                               bool occurrences_case_sensitive) {
  auto& scan_cache = g_occurrence_scan_cache;
  scan_cache.viewport = viewport_key;
  scan_cache.layout_revision = viewport.layout_revision();
  scan_cache.scroll_line = viewport.scroll_line();
  scan_cache.visible_rows = visible_rows;
  scan_cache.case_sensitive = occurrences_case_sensitive;
  scan_cache.needle_key = needle;

  scan_cache.ranges.clear();

  // Reused across calls; cleared (not reallocated) every refresh. Wrapped rows
  // can share a buffer line, so we sort+unique instead of carrying a hash set.
  thread_local std::vector<std::size_t> visible_line_indices_scratch;
  std::vector<std::size_t>& visible_line_indices = visible_line_indices_scratch;
  visible_line_indices.clear();
  const std::size_t scroll = viewport.scroll_line();
  const std::size_t visual_total = viewport.visual_line_count();
  for (std::size_t row = 0; row < visible_rows; ++row) {
    const std::size_t visual_row_index = scroll + row;
    if (visual_row_index >= visual_total) {
      break;
    }
    const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
    visible_line_indices.push_back(row_meta.line_index);
  }
  std::sort(visible_line_indices.begin(), visible_line_indices.end());
  visible_line_indices.erase(
      std::unique(visible_line_indices.begin(), visible_line_indices.end()),
      visible_line_indices.end());

  const auto& lines = viewport.lines();
  const std::string_view needle_view(needle.data(), needle.size());

  auto append_occurrences = [&](std::size_t line_index, const std::string& haystack) {
    const auto emit = [&](std::size_t match_start, std::size_t match_end) {
      const bool primary = line_index == seed_line && match_start == seed_start &&
                           match_end == seed_end;
      scan_cache.ranges.push_back(editor::OccurrenceRange{
          .line_index = line_index,
          .start_column = match_start,
          .end_column = match_end,
          .is_primary_seed = primary,
      });
    };
    if (occurrences_case_sensitive) {
      for (std::size_t pos = 0; pos <= haystack.size();) {
        const std::size_t found = haystack.find(needle_view, pos);
        if (found == std::string::npos) {
          break;
        }
        emit(found, found + needle_view.size());
        pos = found + 1;
      }
    } else {
      for (std::size_t i = 0; i + lowered_needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < lowered_needle.size(); ++j) {
          if (static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j]))) !=
              lowered_needle[j]) {
            match = false;
            break;
          }
        }
        if (match) {
          emit(i, i + lowered_needle.size());
        }
      }
    }
  };

  for (std::size_t line_index : visible_line_indices) {
    if (line_index >= lines.size()) {
      continue;
    }
    append_occurrences(line_index, lines[line_index]);
  }
}

SidebarMode SidebarModeFromViewId(std::string_view view_id) {
  if (view_id == "search") {
    return SidebarMode::Search;
  }
  if (view_id == "problems") {
    return SidebarMode::Problems;
  }
  if (view_id == "git") {
    return SidebarMode::Git;
  }
  if (view_id == "tests") {
    return SidebarMode::Tests;
  }
  if (view_id == "plugin") {
    return SidebarMode::Plugin;
  }
  if (view_id == "tree") {
    return SidebarMode::Tree;
  }
  return SidebarMode::None;
}

void CollectWhitespaceGlyphRuns(const editor::TextViewport& viewport,
                                std::size_t visible_rows,
                                std::vector<editor::WhitespaceGlyphRun>* out) {
  out->clear();
  if (visible_rows == 0) {
    return;
  }
  const auto& lines = viewport.lines();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t visual_total = viewport.visual_line_count();
  const std::size_t tab_size = viewport.tab_size();
  if (visual_total == 0) {
    return;
  }
  for (std::size_t row = 0; row < visible_rows; ++row) {
    const std::size_t visual_row_index = scroll_line + row;
    if (visual_row_index >= visual_total) {
      break;
    }
    const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
    const std::size_t line_index = row_meta.line_index;
    if (line_index >= lines.size()) {
      continue;
    }
    const std::string_view line_text = lines[line_index];
    const std::size_t row_start_visual = row_meta.visual_start;
    const std::size_t row_end_visual = row_meta.visual_end;
    std::size_t visual_col = 0;
    for (char c : line_text) {
      std::size_t cell_width = 1;
      if (c == '\t') {
        const std::size_t step = tab_size == 0 ? 1 : tab_size;
        cell_width = step - (visual_col % step);
      }
      const std::size_t cell_start = visual_col;
      visual_col += cell_width;
      if (cell_start >= row_end_visual) {
        break;
      }
      if (cell_start < row_start_visual) {
        continue;
      }
      if (visual_col > row_end_visual) {
        continue;
      }
      editor::WhitespaceGlyphRun run{};
      run.visual_row_index = visual_row_index;
      run.row_visual_start = row_start_visual;
      run.row_visual_end = row_end_visual;
      run.cell_visual_start = cell_start;
      run.cell_visual_extent = cell_width;
      if (c == ' ') {
        run.is_tab_rule = false;
        out->push_back(run);
      } else if (c == '\t') {
        run.is_tab_rule = true;
        out->push_back(run);
      }
    }
  }
}

}  // namespace

int ParseStickyScrollMaxDepthSetting(const std::optional<std::string>& value) {
  constexpr int kDefault = 3;
  if (!value.has_value() || value->empty()) {
    return kDefault;
  }
  const auto parsed = util::ParseInt(*value);
  if (!parsed.has_value()) {
    return kDefault;
  }
  return std::clamp(*parsed, 1, 8);
}

void ComputeStickyScrollLinesUncached(const editor::TextViewport& viewport,
                                      const editor::FoldingModel* folding_model,
                                      bool sticky_scroll_enabled,
                                      int sticky_max_depth,
                                      std::vector<std::size_t>& out_opener_lines) {
  out_opener_lines.clear();
  if (!sticky_scroll_enabled || folding_model == nullptr || folding_model->ranges().empty()) {
    return;
  }
  const int clamped_depth = std::clamp(sticky_max_depth, 1, 8);
  const std::size_t max_depth = static_cast<std::size_t>(clamped_depth);
  if (viewport.visual_line_count() == 0 || viewport.scroll_line() >= viewport.visual_line_count()) {
    return;
  }
  const std::size_t top_line = viewport.VisualRowLineIndex(viewport.scroll_line());
  // The indexed AppendFoldsContaining walk returns ranges in outer→inner order
  // via the prefix-max-closer cache instead of a linear scan over ranges().
  std::vector<editor::FoldRange> ancestors;
  ancestors.reserve(8);
  folding_model->AppendFoldsContaining(top_line, &ancestors);
  // The sticky-scroll bar paints openers strictly above the viewport's top
  // visible line (opener_line < top_line); drop the self-containing entry if
  // the top happens to land on an opener.
  std::vector<std::size_t> openers;
  openers.reserve(ancestors.size());
  for (const editor::FoldRange& range : ancestors) {
    if (range.opener_line < top_line && top_line <= range.closer_line) {
      openers.push_back(range.opener_line);
    }
  }
  if (openers.empty()) {
    return;
  }
  const std::size_t take = std::min(max_depth, openers.size());
  // openers is already sorted outermost-first; take the innermost `max_depth`.
  const auto start_it = openers.end() - static_cast<std::ptrdiff_t>(take);
  out_opener_lines.assign(start_it, openers.end());
}

RenderViewModelBuilder::RenderViewModelBuilder(const WorkspaceContext& context)
    : context_(context) {}

FrameSurfaceViewModel RenderViewModelBuilder::BuildFrameSurface(const WorkspaceLayout& layout) const {
  std::optional<FrameSurfaceViewModel::CompareSurfaceViewModel> compare_surface;
  if (context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size()) {
    const TabEntry& active_tab =
        context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index];
    if (active_tab.kind == TabEntry::Kind::Compare || active_tab.kind == TabEntry::Kind::Merge) {
      compare_surface = FrameSurfaceViewModel::CompareSurfaceViewModel{
          .kind = active_tab.kind,
      };
    }
  }

  return FrameSurfaceViewModel{
      .layout = layout,
      .sidebar_visible = context_.current_project_state.sidebar.visible,
      .bottom_panel_visible = context_.current_project_state.panel.command_mode ||
                              context_.current_project_state.panel.content !=
                                  PanelContentKind::None,
      .compare_surface = compare_surface,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

OverlaySurfaceViewModel RenderViewModelBuilder::BuildOverlaySurface() const {
  return OverlaySurfaceViewModel{
      .visible = context_.current_project_state.overlay.visible,
      .mode = context_.current_project_state.overlay.mode,
      .scroll_row = context_.current_project_state.overlay.scroll_row,
      .current_surface = context_.text_input.active_surface,
      .buffer_search_query_text =
          context_.current_project_state.overlay.workflow.buffer_search.query.text(),
      .state = &context_.current_project_state.overlay,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

TextInputSurfaceViewModel RenderViewModelBuilder::BuildTextInputSurface() const {
  return TextInputSurfaceViewModel{
      .current_surface = context_.text_input.active_surface,
      .prompt_editing = context_.prompts.surface_visible,
      .command_mode = context_.current_project_state.panel.command_mode,
      .command_input = &context_.current_project_state.panel.command.input,
      .prompt_input = &context_.prompts.surface.input,
      .buffer_search_query = &context_.current_project_state.overlay.workflow.buffer_search.query,
      .buffer_search_replace =
          &context_.current_project_state.overlay.workflow.buffer_search.replace_text,
      .project_search_query = &context_.current_project_state.overlay.workflow.project_search.query,
      .project_search_edit_buffer =
          &context_.current_project_state.overlay.workflow.project_search.edit_buffer,
      .commit_picker_query = &context_.current_project_state.overlay.workflow.compare_picker.query,
      .file_finder_query = &context_.current_project_state.file_finder.query_state(),
  };
}

SidebarSurfaceViewModel RenderViewModelBuilder::BuildSidebarSurface() const {
  const auto& project_search = context_.current_project_state.overlay.workflow.project_search;
  const bool editing_query =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Query;
  const bool editing_replace =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Replace;
  const std::string_view query_text =
      editing_query ? project_search.edit_buffer.text() : project_search.query.text();
  const std::string_view replace_text =
      editing_replace ? project_search.edit_buffer.text() : project_search.replace_text.text();

  // String constants live in static storage; live state is owned by the project state which
  // outlives this view model. Either way the view is safe to hold for the duration of a frame.
  constexpr std::string_view kQueryPlaceholder = "Search in project";
  constexpr std::string_view kReplacePlaceholder = "Replace in project";
  const std::string_view query_fallback_text =
      query_text.empty() ? kQueryPlaceholder : query_text;
  const std::string_view replace_fallback_text =
      replace_text.empty() ? kReplacePlaceholder : replace_text;

  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id),
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
      .query_fallback_text = query_fallback_text,
      .replace_fallback_text = replace_fallback_text,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

void RenderViewModelBuilder::BuildEditorViewModelInto(
    editor::EditorViewModel& out,
    const editor::TextViewport& viewport,
    std::size_t visible_rows,
    const editor::FoldingModel* folding_model,
    bool occurrences_highlight_enabled,
    bool occurrences_case_sensitive,
    bool sticky_scroll_enabled,
    int sticky_max_depth,
    bool render_whitespace_enabled) const {
  util::AddPerformanceCounter(util::PerfCounterId::RenderBuildEditorViewModelCalls);
  out.fold_gutter_marks.clear();
  out.sticky_lines = {};
  out.occurrence_ranges = {};
  out.whitespace_glyph_runs.clear();

  if (folding_model != nullptr && !folding_model->ranges().empty()) {
    out.fold_gutter_marks.reserve(visible_rows);
    for (std::size_t row = 0; row < visible_rows; ++row) {
      const std::size_t visual_row_index = viewport.scroll_line() + row;
      if (visual_row_index >= viewport.visual_line_count()) {
        break;
      }
      const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
      if (viewport.soft_wrap() && row_meta.visual_start != 0) {
        continue;
      }
      if (!folding_model->FoldStartingAt(row_meta.line_index).has_value()) {
        continue;
      }
      out.fold_gutter_marks.push_back(editor::FoldGutterMark{
          .line_index = row_meta.line_index,
          .visual_row_index = visual_row_index,
          .collapsed = folding_model->IsCollapsedAtOpener(row_meta.line_index),
      });
    }
  }

  if (sticky_scroll_enabled && folding_model != nullptr && !folding_model->ranges().empty()) {
    const std::uintptr_t viewport_key = reinterpret_cast<std::uintptr_t>(&viewport);
    const std::size_t scroll_line = viewport.scroll_line();
    const std::uint64_t fold_revision = folding_model->revision();
    const bool cache_hit = g_sticky_scroll_cache.viewport == viewport_key &&
                           g_sticky_scroll_cache.scroll_line == scroll_line &&
                           g_sticky_scroll_cache.fold_revision == fold_revision &&
                           g_sticky_scroll_cache.enabled == sticky_scroll_enabled &&
                           g_sticky_scroll_cache.max_depth == sticky_max_depth;
    if (!cache_hit) {
      ++g_sticky_scroll_misses;
      g_sticky_scroll_cache.viewport = viewport_key;
      g_sticky_scroll_cache.scroll_line = scroll_line;
      g_sticky_scroll_cache.fold_revision = fold_revision;
      g_sticky_scroll_cache.enabled = sticky_scroll_enabled;
      g_sticky_scroll_cache.max_depth = sticky_max_depth;
      ComputeStickyScrollLinesUncached(viewport,
                                       folding_model,
                                       sticky_scroll_enabled,
                                       sticky_max_depth,
                                       g_sticky_scroll_cache.lines);
    } else {
      ++g_sticky_scroll_hits;
    }
    out.sticky_lines = std::span<const std::size_t>(g_sticky_scroll_cache.lines);
  }

  if (render_whitespace_enabled && !viewport.is_placeholder()) {
    CollectWhitespaceGlyphRuns(viewport, visible_rows, &out.whitespace_glyph_runs);
  }

  if (!occurrences_highlight_enabled || viewport.is_placeholder()) {
    return;
  }

  const std::uintptr_t viewport_key = reinterpret_cast<std::uintptr_t>(&viewport);

  const bool seed_cache_hit =
      OccurrenceSeedCacheMatches(viewport, viewport_key, occurrences_case_sensitive);
  if (!seed_cache_hit) {
    ++g_occurrence_seed_misses;
    RefillOccurrenceSeedCache(viewport, viewport_key, occurrences_case_sensitive);
  } else {
    ++g_occurrence_seed_hits;
  }

  if (!g_occurrence_seed_cache.has_seed) {
    return;
  }

  const auto& seed_cache = g_occurrence_seed_cache;

  bool scan_hit = OccurrenceScanCacheMatches(viewport, viewport_key, visible_rows,
                                             seed_cache.needle);
  if (!scan_hit) {
    ++g_occurrence_scan_misses;
    RefillOccurrenceScanCache(viewport,
                              viewport_key,
                              visible_rows,
                              seed_cache.seed_line,
                              seed_cache.seed_start,
                              seed_cache.seed_end,
                              seed_cache.needle,
                              seed_cache.lowered_needle,
                              occurrences_case_sensitive);
  } else {
    ++g_occurrence_scan_hits;
  }

  out.occurrence_ranges = std::span<const editor::OccurrenceRange>(g_occurrence_scan_cache.ranges);
}

editor::EditorViewModel RenderViewModelBuilder::BuildEditorViewModel(
    const editor::TextViewport& viewport,
    std::size_t visible_rows,
    const editor::FoldingModel* folding_model,
    bool occurrences_highlight_enabled,
    bool occurrences_case_sensitive,
    bool sticky_scroll_enabled,
    int sticky_max_depth,
    bool render_whitespace_enabled) const {
  editor::EditorViewModel vm;
  BuildEditorViewModelInto(vm,
                           viewport,
                           visible_rows,
                           folding_model,
                           occurrences_highlight_enabled,
                           occurrences_case_sensitive,
                           sticky_scroll_enabled,
                           sticky_max_depth,
                           render_whitespace_enabled);
  return vm;
}

BottomPanelSurfaceViewModel RenderViewModelBuilder::BuildBottomPanelSurface() const {
  return BottomPanelSurfaceViewModel{
      .command_mode = context_.current_project_state.panel.command_mode,
      .content = context_.current_project_state.panel.content,
      .height = context_.current_project_state.panel.height,
      .output_channel_id = context_.current_project_state.panel.output.channel_id,
      .project_root = context_.current_project_state.root,
      .focus = context_.current_project_state.surface.focus,
      .command_state = &context_.current_project_state.panel.command,
  };
}

HoverPopupViewModel RenderViewModelBuilder::BuildHoverPopup(bool has_active_target) const {
  return HoverPopupViewModel{
      .visible = has_active_target,
      .has_active_target = has_active_target,
  };
}

HoverTargetsViewModel RenderViewModelBuilder::BuildHoverTargets() const {
  return HoverTargetsViewModel{
      .hover_enabled = true,
      .diagnostics_store = &context_.current_project_state.diagnostics_store,
  };
}

StatusBarViewModel RenderViewModelBuilder::BuildStatusBar(const WorkspaceLayout& layout,
                                                          const StatusBarService& service) const {
  StatusBarViewModel vm;
  vm.visible = layout.status_bar.w > 0.0f && layout.status_bar.h > 0.0f;
  vm.rect = layout.status_bar;
  vm.layout_mode = layout.layout_mode;
  if (!vm.visible) {
    return vm;
  }
  const auto& snapshot = service.Snapshot();
  const auto add_segment = [&](StatusBarSegmentId id,
                                std::vector<StatusBarSegmentViewModel>& target) {
    const auto& seg = snapshot[static_cast<std::size_t>(id)];
    if (!seg.visible || seg.text.empty()) {
      return;
    }
    target.push_back(StatusBarSegmentViewModel{id, seg.text, seg.tooltip, seg.clickable});
  };
  // Spec ordering (workspace-status-bar §"Segment list at first slice"):
  //   left:  project, branch, language, indent, encoding
  //   right: line/column, problems, lsp, layout-mode
  add_segment(StatusBarSegmentId::Project, vm.left_segments);
  add_segment(StatusBarSegmentId::Branch, vm.left_segments);
  add_segment(StatusBarSegmentId::Language, vm.left_segments);
  add_segment(StatusBarSegmentId::Indent, vm.left_segments);
  add_segment(StatusBarSegmentId::Encoding, vm.left_segments);
  add_segment(StatusBarSegmentId::LineColumn, vm.right_segments);
  add_segment(StatusBarSegmentId::Problems, vm.right_segments);
  add_segment(StatusBarSegmentId::Lsp, vm.right_segments);
  add_segment(StatusBarSegmentId::LayoutMode, vm.right_segments);

  if (vm.layout_mode == LayoutMode::Compact) {
    // Compact-mode drop order (workspace-status-bar §"Compact-mode segment drop order"):
    //   layout-mode badge, encoding, language, indent display
    // Keep: project+branch+cleanliness, line/column, problems count, LSP state
    const auto drop_segment = [&](StatusBarSegmentId id,
                                    std::vector<StatusBarSegmentViewModel>& segments) {
      const auto it = std::find_if(segments.begin(), segments.end(),
                                    [&](const StatusBarSegmentViewModel& seg) {
                                      return seg.id == id;
                                    });
      if (it != segments.end()) {
        segments.erase(it);
      }
    };
    drop_segment(StatusBarSegmentId::LayoutMode, vm.right_segments);
    drop_segment(StatusBarSegmentId::Encoding, vm.left_segments);
    drop_segment(StatusBarSegmentId::Language, vm.left_segments);
    drop_segment(StatusBarSegmentId::Indent, vm.left_segments);
  }
  return vm;
}

SettingsOverlayViewModel RenderViewModelBuilder::BuildSettingsOverlay(
    const WorkspaceLayout& layout,
    const SettingsOverlayService& service) const {
  SettingsOverlayViewModel vm;
  vm.visible = service.Visible();
  vm.mode = service.Mode();
  vm.rect = ComputeOverlaySurfaceRect(layout.editor_area);
  vm.scroll_row = service.ScrollRow();
  vm.query = service.Query();
  if (!vm.visible) {
    return vm;
  }
  switch (vm.mode) {
    case SettingsOverlayMode::Settings:
      vm.title = "Settings";
      vm.settings_rows = service.SettingsRows();
      break;
    case SettingsOverlayMode::HelpAbout:
      vm.title = "Help / About";
      vm.help_rows = service.HelpRows();
      break;
  }
  return vm;
}

void RenderViewModelBuilder::ResetOccurrenceCachesForTesting() {
  g_occurrence_seed_cache = {};
  g_occurrence_scan_cache.viewport = 0;
  g_occurrence_scan_cache.layout_revision = 0;
  g_occurrence_scan_cache.scroll_line = 0;
  g_occurrence_scan_cache.visible_rows = 0;
  g_occurrence_scan_cache.case_sensitive = false;
  g_occurrence_scan_cache.needle_key.clear();
  g_occurrence_scan_cache.ranges.clear();
  g_occurrence_seed_hits = 0;
  g_occurrence_seed_misses = 0;
  g_occurrence_scan_hits = 0;
  g_occurrence_scan_misses = 0;
}

void RenderViewModelBuilder::ResetStickyScrollCacheForTesting() {
  g_sticky_scroll_cache = {};
  g_sticky_scroll_hits = 0;
  g_sticky_scroll_misses = 0;
}

std::uint64_t RenderViewModelBuilder::StickyScrollCacheHitsForTesting() {
  return g_sticky_scroll_hits;
}

std::uint64_t RenderViewModelBuilder::StickyScrollCacheMissesForTesting() {
  return g_sticky_scroll_misses;
}

std::uint64_t RenderViewModelBuilder::OccurrenceSeedCacheHitsForTesting() {
  return g_occurrence_seed_hits;
}

std::uint64_t RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() {
  return g_occurrence_seed_misses;
}

std::uint64_t RenderViewModelBuilder::OccurrenceScanCacheHitsForTesting() {
  return g_occurrence_scan_hits;
}

std::uint64_t RenderViewModelBuilder::OccurrenceScanCacheMissesForTesting() {
  return g_occurrence_scan_misses;
}

}  // namespace microide::workspace
