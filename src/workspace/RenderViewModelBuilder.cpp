#include "workspace/RenderViewModelBuilder.h"

#include "workspace/DebugPaneRegistry.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

#include "editor/EditorInsetLayout.h"
#include "editor/FoldingModel.h"
#include "editor/PluginSurfaceStore.h"
#include "util/Parse.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

#include "workspace/RecentsService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceCommandRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace microide::workspace {

namespace {

thread_local struct OccurrenceSeedDetectCache {
  std::uintptr_t viewport = 0;
  // Occurrence seed text comes from the buffer at the caret; only content
  // mutations can change what the seed string is.
  std::uint64_t content_revision = 0;
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
  // Scan results enumerate occurrences of the needle inside buffer bytes.
  std::uint64_t content_revision = 0;
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
         g_occurrence_seed_cache.content_revision == viewport.content_revision() &&
         g_occurrence_seed_cache.caret_line == viewport.cursor_line() &&
         g_occurrence_seed_cache.caret_column == viewport.cursor_column() &&
         g_occurrence_seed_cache.case_sensitive == case_sensitive_flag;
}

void RefillOccurrenceSeedCache(const editor::TextViewport& viewport,
                               std::uintptr_t viewport_key,
                               bool occurrences_case_sensitive) {
  auto& seed_cache = g_occurrence_seed_cache;
  seed_cache.viewport = viewport_key;
  seed_cache.content_revision = viewport.content_revision();
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
    util::ToLowerAsciiInto(seed_cache.needle, seed_cache.lowered_needle);
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
         g_occurrence_scan_cache.content_revision == viewport.content_revision() &&
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
  scan_cache.content_revision = viewport.content_revision();
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
      // Lower the haystack once, then reuse the same fast find() as the
      // case-sensitive branch — the previous nested loop re-lowered every
      // haystack byte O(needle) times for each candidate position.
      thread_local std::string lowered_haystack;
      util::ToLowerAsciiInto(haystack, lowered_haystack);
      for (std::size_t pos = 0; pos <= lowered_haystack.size();) {
        const std::size_t found = lowered_haystack.find(lowered_needle, pos);
        if (found == std::string::npos) {
          break;
        }
        emit(found, found + lowered_needle.size());
        pos = found + 1;
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
  if (view_id == "outline") {
    return SidebarMode::Outline;
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
                                std::vector<editor::WhitespaceGlyphRun>* out,
                                std::vector<std::size_t>* out_row_offsets) {
  out->clear();
  out_row_offsets->clear();
  if (visible_rows == 0) {
    return;
  }
  // CSR layout: out_row_offsets has size visible_rows + 1; the last entry is the total run count.
  out_row_offsets->reserve(visible_rows + 1);
  out_row_offsets->push_back(0);
  const auto& lines = viewport.lines();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t visual_total = viewport.visual_line_count();
  const std::size_t tab_size = viewport.tab_size();
  const auto finalize_remaining_rows = [&](std::size_t starting_row) {
    for (std::size_t r = starting_row; r < visible_rows; ++r) {
      out_row_offsets->push_back(out->size());
    }
  };
  if (visual_total == 0) {
    finalize_remaining_rows(0);
    return;
  }
  for (std::size_t row = 0; row < visible_rows; ++row) {
    const std::size_t visual_row_index = scroll_line + row;
    if (visual_row_index >= visual_total) {
      finalize_remaining_rows(row);
      return;
    }
    const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
    const std::size_t line_index = row_meta.line_index;
    if (line_index >= lines.size()) {
      out_row_offsets->push_back(out->size());
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
    out_row_offsets->push_back(out->size());
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
  const EditorGroup& focused = context_.current_project_state.focused_group();
  if (focused.has_active_tab()) {
    const TabEntry& active_tab = focused.active_tab();
    if (active_tab.kind == TabEntry::Kind::Compare || active_tab.kind == TabEntry::Kind::Merge) {
      compare_surface = FrameSurfaceViewModel::CompareSurfaceViewModel{
          .kind = active_tab.kind,
      };
    }
  }

  std::optional<EditorBannerViewModel> editor_banner;
  if (const EditorBannerState* banner = ActiveEditorBannerForTab(context_.current_project_state);
      banner != nullptr) {
    const std::string name = banner->path.filename().string();
    EditorBannerViewModel view_model;
    if (banner->kind == EditorBannerState::Kind::ExternalChange) {
      view_model.has_actions = true;
      view_model.message = name + " changed on disk - reload, overwrite, or keep editing?";
    } else {
      view_model.has_actions = false;
      view_model.message = "Reloaded " + name + " from disk";
    }
    editor_banner = std::move(view_model);
  }

  return FrameSurfaceViewModel{
      .layout = layout,
      .sidebar_visible = context_.current_project_state.sidebar.visible,
      .bottom_panel_visible =
          context_.current_project_state.panel.content != PanelContentKind::None,
      .compare_surface = compare_surface,
      .editor_banner = std::move(editor_banner),
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

  const SidebarMode mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id);
  std::optional<GitSidebarViewModel> git_sidebar;
  std::vector<GitSidebarLine> git_sidebar_lines;
  // Building the git VM walks every changed/staged/untracked/outgoing entry and
  // allocates per-entry label strings. Only do it when the sidebar is actually
  // visible: a hidden-but-git-selected sidebar otherwise rebuilds (and discards)
  // the whole VM every frame. Mirrors the debug-pane VM's visibility guard.
  if (mode == SidebarMode::Git && context_.current_project_state.sidebar.visible) {
    // Pull the view model + flattened rows from the revision-exact memo so a
    // hover/scroll repaint that changed no git state skips the whole rebuild.
    // Copied out because the returned SidebarSurfaceViewModel outlives the frame
    // and the cache entry is only stable until the next CachedGitSidebarPresentation
    // call on this thread.
    const GitSidebarPresentation& presentation = CachedGitSidebarPresentation(
        context_.current_project_state.sidebar.git, context_.current_project_state.root,
        context_.current_project_state.branch_review);
    git_sidebar = presentation.view_model;
    git_sidebar_lines = presentation.lines;
  }

  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = mode,
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
      .query_fallback_text = query_fallback_text,
      .replace_fallback_text = replace_fallback_text,
      .git_sidebar = std::move(git_sidebar),
      .git_sidebar_lines = std::move(git_sidebar_lines),
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

DebugPaneSurfaceViewModel RenderViewModelBuilder::BuildDebugPaneSurface() const {
  const DebugPaneState& pane = context_.current_project_state.debug_pane;
  int scroll_row = 0;
  switch (pane.mode) {
    case DebugPaneMode::CallStack:
      scroll_row = pane.call_stack_scroll_row;
      break;
    case DebugPaneMode::Variables:
      scroll_row = pane.variables_scroll_row;
      break;
    case DebugPaneMode::Watch:
      scroll_row = pane.watch_scroll_row;
      break;
    case DebugPaneMode::Breakpoints:
      scroll_row = pane.breakpoints_scroll_row;
      break;
  }
  const DebugPaneSurfaceSpec* spec = FindDebugPaneSurface(pane.mode);
  return DebugPaneSurfaceViewModel{
      .visible = pane.visible,
      .mode = pane.mode,
      .scroll_row = scroll_row,
      .header_label = spec != nullptr ? spec->label : std::string_view{},
      .focus = context_.current_project_state.surface.focus,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

void RenderViewModelBuilder::BuildEditorInsetGaps(editor::EditorViewModel& out,
                                                  const editor::TextViewport& viewport,
                                                  std::size_t visible_rows,
                                                  editor::InsetGapFeatureFlags inset_flags,
                                                  float line_height) const {
  out.code_lens_above = inset_flags.code_lens_above;
  // No plugin/LSP contribution published: no surfaces or code lenses can exist,
  // so skip the gap build entirely (and never touch the stores) — the common
  // no-plugin case pays nothing here.
  const auto* pres = context_.current_project_state.plugin_presentation_if_present();
  if (pres == nullptr) {
    out.row_gaps = {};
    out.row_gap_contents = {};
    return;
  }
  // Ghost text (gated): the suggestion's first line draws inline at the caret
  // (tail, set below), and its remaining lines occupy a single Below gap so the
  // real rows push down. The gap is emitted by the shared producer so the render
  // and hit-test paths stay in sync; the tail is render-only.
  editor::InsetGapOptions gap_options{.inline_surfaces = inset_flags.inline_surfaces,
                                      .code_lens_above = inset_flags.code_lens_above,
                                      .code_lens_height = line_height};
  thread_local editor::GhostTextInset ghost_inset;
  // SamePathNormalized compares raw first (document paths are stored normalized,
  // so it matches in the common case), keeping this per-frame check zero-allocation
  // while a suggestion shows.
  if (inset_flags.ghost_text && pres->ghost_text.has_value() &&
      !pres->ghost_text->lines.empty() &&
      util::SamePathNormalized(viewport.path(), pres->ghost_text->path)) {
    const auto& ghost = *pres->ghost_text;
    const std::size_t vrow = viewport.VisualRowForLine(ghost.anchor_line);
    if (editor::VisualRowInWindow(vrow, viewport.scroll_line(), visible_rows,
                                  viewport.visual_line_count())) {
      out.ghost_text_tail = editor::EditorViewModel::GhostTextTail{vrow, ghost.lines.front()};
      if (ghost.lines.size() > 1) {
        ghost_inset.below_lines = std::span<const std::string>(ghost.lines).subspan(1);
        gap_options.ghost_anchor_line = ghost.anchor_line;
        gap_options.ghost_height = static_cast<float>(ghost.lines.size() - 1) * line_height;
        gap_options.ghost_content = &ghost_inset;
      }
    }
  }

  thread_local std::vector<editor::RowGap> gaps;
  thread_local std::vector<editor::RowGapContent> contents;
  editor::BuildRowGapsForWindow(pres->surfaces, pres->decorations, viewport, visible_rows,
                                gap_options, gaps, contents);
  out.row_gaps = std::span<const editor::RowGap>(gaps.data(), gaps.size());
  out.row_gap_contents = std::span<const editor::RowGapContent>(contents.data(), contents.size());
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
    bool render_whitespace_enabled,
    bool debug_enabled,
    const editor::BreakpointStore* breakpoints,
    const DebugExecutionView* debug_execution,
    editor::InsetGapFeatureFlags inset_flags,
    float line_height) const {
  util::AddPerformanceCounter(util::PerfCounterId::RenderBuildEditorViewModelCalls);
  out.fold_gutter_marks.clear();
  out.breakpoint_gutter_marks.clear();
  out.execution_line_index.reset();
  out.sticky_lines = {};
  out.occurrence_ranges = {};
  out.row_gaps = {};
  out.row_gap_contents = {};
  out.ghost_text_tail.reset();
  out.whitespace_glyph_runs.clear();
  out.whitespace_row_offsets.clear();

  // Phase E1/E2 (gated, default off): inline plugin-surface insets become inert
  // gaps below their anchor row, and above-line code lenses become inert strips
  // above their line. Built here so the render TU stays view-model-only; bounded
  // to the visible window so it is O(visible).
  out.code_lens_above = false;
  BuildEditorInsetGaps(out, viewport, visible_rows, inset_flags, line_height);

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

  // Breakpoint gutter dots, gated on the debugger being enabled. Mirrors the
  // fold-mark loop: one mark per visible opener row, deduped to the first visual
  // row of a wrapped line so a dot is not painted on every wrap fragment.
  if (debug_enabled && breakpoints != nullptr && !viewport.is_placeholder()) {
    if (const std::vector<editor::Breakpoint>* file =
            breakpoints->FindByPathKey(viewport.path_key());
        file != nullptr && !file->empty()) {
      out.breakpoint_gutter_marks.reserve(file->size());
      for (std::size_t row = 0; row < visible_rows; ++row) {
        const std::size_t visual_row_index = viewport.scroll_line() + row;
        if (visual_row_index >= viewport.visual_line_count()) {
          break;
        }
        const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
        if (viewport.soft_wrap() && row_meta.visual_start != 0) {
          continue;
        }
        const auto bp = std::lower_bound(
            file->begin(), file->end(), row_meta.line_index,
            [](const editor::Breakpoint& b, std::size_t line) { return b.line < line; });
        if (bp == file->end() || bp->line != row_meta.line_index) {
          continue;
        }
        out.breakpoint_gutter_marks.push_back(editor::BreakpointGutterMark{
            .line_index = row_meta.line_index,
            .visual_row_index = visual_row_index,
            .enabled = bp->enabled,
            .verified = bp->verified,
            .has_condition = bp->condition.has_value() || bp->hit_condition.has_value(),
            .is_logpoint = bp->log_message.has_value(),
        });
      }
    }
  }

  // Execution-line marker: set only when a session is stopped on this viewport's
  // file. Path-matched after normalization (the frame source was normalized the
  // same way when the stop was recorded); SamePathNormalized skips the per-frame
  // string allocations the old generic_string() compare paid.
  if (debug_enabled && debug_execution != nullptr && debug_execution->HasLocation() &&
      !viewport.is_placeholder()) {
    if (util::SamePathNormalized(viewport.path(), debug_execution->FocusedPath())) {
      out.execution_line_index = debug_execution->FocusedLine();
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
    CollectWhitespaceGlyphRuns(viewport, visible_rows, &out.whitespace_glyph_runs,
                                &out.whitespace_row_offsets);
  }

  if (!occurrences_highlight_enabled || viewport.is_placeholder()) {
    return;
  }

  // While a word is being actively typed/deleted the occurrence seed is a growing
  // prefix ("a" → "ap" → "app"), which both churns the scan cache every keystroke
  // and paints a fresh semi-transparent highlight layer into the retained scene
  // texture before the prior one is cleared (visible as left-to-right darkening).
  // Suppress occurrence highlighting for an edit-driven caret; a deliberate
  // navigation re-syncs the anchor and restores it. A selection-seeded highlight
  // stays (the seed is the selection, not the typed prefix).
  if (viewport.CaretIsFromActiveTextEdit() && !viewport.selection_range().has_value()) {
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
  const TabDragState& drag = context_.interaction_state.tab_drag;
  return BottomPanelSurfaceViewModel{
      .content = context_.current_project_state.panel.content,
      .height = context_.current_project_state.panel.height,
      .output_channel_id = context_.current_project_state.panel.output.channel_id,
      .project_root = context_.current_project_state.root,
      .focus = context_.current_project_state.surface.focus,
      .project_state = &context_.current_project_state,
      .tab_drag =
          BottomPanelTabDragViewModel{
              .active = drag.dragging && drag.kind == TabDragKind::Terminal,
              .source_index = drag.source_index,
              .target_slot = drag.target_slot,
              .pointer_x = drag.pointer_x,
              .grab_offset_x = drag.grab_offset_x,
          },
  };
}

HoverPopupViewModel RenderViewModelBuilder::BuildHoverPopup(bool has_active_target) const {
  return HoverPopupViewModel{
      .visible = has_active_target,
      .has_active_target = has_active_target,
  };
}

HoverTargetsViewModel RenderViewModelBuilder::BuildHoverTargets(bool debug_hover_enabled) const {
  const auto* pres = context_.current_project_state.plugin_presentation_if_present();
  return HoverTargetsViewModel{
      .hover_enabled = true,
      .diagnostics_store = &context_.current_project_state.diagnostics_store,
      .decoration_store = pres != nullptr ? &pres->decorations : nullptr,
      .debug_execution =
          debug_hover_enabled ? &context_.current_project_state.debug_execution : nullptr,
      .debug_hover = debug_hover_enabled ? &context_.current_project_state.debug_hover : nullptr,
  };
}

NotificationsViewModel RenderViewModelBuilder::BuildNotifications(
    const NotificationService& service) const {
  NotificationsViewModel vm;
  vm.entries.reserve(service.Active().size());
  for (const NotificationService::Notification& notification : service.Active()) {
    vm.entries.push_back(NotificationEntryViewModel{
        .tone = notification.tone,
        .message = notification.message,
    });
  }
  return vm;
}

namespace {

// The display leaf of a path: its folder/file name, stepping past a trailing separator so
// "/path/proj/" still yields "proj"; falls back to the full string for a bare root.
std::string PathLeafName(const std::filesystem::path& path) {
  std::filesystem::path leaf = path;
  if (!leaf.has_filename() && leaf.has_parent_path()) {
    leaf = leaf.parent_path();
  }
  std::string name = leaf.filename().string();
  return name.empty() ? path.string() : name;
}

// Build a WelcomeRecent row for each path that still exists on disk, so every row the user
// sees is actually openable (stale entries would otherwise click into a no-op). The store
// itself is left intact; a temporarily-unmounted path is just hidden.
std::vector<editor::WelcomeRecent> BuildRecentRows(
    std::span<const std::filesystem::path> paths) {
  std::vector<editor::WelcomeRecent> rows;
  rows.reserve(paths.size());
  for (const std::filesystem::path& path : paths) {
    if (path.empty()) {
      continue;
    }
    std::error_code exists_ec;
    if (!std::filesystem::exists(path, exists_ec)) {
      continue;
    }
    rows.push_back(editor::WelcomeRecent{
        .name = PathLeafName(path),
        .path_display = path.string(),
        .path = path,
    });
  }
  return rows;
}

// Append the action's accelerator to `label` as "  (chord)" when one is bound.
std::string LabelWithChord(std::string label, ActionId id) {
  if (const ActionSpec* spec = FindWorkspaceActionSpec(id);
      spec != nullptr && !spec->accelerator.empty()) {
    label += "  (" + std::string(spec->accelerator) + ")";
  }
  return label;
}

}  // namespace

editor::WelcomeViewModel RenderViewModelBuilder::BuildWelcomeView(
    const RecentsService& recents) const {
  editor::WelcomeViewModel vm;
  vm.shortcuts_heading = "Keyboard Shortcuts";

  // Curated, registry-sourced shortcut rows. Listing ActionIds (not literal chords)
  // keeps the welcome screen in lock-step with the command registry and keybindings.
  static constexpr std::array<ActionId, 9> kWelcomeShortcutActions = {
      ActionId::OpenCommandPalette, ActionId::Files,    ActionId::ProjectSearch,
      ActionId::Search,             ActionId::Save,     ActionId::SidebarToggle,
      ActionId::OpenSettings,       ActionId::CloseActiveTab,
      ActionId::AddCursorAtNextMatch,
  };
  for (ActionId id : kWelcomeShortcutActions) {
    const ActionSpec* spec = FindWorkspaceActionSpec(id);
    if (spec == nullptr || spec->accelerator.empty() || spec->label.empty()) {
      continue;
    }
    vm.shortcuts.push_back(editor::WelcomeShortcut{
        .keys = std::string(spec->accelerator),
        .label = std::string(spec->label),
    });
  }

  const std::filesystem::path& root = context_.current_project_state.root;
  if (root.empty()) {
    // Cold-start home: open a folder or reopen a recent project.
    vm.kind = editor::WelcomeKind::NoProject;
    vm.title = "Welcome to microide";
    vm.subtitle = "Open a folder to start, or pick up where you left off.";
    vm.start_heading = "Start";
    vm.recents_heading = "Recent";
    vm.empty_recents_label = "No recent projects yet.";
    vm.open_folder_label = LabelWithChord("Open Folder…", ActionId::ProjectOpen);
    vm.recent_projects = BuildRecentRows(recents.RecentProjects());
    return vm;
  }

  // Project home: a project is open but the focused group has no tab. Offer this project's
  // recent files plus the create/open/find affordances — not "open a different folder".
  vm.kind = editor::WelcomeKind::ProjectHome;
  vm.title = PathLeafName(root);
  vm.subtitle = "Open a file, or jump back into a recent one.";
  vm.actions_heading = "Actions";
  vm.recent_files_heading = "Recent files";
  vm.empty_recent_files_label = "No files opened in this project yet.";
  vm.new_file_label = LabelWithChord("New File", ActionId::Tab);
  vm.open_file_label = LabelWithChord("Open File…", ActionId::Open);
  vm.find_in_project_label = LabelWithChord("Find in Project…", ActionId::ProjectSearch);
  const std::vector<std::filesystem::path> recent_files =
      recents.RecentFilesFor(root, editor::kWelcomeRecentFileLimit);
  vm.recent_files = BuildRecentRows(recent_files);
  return vm;
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
  vm.left_segments.reserve(5);
  vm.right_segments.reserve(4);
  const auto add_segment = [&](StatusBarSegmentId id,
                                std::vector<StatusBarSegmentViewModel>& target) {
    const auto& seg = snapshot[static_cast<std::size_t>(id)];
    if (!seg.visible || seg.text.empty()) {
      return;
    }
    target.emplace_back(StatusBarSegmentViewModel{
        .id = id,
        .text = seg.text,
        .tooltip = seg.tooltip,
        .clickable = seg.clickable,
        .tone = seg.tone,
    });
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

namespace {

// Settings two-pane metrics. All control geometry uses fixed slots so the view
// model can be built without a text renderer (the builder cannot measure text).
constexpr float kSettingsPad = 12.0f;
constexpr float kSettingsHeaderH = 38.0f;
constexpr float kSettingsFilterH = 26.0f;
constexpr float kSettingsCatRowH = 28.0f;
constexpr float kSettingsValueRowH = 46.0f;
constexpr float kSettingsScrollbarMargin = 14.0f;
constexpr float kSettingsBtnW = 24.0f;
constexpr float kSettingsBtnH = 22.0f;
constexpr float kSettingsValueSlotW = 100.0f;
constexpr float kSettingsSegW = 132.0f;
constexpr float kSettingsResetW = 22.0f;
constexpr float kSettingsCheckBox = 18.0f;
constexpr float kSettingsControlGap = 6.0f;

bool SettingBoolIsOn(std::string_view value) {
  return !(value == "false" || value == "0" || value == "off" || value.empty());
}

}  // namespace

SettingsOverlayViewModel RenderViewModelBuilder::BuildSettingsOverlay(
    const WorkspaceLayout& layout,
    const SettingsOverlayService& service) const {
  SettingsOverlayViewModel vm;
  vm.visible = service.Visible();
  vm.mode = service.Mode();
  vm.rect = ComputeSettingsOverlaySurfaceRect(layout.editor_area);
  vm.header_rect = MakeRect(vm.rect.x, vm.rect.y, vm.rect.w, kSettingsHeaderH);
  vm.scroll_row = service.ScrollRow();
  vm.focused_pane = service.FocusedPane();
  vm.query = service.Query();
  vm.query_empty = service.Query().empty();
  if (!vm.visible) {
    return vm;
  }

  if (vm.mode == SettingsOverlayMode::HelpAbout) {
    vm.title = "Help / About";
    vm.help_rows = service.HelpRows();
    return vm;
  }

  vm.title = "Settings";
  vm.filter_placeholder = "Type to filter settings…";

  // Header carries the title; a full-width filter bar sits just below it.
  vm.filter_rect = MakeRect(vm.rect.x + kSettingsPad, vm.rect.y + kSettingsHeaderH + 4.0f,
                            vm.rect.w - 2.0f * kSettingsPad, kSettingsFilterH);
  const float content_top = vm.filter_rect.y + vm.filter_rect.h + 8.0f;
  const float content_bottom = vm.rect.y + vm.rect.h - kSettingsPad;
  const float content_height = std::max(0.0f, content_bottom - content_top);
  const float left_w = std::clamp(vm.rect.w * 0.26f, 150.0f, 240.0f);
  vm.left_pane_rect = MakeRect(vm.rect.x, content_top, left_w, content_height);
  vm.right_pane_rect =
      MakeRect(vm.rect.x + left_w, content_top, vm.rect.w - left_w, content_height);

  // Left pane: one clickable rect per category.
  const std::vector<std::string>& categories = service.Categories();
  vm.categories.reserve(categories.size());
  for (std::size_t i = 0; i < categories.size(); ++i) {
    SettingsCategoryViewModel cat;
    cat.label = categories[i];
    cat.rect = MakeRect(vm.left_pane_rect.x, vm.left_pane_rect.y +
                                                 static_cast<float>(i) * kSettingsCatRowH,
                        vm.left_pane_rect.w, kSettingsCatRowH);
    cat.selected = static_cast<int>(i) == service.SelectedCategory();
    vm.categories.push_back(cat);
  }

  // Right pane: rows of the selected category, fixed height, scrolled.
  std::vector<const SettingsOverlayRow*> cat_rows;
  for (int i = 0;; ++i) {
    const SettingsOverlayRow* row = service.RowAtVisibleIndex(service.SelectedCategory(), i);
    if (row == nullptr) {
      break;
    }
    cat_rows.push_back(row);
  }
  const int total = static_cast<int>(cat_rows.size());
  vm.visible_rows = std::max(1, static_cast<int>(content_height / kSettingsValueRowH));
  vm.max_scroll = std::max(0, total - vm.visible_rows);
  const int scroll = std::clamp(vm.scroll_row, 0, vm.max_scroll);
  vm.scroll_row = scroll;
  if (vm.max_scroll > 0) {
    vm.scrollbar = MakeVerticalScrollbarGeometry(
        vm.right_pane_rect, static_cast<float>(total), static_cast<float>(vm.visible_rows),
        static_cast<float>(scroll), false);
  }
  const int first = scroll;
  const int last = std::min(total, scroll + vm.visible_rows);
  vm.rows.reserve(static_cast<std::size_t>(std::max(0, last - first)));
  for (int i = first; i < last; ++i) {
    const SettingsOverlayRow& row = *cat_rows[static_cast<std::size_t>(i)];
    SettingsRowViewModel rvm;
    rvm.id = row.id;
    rvm.label = row.label;
    rvm.description = row.description;
    rvm.scope_label = row.scope_label;
    rvm.row_in_category = i;
    rvm.selected = i == service.SelectedRow();
    rvm.resettable = row.resettable;

    const float row_y =
        vm.right_pane_rect.y + static_cast<float>(i - scroll) * kSettingsValueRowH;
    rvm.row_rect = MakeRect(vm.right_pane_rect.x,
                            row_y,
                            vm.right_pane_rect.w - kSettingsScrollbarMargin,
                            kSettingsValueRowH);

    const float content_right = rvm.row_rect.x + rvm.row_rect.w - kSettingsPad;
    const float cy = rvm.row_rect.y + (kSettingsValueRowH - kSettingsBtnH) * 0.5f;
    float leftmost = content_right;

    SettingsControlViewModel& control = rvm.control;
    control.kind = row.control_kind;
    control.display_value = row.value_display;
    switch (row.control_kind) {
      case SettingsControlKind::Checkbox: {
        const float box_y = rvm.row_rect.y + (kSettingsValueRowH - kSettingsCheckBox) * 0.5f;
        control.checkbox_rect =
            MakeRect(content_right - kSettingsCheckBox, box_y, kSettingsCheckBox, kSettingsCheckBox);
        control.checkbox_on = SettingBoolIsOn(row.value);
        leftmost = control.checkbox_rect.x;
        break;
      }
      case SettingsControlKind::Segmented: {
        control.value_rect = MakeRect(content_right - kSettingsSegW, cy, kSettingsSegW, kSettingsBtnH);
        leftmost = control.value_rect.x;
        break;
      }
      case SettingsControlKind::Stepper: {
        control.inc_rect = MakeRect(content_right - kSettingsBtnW, cy, kSettingsBtnW, kSettingsBtnH);
        control.value_rect =
            MakeRect(control.inc_rect.x - kSettingsValueSlotW, cy, kSettingsValueSlotW, kSettingsBtnH);
        control.dec_rect =
            MakeRect(control.value_rect.x - kSettingsBtnW, cy, kSettingsBtnW, kSettingsBtnH);
        leftmost = control.dec_rect.x;
        break;
      }
      case SettingsControlKind::None:
        break;
    }

    if (row.resettable) {
      rvm.reset_rect =
          MakeRect(leftmost - kSettingsControlGap - kSettingsResetW, cy, kSettingsResetW, kSettingsBtnH);
    }
    vm.rows.push_back(rvm);
  }
  return vm;
}

void RenderViewModelBuilder::ResetOccurrenceCachesForTesting() {
  g_occurrence_seed_cache = {};
  g_occurrence_scan_cache.viewport = 0;
  g_occurrence_scan_cache.content_revision = 0;
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
