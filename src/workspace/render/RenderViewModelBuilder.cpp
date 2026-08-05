#include "workspace/render/RenderViewModelBuilder.h"

#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"

#include "editor/EditorInsetLayout.h"
#include "editor/FoldingModel.h"
#include "editor/PluginSurfaceStore.h"
#include "render/TextRenderer.h"
#include "util/Parse.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

#include "workspace/persistence/RecentsService.h"
#include "workspace/render/SingleLineViewMetrics.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceUiText.h"

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
  // The scan is bounded to the columns a row can actually show, so the horizontal
  // window is part of what the result depends on.
  std::size_t horizontal_scroll = 0;
  std::size_t visible_columns = 0;
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

// Project-search sidebar status/hint line. Previously assembled per frame from
// 5+ JoinHintSegments/BuildCountStatus calls while the search panel was visible
// (and progress wakes repaint it constantly). Cache it keyed on the small set of
// inputs that change the text and rebuild only on change; the render TU draws a
// prebuilt std::string_view.
struct ProjectSearchStatusKey {
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  bool error_empty = true;
  bool running = false;
  bool query_empty = true;
  bool truncated = false;
  std::size_t results_size = 0;
  std::size_t searched_files = 0;
  std::size_t total_files = 0;
  std::size_t total_matches = 0;
  bool operator==(const ProjectSearchStatusKey&) const = default;
};

thread_local struct ProjectSearchStatusCache {
  bool valid = false;
  ProjectSearchStatusKey key;
  std::string text;
} g_project_search_status_cache;

// Status only — no key cheat-sheet. The sidebar is ~270px wide, so appending
// "/ query | = replace | r rerun | R replace all | c count all" guaranteed the
// line was cut mid-word ("26 matches | / query | = rep…") and ate the part that
// carried information. No other sidebar inlines its keys either; the search
// panel's keys live in Help/About next to the git sidebar's, and every button
// already carries a hover tooltip.
std::string ComposeProjectSearchStatus(const ProjectSearchState& ps) {
  if (ps.editing) {
    // No "Editing query"/"Editing replace" prefix: the field being edited already
    // draws with the accent border and a caret, and spelling it out here pushed
    // the line past the sidebar width so the keys themselves were what got cut.
    return JoinHintSegments({"Enter apply", "Esc cancel"});
  }
  if (!ps.error.empty()) {
    return "Search failed";
  }
  if (ps.running) {
    return BuildCountStatus("Searching ", ps.results.size(), " matches") +
           BuildSearchProgressSuffix(ps.searched_files, ps.total_files);
  }
  if (ps.results.empty()) {
    return ps.query.text().empty() ? std::string("Type to search the project")
                                   : FormatEmptyState("matches");
  }
  if (ps.truncated) {
    return ps.total_matches > ps.results.size()
               ? BuildShownOfTotalStatus(ps.results.size(), ps.total_matches, "")
               : BuildCountStatus("Showing first ", ps.results.size(), " matches");
  }
  return BuildCountStatus("", ps.results.size(), " matches");
}

std::string_view CachedProjectSearchStatus(const ProjectSearchState& ps) {
  const ProjectSearchStatusKey key{
      .editing = ps.editing,
      .edit_field = ps.edit_field,
      .error_empty = ps.error.empty(),
      .running = ps.running,
      .query_empty = ps.query.text().empty(),
      .truncated = ps.truncated,
      .results_size = ps.results.size(),
      .searched_files = ps.searched_files,
      .total_files = ps.total_files,
      .total_matches = ps.total_matches,
  };
  auto& cache = g_project_search_status_cache;
  if (!cache.valid || !(cache.key == key)) {
    cache.text = ComposeProjectSearchStatus(ps);
    cache.key = key;
    cache.valid = true;
  }
  return cache.text;
}

// Placeholder drawn over an empty project-search result list. The render TU used
// to build this with `"Error: " + state.error` / FormatEmptyState("matches")
// EVERY frame — two heap allocations per repaint on a surface that repaints on
// every search-progress wake. Same keyed-cache idiom as the status line above.
struct ProjectSearchEmptyKey {
  bool error_empty = true;
  bool running = false;
  bool query_empty = true;
  std::size_t error_size = 0;
  bool operator==(const ProjectSearchEmptyKey&) const = default;
};

thread_local struct ProjectSearchEmptyCache {
  bool valid = false;
  ProjectSearchEmptyKey key;
  std::string text;
} g_project_search_empty_cache;

std::string_view CachedProjectSearchEmptyText(const ProjectSearchState& ps) {
  const ProjectSearchEmptyKey key{
      .error_empty = ps.error.empty(),
      .running = ps.running,
      .query_empty = ps.query.text().empty(),
      // The error string itself is the only variable-length input; its length is
      // enough of a discriminator to catch a swapped message without hashing it.
      .error_size = ps.error.size(),
  };
  auto& cache = g_project_search_empty_cache;
  if (!cache.valid || !(cache.key == key)) {
    if (!ps.error.empty()) {
      cache.text = "Error: ";
      cache.text += ps.error;
    } else if (ps.running) {
      cache.text = "Searching…";
    } else if (ps.query.text().empty()) {
      // Nothing: the status line directly above already says "Type to search the
      // project". Saying it twice, and in the second voice naming the subsystem
      // back at the user ("Project Search is idle"), was noise.
      cache.text.clear();
    } else {
      cache.text = FormatEmptyState("matches");
    }
    cache.key = key;
    cache.valid = true;
  }
  return cache.text;
}

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
  if (seed_start >= seed_end || seed_line >= viewport.lines().size() ||
      seed_end > viewport.lines().LineLength(seed_line)) {
    // A stale selection anchor column can outlive a content shrink, leaving
    // seed_start/seed_end past the line length; guard before substr (which throws
    // std::out_of_range when pos > size) and to avoid seeding a truncated needle.
    seed_cache.lowered_needle.clear();
    return;
  }

  seed_cache.has_seed = true;
  seed_cache.seed_line = seed_line;
  seed_cache.seed_start = seed_start;
  seed_cache.seed_end = seed_end;

  seed_cache.needle =
      viewport.lines().LineView(seed_line).substr(seed_start, seed_end - seed_start);

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
         g_occurrence_scan_cache.horizontal_scroll == viewport.horizontal_scroll() &&
         g_occurrence_scan_cache.visible_columns == viewport.visible_columns() &&
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
  scan_cache.horizontal_scroll = viewport.horizontal_scroll();
  scan_cache.visible_columns = viewport.visible_columns();
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

  auto append_occurrences = [&](std::size_t line_index, std::string_view haystack,
                                std::size_t haystack_base) {
    const auto emit = [&](std::size_t match_start, std::size_t match_end) {
      const std::size_t start_column = haystack_base + match_start;
      const std::size_t end_column = haystack_base + match_end;
      const bool primary = line_index == seed_line && start_column == seed_start &&
                           end_column == seed_end;
      scan_cache.ranges.push_back(editor::OccurrenceRange{
          .line_index = line_index,
          .start_column = start_column,
          .end_column = end_column,
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

  // An occurrence is only ever painted inside a row's visible window -- the
  // row-fill loops clip every span to it -- so scanning the whole line produces
  // matches that are discarded on arrival. On ordinary text that is a few dozen
  // wasted bytes. On a file with no line breaks in it the "line" is the whole
  // document, and the scan re-lowercases and re-searches a megabyte every time the
  // caret moves to a different word, which is every keystroke of word motion
  // (TD-2026-08-05-132 follow-up, `editor_long_line_horizontal_scroll`).
  //
  // Soft wrap lays the whole line out across several rows, so all of it is visible
  // and the full scan is the right one.
  const bool bounded_by_window = !viewport.soft_wrap() && viewport.visible_columns() != 0;
  const std::size_t window_first_visual = viewport.horizontal_scroll();
  const std::size_t window_last_visual = window_first_visual + viewport.visible_columns();
  // Pad by the needle so a match straddling either edge is still found; the edge
  // conversion is a nearest-column mapping, not an exact one.
  const std::size_t window_pad = std::max<std::size_t>(needle.size(), 1);

  for (std::size_t line_index : visible_line_indices) {
    if (line_index >= lines.size()) {
      continue;
    }
    const std::string_view line = lines.LineView(line_index);
    if (!bounded_by_window) {
      append_occurrences(line_index, line, 0);
      continue;
    }
    const std::size_t first_byte =
        viewport.TextColumnAtVisualColumn(line_index, window_first_visual);
    const std::size_t last_byte =
        viewport.TextColumnAtVisualColumn(line_index, window_last_visual);
    const std::size_t begin = first_byte > window_pad ? first_byte - window_pad : 0;
    const std::size_t end = std::min(line.size(), last_byte + window_pad);
    if (begin >= end) {
      continue;
    }
    append_occurrences(line_index, line.substr(begin, end - begin), begin);
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
  if (!sticky_scroll_enabled || folding_model == nullptr || folding_model->resolved_ranges().empty()) {
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

namespace {

// Deferred reference into either OverlaySurfaceViewModel::label_storage (which may
// reallocate while the model is being composed) or a frame-stable state string.
// Resolved into string_views only after the blob stops growing.
struct OverlayLabelRef {
  static constexpr std::size_t kDirect = static_cast<std::size_t>(-1);
  std::size_t offset = kDirect;
  std::size_t length = 0;
  std::string_view direct;

  static OverlayLabelRef Direct(std::string_view view) {
    OverlayLabelRef ref;
    ref.direct = view;
    return ref;
  }
  static OverlayLabelRef Owned(std::string& blob, std::string_view text) {
    OverlayLabelRef ref;
    ref.offset = blob.size();
    ref.length = text.size();
    blob.append(text);
    return ref;
  }
  std::string_view Resolve(const std::string& blob) const {
    if (offset == kDirect) {
      return direct;
    }
    return std::string_view(blob).substr(offset, length);
  }
};

// Truncates `text` to `max_width` and records it: a fitting, frame-stable string
// stays a zero-copy direct view; anything truncated (the ephemeral scratch) or
// unstable is copied into the blob.
OverlayLabelRef AddTruncatedLabel(const render::TextRenderer& tr, std::string& blob,
                                  std::string_view text, float max_width, bool stable) {
  const std::string_view shown = tr.TruncateToWidthEphemeralView(text, max_width);
  if (stable && shown.data() == text.data() && shown.size() == text.size()) {
    return OverlayLabelRef::Direct(text);
  }
  return OverlayLabelRef::Owned(blob, shown);
}

struct OverlayRowRef {
  OverlayLabelRef primary;
  OverlayLabelRef secondary;
  float secondary_width = 0.0f;
};

}  // namespace

void RenderViewModelBuilder::BuildOverlaySurfaceInto(OverlaySurfaceViewModel& out,
                                                     const WorkspaceLayout& layout,
                                                     const SDL_FRect& overlay_rect,
                                                     const render::TextRenderer& tr) const {
  const OverlayState& overlay = context_.current_project_state.overlay;
  const TextInputSurface current_surface = context_.text_input.active_surface;

  out.visible = overlay.visible;
  out.mode = overlay.mode;
  out.current_surface = current_surface;
  // Seeded from state so the field is never stale on the paths that return before
  // the list layout computes it (hidden overlay, find widget); the list-overlay
  // path below replaces it with the layout-clamped row.
  out.scroll_row = overlay.scroll_row;
  out.buffer_search_query_text = overlay.workflow.buffer_search.query.text();
  out.caret_anchored = overlay.mode == OverlayMode::Completion;
  out.overlay_rect = overlay_rect;
  out.total_rows = 0;
  out.selected_row = 0;
  out.title = {};
  out.note = {};
  out.note_x = 0.0f;
  out.context_label = {};
  out.has_query_field = false;
  out.query_surface = TextInputSurface::None;
  out.query_row_y = 0.0f;
  out.query_display_text = {};
  out.summary_line = {};
  out.summary_y = 0.0f;
  out.hint = {};
  out.hint_x = 0.0f;
  out.error_line = {};
  out.error_at_title_row = false;
  out.empty_label = {};
  out.rows.clear();
  out.label_storage.clear();
  out.find_widget = OverlayFindWidgetViewModel{};
  if (!out.visible) {
    out.list_layout = ScrollableListLayout{};
    return;
  }

  std::string& blob = out.label_storage;
  const auto metrics_display = [&](const editor::SingleLineEditor& editor,
                                   std::string_view prefix,
                                   float available_width) -> std::string {
    return ComputeSingleLineViewMetrics(tr, editor, prefix, available_width).displayed_text;
  };
  // Focused fields show the caret-relative scrolled tail; unfocused fields (and an
  // empty focused field) fall back to "<prefix><full text>" from the start.
  const auto query_field_text = [&](TextInputSurface surface,
                                    const editor::SingleLineEditor& editor,
                                    float available_width) -> OverlayLabelRef {
    if (current_surface == surface) {
      if (std::string focused = metrics_display(editor, "> ", available_width);
          !focused.empty()) {
        return OverlayLabelRef::Owned(blob, focused);
      }
    }
    const std::size_t offset = blob.size();
    blob.append("> ");
    blob.append(editor.text());
    OverlayLabelRef ref;
    ref.offset = offset;
    ref.length = blob.size() - offset;
    return ref;
  };

  // Find widget (compact non-modal card): its own submodel, no list chrome.
  if (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace) {
    const BufferSearchState& buffer_search = overlay.workflow.buffer_search;
    OverlayFindWidgetViewModel& fw_vm = out.find_widget;
    fw_vm.replace_mode = overlay.mode == OverlayMode::BufferReplace;
    fw_vm.fw = ComputeBufferFindWidgetLayout(layout.editor_surface, fw_vm.replace_mode);
    fw_vm.search_focused = current_surface == TextInputSurface::BufferSearch ||
                           current_surface == TextInputSurface::BufferReplaceSearch;
    fw_vm.replace_focused = current_surface == TextInputSurface::BufferReplaceReplace;
    // Aa / ab / .*, the same order and the same first two glyphs as the terminal
    // find bar three pixels below it.
    fw_vm.toggles[static_cast<std::size_t>(BufferFindToggle::MatchCase)] =
        FindWidgetToggleViewModel{.label = "Aa", .active = buffer_search.match_case};
    fw_vm.toggles[static_cast<std::size_t>(BufferFindToggle::WholeWord)] =
        FindWidgetToggleViewModel{.label = "ab", .active = buffer_search.whole_word};
    fw_vm.toggles[static_cast<std::size_t>(BufferFindToggle::Regex)] =
        FindWidgetToggleViewModel{.label = ".*", .active = buffer_search.regex};
    fw_vm.has_matches = !buffer_search.matches.empty();
    fw_vm.has_query = !buffer_search.query.text().empty();

    OverlayLabelRef search_ref = OverlayLabelRef::Direct(buffer_search.query.text());
    if (fw_vm.search_focused) {
      if (std::string focused = metrics_display(buffer_search.query, "",
                                                std::max(1.0f, fw_vm.fw.search_field.w - 12.0f));
          !focused.empty()) {
        search_ref = OverlayLabelRef::Owned(blob, focused);
      }
    }
    OverlayLabelRef replace_ref = OverlayLabelRef::Direct(buffer_search.replace_text.text());
    if (fw_vm.replace_focused) {
      if (std::string focused = metrics_display(buffer_search.replace_text, "",
                                                std::max(1.0f, fw_vm.fw.replace_field.w - 12.0f));
          !focused.empty()) {
        replace_ref = OverlayLabelRef::Owned(blob, focused);
      }
    }
    OverlayLabelRef count_ref;
    if (fw_vm.has_query) {
      const std::size_t offset = blob.size();
      if (fw_vm.has_matches) {
        AppendUnsigned(blob, buffer_search.selected_index + 1);
        blob += "/";
        AppendUnsigned(blob, buffer_search.matches.size());
      } else {
        blob += "0/0";
      }
      count_ref.offset = offset;
      count_ref.length = blob.size() - offset;
    }
    fw_vm.search_display_text = search_ref.Resolve(blob);
    fw_vm.replace_display_text = replace_ref.Resolve(blob);
    fw_vm.count_text = count_ref.Resolve(blob);
    out.list_layout = ScrollableListLayout{};
    return;
  }

  // Modal list overlays: shared geometry, then per-mode chrome + visible rows.
  const std::size_t total_rows = [&]() -> std::size_t {
    switch (overlay.mode) {
      case OverlayMode::CommitPicker:
        return overlay.workflow.compare_picker.matches.size();
      case OverlayMode::LaunchConfigPicker:
        return overlay.workflow.launch_config_picker.matches.size();
      case OverlayMode::CommandPalette:
        return overlay.workflow.command_palette.matches.size();
      case OverlayMode::ProjectSearch:
        return overlay.workflow.project_search.results.size();
      case OverlayMode::Completion:
        return overlay.workflow.completion.items.size();
      case OverlayMode::CodeActions:
        return overlay.workflow.code_actions.items.size();
      case OverlayMode::FileFinder:
      default:
        return context_.current_project_state.file_finder.results().size();
    }
  }();
  const std::size_t selected_index = [&]() -> std::size_t {
    switch (overlay.mode) {
      case OverlayMode::CommitPicker:
        return overlay.workflow.compare_picker.selected_index;
      case OverlayMode::LaunchConfigPicker:
        return overlay.workflow.launch_config_picker.selected_index;
      case OverlayMode::CommandPalette:
        return overlay.workflow.command_palette.selected_index;
      case OverlayMode::ProjectSearch:
        return overlay.workflow.project_search.selected_index;
      case OverlayMode::Completion:
        return overlay.workflow.completion.selected_index;
      case OverlayMode::CodeActions:
        return overlay.workflow.code_actions.selected_index;
      case OverlayMode::FileFinder:
      default:
        return context_.current_project_state.file_finder.selected_index();
    }
  }();

  out.list_layout = ComputeScrollableListLayout(
      overlay_rect, overlay_rect.y + OverlayListStartOffset(overlay.mode), total_rows,
      overlay.scroll_row, 18.0f, 22.0f, 18.0f, 16.0f, 8.0f);
  out.scroll_row = out.list_layout.scroll_row;
  out.total_rows = total_rows;
  out.selected_row = static_cast<int>(selected_index);

  constexpr float kOverlayInset = 18.0f;
  const float title_width = overlay_rect.w - kOverlayInset * 2.0f;
  const float query_available = std::max(1.0f, overlay_rect.w - kOverlayInset * 2.0f);
  const float row_width = out.list_layout.row_width;
  const auto right_aligned_x = [&](std::string_view text) {
    return overlay_rect.x + overlay_rect.w - kOverlayInset - tr.MeasureWidth(text);
  };

  OverlayLabelRef title_ref;
  OverlayLabelRef note_ref;
  OverlayLabelRef context_ref;
  OverlayLabelRef query_ref;
  OverlayLabelRef summary_ref;
  OverlayLabelRef error_ref;
  OverlayLabelRef empty_ref;
  thread_local std::vector<OverlayRowRef> row_refs;
  row_refs.clear();
  thread_local std::string compose_scratch;

  const int visible_rows = out.list_layout.visible_rows;
  const auto for_visible = [&](auto&& emit_row) {
    for (int row = 0; row < visible_rows; ++row) {
      const std::size_t item_index =
          static_cast<std::size_t>(out.scroll_row) + static_cast<std::size_t>(row);
      if (item_index >= total_rows) {
        break;
      }
      emit_row(item_index);
    }
  };
  // Two-column picker rows: the muted right column keeps its measured width and
  // the primary is pre-truncated to the space that remains (TD-2026-07-16-33
  // still holds: only the VISIBLE window is materialized).
  const auto emit_two_column_rows = [&](auto&& row_at) {
    for_visible([&](std::size_t item_index) {
      const auto [primary, secondary] = row_at(item_index);
      OverlayRowRef row_ref;
      if (!secondary.empty()) {
        row_ref.secondary = OverlayLabelRef::Direct(secondary);
        row_ref.secondary_width = tr.MeasureWidth(secondary) + 12.0f;
      }
      const float primary_width = std::max(20.0f, row_width - 12.0f - row_ref.secondary_width);
      row_ref.primary = AddTruncatedLabel(tr, blob, primary, primary_width, /*stable=*/true);
      row_refs.push_back(row_ref);
    });
  };

  constexpr std::string_view kPickerHint = "↑↓ select · Enter choose · Esc cancel";
  // Shared chrome for every quick-open modal (file finder, project search, command
  // palette, commit/launch pickers): title, optional context subtitle, query field,
  // result summary, and the key hint. `*_stable` says a label lives in state that
  // outlives the frame; anything composed per frame is copied into the blob.
  const auto fill_picker_chrome = [&](std::string_view title, std::string_view context_label,
                                      const editor::SingleLineEditor& query,
                                      TextInputSurface surface, std::string_view summary_line,
                                      bool summary_stable, std::string_view empty_label,
                                      bool empty_label_stable) {
    title_ref = OverlayLabelRef::Direct(title);
    if (!context_label.empty()) {
      context_ref = AddTruncatedLabel(tr, blob, context_label, title_width, /*stable=*/true);
    }
    out.has_query_field = true;
    out.query_surface = surface;
    out.query_row_y = overlay_rect.y + OverlayQueryRowOffset(overlay.mode);
    query_ref = query_field_text(surface, query, query_available);
    // An empty summary stays empty. This used to substitute a literal "0 of 0",
    // which a picker showing no rows then printed directly above its own "No
    // matching …" line -- the same sentence twice, in the old count wording.
    if (!summary_line.empty()) {
      summary_ref = summary_stable ? OverlayLabelRef::Direct(summary_line)
                                   : OverlayLabelRef::Owned(blob, summary_line);
    }
    out.summary_y = overlay_rect.y + OverlaySummaryRowOffset(overlay.mode);
    out.hint = kPickerHint;
    out.hint_x = right_aligned_x(kPickerHint);
    if (total_rows == 0) {
      empty_ref = empty_label_stable ? OverlayLabelRef::Direct(empty_label)
                                     : OverlayLabelRef::Owned(blob, empty_label);
    }
  };

  switch (overlay.mode) {
    case OverlayMode::ProjectSearch: {
      const ProjectSearchState& search = overlay.workflow.project_search;
      // The candidate set came from the file index; if that index is only a prefix
      // of the tree, some files were never searched — flag it (TD-2026-07-17-008/033).
      if (search.index_incomplete) {
        constexpr std::string_view kNote = "index incomplete — results may be partial";
        note_ref = OverlayLabelRef::Direct(kNote);
        out.note_x = right_aligned_x(kNote);
      }
      compose_scratch = search.results.empty()
                            ? FormatEmptyState("results")
                        : search.truncated
                            ? BuildSelectionSummary(search.selected_index, search.results.size(),
                                                    " shown (capped)")
                            : BuildSelectionSummary(search.selected_index, search.results.size(),
                                                    " results");
      if (search.running) {
        compose_scratch += BuildSearchProgressSuffix(search.searched_files, search.total_files);
      }
      fill_picker_chrome("Project Search", std::string_view{}, search.query,
                         TextInputSurface::ProjectSearchOverlay, compose_scratch,
                         /*summary_stable=*/false, "No results",
                         /*empty_label_stable=*/true);
      for_visible([&](std::size_t item_index) {
        const auto& result = search.results[item_index];
        compose_scratch.clear();
        compose_scratch += result.relative_path_string;
        compose_scratch += ":";
        AppendUnsigned(compose_scratch, result.line + 1);
        compose_scratch += ":";
        AppendUnsigned(compose_scratch, result.column + 1);
        compose_scratch += "  ";
        compose_scratch += tr.TruncateToWidthEphemeralView(result.preview,
                                                           overlay_rect.w - 220.0f);
        OverlayRowRef row_ref;
        row_ref.primary =
            AddTruncatedLabel(tr, blob, compose_scratch, row_width - 16.0f, /*stable=*/false);
        row_refs.push_back(row_ref);
      });
      break;
    }
    case OverlayMode::CommitPicker: {
      const ComparePickerState& picker = overlay.workflow.compare_picker;
      compose_scratch = picker.loading ? std::string("Loading history…")
                                       : FormatEmptyState("matching revisions");
      fill_picker_chrome(
          picker.title.empty() ? std::string_view("Compare against") : picker.title,
          picker.context_label, picker.query, TextInputSurface::CommitPicker,
          picker.summary_line, /*summary_stable=*/true, compose_scratch,
          /*empty_label_stable=*/false);
      emit_two_column_rows([&](std::size_t i) -> std::pair<std::string_view, std::string_view> {
        return {picker.matches[i].primary_label, picker.matches[i].secondary_label};
      });
      break;
    }
    case OverlayMode::LaunchConfigPicker: {
      const LaunchConfigPickerState& picker = overlay.workflow.launch_config_picker;
      compose_scratch = FormatEmptyState("matching launch configurations");
      fill_picker_chrome(
          picker.title.empty() ? std::string_view("Select Launch Configuration") : picker.title,
          std::string_view{}, picker.query, TextInputSurface::LaunchConfigPicker,
          picker.summary_line, /*summary_stable=*/true, compose_scratch,
          /*empty_label_stable=*/false);
      emit_two_column_rows([&](std::size_t i) -> std::pair<std::string_view, std::string_view> {
        return {picker.matches[i].primary_label, picker.matches[i].secondary_label};
      });
      break;
    }
    case OverlayMode::CommandPalette: {
      const CommandPaletteState& palette = overlay.workflow.command_palette;
      compose_scratch = FormatEmptyState("matching commands");
      fill_picker_chrome(palette.title.empty() ? std::string_view("Commands") : palette.title,
                         std::string_view{}, palette.query, TextInputSurface::CommandPalette,
                         palette.summary_line, /*summary_stable=*/true, compose_scratch,
                         /*empty_label_stable=*/false);
      emit_two_column_rows([&](std::size_t i) -> std::pair<std::string_view, std::string_view> {
        const CommandPaletteItem& item = palette.items[palette.matches[i]];
        return {item.primary_label, item.secondary_label};
      });
      break;
    }
    case OverlayMode::Completion: {
      const CompletionSessionState& completion = overlay.workflow.completion;
      if (!completion.error.empty()) {
        error_ref = AddTruncatedLabel(tr, blob, completion.error, overlay_rect.w - 36.0f,
                                      /*stable=*/true);
        out.error_at_title_row = true;
      }
      // Rows truncate to the row column (which already excludes the scrollbar
      // gutter when the list overflows), matching every other single-column mode
      // — the card-relative width would let a long label run under the scrollbar.
      for_visible([&](std::size_t item_index) {
        const CompletionSessionItem& item = completion.items[item_index];
        OverlayRowRef row_ref;
        if (item.detail.empty()) {
          row_ref.primary =
              AddTruncatedLabel(tr, blob, item.label, row_width - 16.0f, /*stable=*/true);
        } else {
          compose_scratch.clear();
          compose_scratch += item.label;
          compose_scratch += "  ";
          compose_scratch += item.detail;
          row_ref.primary =
              AddTruncatedLabel(tr, blob, compose_scratch, row_width - 16.0f, /*stable=*/false);
        }
        row_refs.push_back(row_ref);
      });
      break;
    }
    case OverlayMode::CodeActions: {
      const CodeActionSessionState& actions = overlay.workflow.code_actions;
      title_ref = OverlayLabelRef::Direct("Code Actions");
      // "Loading..." / "No code actions available" render in the list area, below
      // the title, so they read as the menu's status rather than colliding with it.
      if (!actions.error.empty()) {
        error_ref =
            AddTruncatedLabel(tr, blob, actions.error, overlay_rect.w - 36.0f, /*stable=*/true);
      }
      for_visible([&](std::size_t item_index) {
        OverlayRowRef row_ref;
        row_ref.primary = AddTruncatedLabel(tr, blob, actions.items[item_index].title,
                                            row_width - 16.0f, /*stable=*/true);
        row_refs.push_back(row_ref);
      });
      break;
    }
    case OverlayMode::FileFinder:
    default: {
      const auto& finder = context_.current_project_state.file_finder;
      // When the file index is only a prefix of a very large/deep/unreadable tree,
      // say so on the title row (right-aligned) with the specific cause so the
      // ranked list is never read as authoritative (TD-2026-07-17-008/033).
      if (const std::string_view note = ScanIncompleteNote(finder.index_scan_status());
          !note.empty()) {
        note_ref = OverlayLabelRef::Direct(note);
        out.note_x = right_aligned_x(note);
      }
      const auto& results = finder.results();
      // Shared with the command palette, the git pickers and the overlay footers,
      // so the count means one thing across quick-open. Appended into the reused
      // scratch buffer rather than built, to keep the frame allocation-free.
      compose_scratch.clear();
      AppendFilteredCountSummary(compose_scratch, results.size(), finder.indexed_file_count(),
                                 "files");
      fill_picker_chrome("Find File", std::string_view{}, finder.query_state(),
                         TextInputSurface::FileFinder, compose_scratch,
                         /*summary_stable=*/false, "No matching files",
                         /*empty_label_stable=*/true);
      for_visible([&](std::size_t item_index) {
        OverlayRowRef row_ref;
        row_ref.primary = AddTruncatedLabel(tr, blob, results[item_index].path_string,
                                            row_width - 16.0f, /*stable=*/true);
        row_refs.push_back(row_ref);
      });
      break;
    }
  }

  // The blob is complete: resolve every deferred reference into stable views.
  out.title = title_ref.Resolve(blob);
  out.note = note_ref.Resolve(blob);
  out.context_label = context_ref.Resolve(blob);
  out.query_display_text = query_ref.Resolve(blob);
  out.summary_line = summary_ref.Resolve(blob);
  out.error_line = error_ref.Resolve(blob);
  out.empty_label = empty_ref.Resolve(blob);
  out.rows.reserve(row_refs.size());
  for (const OverlayRowRef& row_ref : row_refs) {
    out.rows.push_back(OverlayListRowViewModel{
        .primary = row_ref.primary.Resolve(blob),
        .secondary = row_ref.secondary.Resolve(blob),
        .secondary_width = row_ref.secondary_width,
    });
  }
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
      .launch_config_picker_query =
          &context_.current_project_state.overlay.workflow.launch_config_picker.query,
      .command_palette_query =
          &context_.current_project_state.overlay.workflow.command_palette.query,
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

  constexpr std::string_view kIncludePlaceholder = "files to include";
  constexpr std::string_view kExcludePlaceholder = "files to exclude";
  const std::string_view include_text =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Include
          ? project_search.edit_buffer.text()
          : project_search.include_globs.text();
  const std::string_view exclude_text =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Exclude
          ? project_search.edit_buffer.text()
          : project_search.exclude_globs.text();
  const std::string_view include_fallback_text =
      include_text.empty() ? kIncludePlaceholder : include_text;
  const std::string_view exclude_fallback_text =
      exclude_text.empty() ? kExcludePlaceholder : exclude_text;

  const SidebarMode mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id);
  const std::string_view project_search_status_text =
      mode == SidebarMode::Search ? CachedProjectSearchStatus(project_search) : std::string_view{};
  const std::string_view project_search_empty_text =
      mode == SidebarMode::Search && project_search.results.empty()
          ? CachedProjectSearchEmptyText(project_search)
          : std::string_view{};
  const GitSidebarViewModel* git_sidebar = nullptr;
  const std::vector<GitSidebarLine>* git_sidebar_lines = nullptr;
  // Building the git VM walks every changed/staged/untracked/outgoing entry and
  // allocates per-entry label strings. Only do it when the sidebar is actually
  // visible: a hidden-but-git-selected sidebar otherwise rebuilds (and discards)
  // the whole VM every frame. Mirrors the debug-pane VM's visibility guard.
  if (mode == SidebarMode::Git && context_.current_project_state.sidebar.visible) {
    // Point straight into the revision-exact memo so a hover/scroll repaint that
    // changed no git state reuses the prior build with zero copying. The returned
    // reference is a thread-local stable until the next mutating build on this
    // thread, which cannot happen between this prep call and the frame's render/
    // hit-test consumers (git state is event-sourced and never mutates mid-frame).
    const GitSidebarPresentation& presentation = CachedGitSidebarPresentation(
        context_.current_project_state.sidebar.git, context_.current_project_state.root,
        context_.current_project_state.branch_review);
    git_sidebar = &presentation.view_model;
    git_sidebar_lines = &presentation.lines;
  }

  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = mode,
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
      .query_fallback_text = query_fallback_text,
      .replace_fallback_text = replace_fallback_text,
      .include_fallback_text = include_fallback_text,
      .exclude_fallback_text = exclude_fallback_text,
      .project_search_scope_expanded = project_search.scope_expanded,
      .project_search_status_text = project_search_status_text,
      .project_search_empty_text = project_search_empty_text,
      .project_search_empty_is_error = !project_search.error.empty(),
      .git_sidebar = git_sidebar,
      .git_sidebar_lines = git_sidebar_lines,
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
  const ProjectWorkspaceState& state = context_.current_project_state;
  return DebugPaneSurfaceViewModel{
      .visible = pane.visible,
      .mode = pane.mode,
      .scroll_row = scroll_row,
      .header_label = spec != nullptr ? spec->label : std::string_view{},
      .focus = state.surface.focus,
      // Only the active mode's model is wired; the render TU draws whichever is
      // non-null and never touches broad project state.
      .execution = pane.mode == DebugPaneMode::CallStack ? &state.debug_execution : nullptr,
      .variables = pane.mode == DebugPaneMode::Variables ? &state.debug_variables : nullptr,
      .watch = pane.mode == DebugPaneMode::Watch ? &state.debug_watch : nullptr,
      .breakpoints =
          pane.mode == DebugPaneMode::Breakpoints ? &state.debug_breakpoints_panel : nullptr,
      .breakpoints_selected_row = pane.breakpoints_selected_row,
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

// The sticky band's opener lines, memoized on (viewport, scroll line, fold
// revision, settings). Split out of BuildEditorViewModelInto because the render
// path needs the band's *height* before it can size the editor metrics, and the
// band does not depend on those metrics.
//
// Resolving it needed a whole view-model build before: the pane built the model
// with visible_rows computed for a zero-height band, read sticky_lines.size(),
// recomputed the metrics, and built the entire model again -- fold gutter marks,
// breakpoints, whitespace runs, occurrence scan, inset gaps, all of it twice per
// frame for every pane with sticky scroll on (the default). The
// render.build_editor_view_model_calls counter read 2 per frame.
std::span<const std::size_t> RenderViewModelBuilder::StickyScrollLines(
    const editor::TextViewport& viewport,
    const editor::FoldingModel* folding_model,
    bool sticky_scroll_enabled,
    int sticky_max_depth) {
  if (!sticky_scroll_enabled || folding_model == nullptr || folding_model->resolved_ranges().empty()) {
    return {};
  }
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
    ComputeStickyScrollLinesUncached(viewport, folding_model, sticky_scroll_enabled,
                                     sticky_max_depth, g_sticky_scroll_cache.lines);
  } else {
    ++g_sticky_scroll_hits;
  }
  return std::span<const std::size_t>(g_sticky_scroll_cache.lines);
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

  if (folding_model != nullptr && !folding_model->resolved_ranges().empty()) {
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

  out.sticky_lines = StickyScrollLines(viewport, folding_model, sticky_scroll_enabled,
                                      sticky_max_depth);

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

  // Prefer the language server's answer whenever it is still valid for this caret.
  // It resolved the symbol, so a same-spelled name in an unrelated scope stays
  // unpainted, a shadowed one is painted, and writes get the strong tint. The
  // textual scan below stays the fallback: unserved languages, servers with no
  // documentHighlightProvider, and the window between a caret move and its
  // response (where showing the last good set would be worse than a word match).
  const ProjectWorkspaceState::SemanticOccurrenceHighlights& semantic =
      context_.current_project_state.semantic_occurrences;
  if (semantic.CoversCaret(viewport.path(), viewport.content_revision(), viewport.cursor_line(),
                           viewport.cursor_column())) {
    out.occurrence_ranges = std::span<const editor::OccurrenceRange>(semantic.ranges);
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
  const TabSlideState& slide = context_.interaction_state.tab_slide;
  const bool sliding = slide.kind == TabDragKind::Terminal;
  const PanelState& panel = context_.current_project_state.panel;
  // Resolve the plugin content surface (Phase E0) here so the render TU paints a
  // prebuilt pointer instead of walking owner/id through project state.
  const editor::SurfaceContent* plugin_surface = nullptr;
  if (panel.content == PanelContentKind::PluginSurface) {
    if (const auto* pres = context_.current_project_state.plugin_presentation_if_present();
        pres != nullptr) {
      plugin_surface = pres->surfaces.Find(panel.surface_owner, panel.surface_id);
    }
  }
  // Empty-state hint. Phrased like the debug pane's -- say what would put a row
  // here -- and static, so painting it costs no allocation. The bottom panel was
  // the last list surface in the shell that painted a blank body with no
  // explanation: an output channel whose tab is open before the tool has written
  // anything, and a Terminal panel with no live session, both showed a header over
  // nothing.
  std::string_view empty_label;
  if (panel.content == PanelContentKind::Terminal) {
    empty_label = "No terminal session — use + on the tab strip, or Terminal ▸ New Terminal.";
  } else if (panel.content == PanelContentKind::Output) {
    empty_label = "No output yet — this channel fills in as tools run.";
  }
  return BottomPanelSurfaceViewModel{
      .content = panel.content,
      .height = panel.height,
      .output_channel_id = panel.output.channel_id,
      .project_root = context_.current_project_state.root,
      .focus = context_.current_project_state.surface.focus,
      .tab_drag =
          BottomPanelTabDragViewModel{
              .active = drag.dragging && drag.kind == TabDragKind::Terminal,
              .source_index = drag.source_index,
              .pointer_x = drag.pointer_x,
              .grab_offset_x = drag.grab_offset_x,
              .sliding = sliding,
              .offsets = sliding ? slide.current : std::vector<float>{},
          },
      // `tabs` / `tab_overflow` stay empty here; PrepareFrameOnce fills them once
      // the frame layout (bottom-panel header rect) is known.
      .plugin_surface = plugin_surface,
      .plugin_surface_scroll_y = static_cast<float>(panel.surface_scroll_y),
      .empty_label = empty_label,
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

// Build a WelcomeRecent row for each path. Callers pass RecentsService's already
// exists-validated lists (ExistingRecentProjects/ExistingRecentFilesFor), so this no
// longer stats each path per paint — the validation is cached against the MRU
// revision in the service (TD-2026-07-17A-014).
std::vector<editor::WelcomeRecent> BuildRecentRows(
    std::span<const std::filesystem::path> paths) {
  std::vector<editor::WelcomeRecent> rows;
  rows.reserve(paths.size());
  for (const std::filesystem::path& path : paths) {
    if (path.empty()) {
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
    vm.recent_projects = BuildRecentRows(recents.ExistingRecentProjects());
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
  vm.recent_files =
      BuildRecentRows(recents.ExistingRecentFilesFor(root, editor::kWelcomeRecentFileLimit));
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
        .clickable = seg.clickable(),
        .command = seg.command,
        .command_arg = seg.command_arg,
        .tone = seg.tone,
    });
  };
  // Spec ordering (workspace-status-bar §"Segment list at first slice"):
  //   left:  project, branch, language, indent, encoding
  //   right: line/column, problems, lsp
  add_segment(StatusBarSegmentId::Project, vm.left_segments);
  add_segment(StatusBarSegmentId::Branch, vm.left_segments);
  add_segment(StatusBarSegmentId::Language, vm.left_segments);
  add_segment(StatusBarSegmentId::Indent, vm.left_segments);
  add_segment(StatusBarSegmentId::Encoding, vm.left_segments);
  add_segment(StatusBarSegmentId::LineColumn, vm.right_segments);
  add_segment(StatusBarSegmentId::Problems, vm.right_segments);
  add_segment(StatusBarSegmentId::Lsp, vm.right_segments);

  if (vm.layout_mode == LayoutMode::Compact) {
    // Compact-mode drop order (workspace-status-bar §"Compact-mode segment drop order"):
    //   encoding, language, indent display
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
    drop_segment(StatusBarSegmentId::Encoding, vm.left_segments);
    drop_segment(StatusBarSegmentId::Language, vm.left_segments);
    drop_segment(StatusBarSegmentId::Indent, vm.left_segments);
  }
  return vm;
}

namespace {

// Settings two-pane metrics. Control geometry uses fixed slots; the description
// column is word-wrapped, so BuildSettingsOverlay takes a TextRenderer to measure
// text and size each row to fit its full (wrapped) help description.
constexpr float kSettingsPad = 12.0f;
constexpr float kSettingsHeaderH = 38.0f;
constexpr float kSettingsFilterH = 26.0f;
constexpr float kSettingsCatRowH = 28.0f;
// Value rows are variable-height: a fixed head band (title + controls) plus one
// line per wrapped description line, so the full help text is always visible.
constexpr float kSettingsRowPadTop = 6.0f;     // title baseline offset from row top
constexpr float kSettingsRowPadBottom = 8.0f;  // gap below the last description line
constexpr float kSettingsScrollbarMargin = 14.0f;
// Help/About column metrics, moved here with the rest of its scroll model.
constexpr float kSettingsHelpPadX = 18.0f;
constexpr float kSettingsHelpPadRight = 16.0f;
constexpr float kSettingsHelpPadY = 10.0f;
constexpr float kSettingsHelpColumnGap = 16.0f;
constexpr float kSettingsHelpEntryGap = 6.0f;
// Footer band carrying the result count and the key hint, matching the quick-open
// modals. Both modes reserve it, so the two surfaces end at the same place.
constexpr float kSettingsFooterH = 24.0f;
constexpr float kSettingsBtnW = 24.0f;
constexpr float kSettingsBtnH = 22.0f;
constexpr float kSettingsValueSlotW = 100.0f;
constexpr float kSettingsSegW = 132.0f;
constexpr float kSettingsResetW = 22.0f;
constexpr float kSettingsScopeW = 68.0f;
constexpr float kSettingsTextEditW = 180.0f;
constexpr float kSettingsCheckBox = 18.0f;

// One-line subtitle shown under each settings section's title, keyed by the
// top-level category label. Unknown categories (e.g. plugin-contributed) get none,
// so the header still renders its title but no subtitle line.
std::string_view SettingsSectionSubtitle(std::string_view category) {
  if (category == "Editor") return "Text editing, indentation, saving, and fonts";
  if (category == "Appearance") return "Theme, interface scale, layout, and chrome";
  if (category == "Terminal") return "Integrated terminal shell and appearance";
  if (category == "Diagnostics") return "How problems are surfaced";
  if (category == "LSP") return "Language server features and capabilities";
  if (category == "Debugger") return "Debug adapter integration";
  if (category == "Plugins") return "Installed plugins and their toggles";
  if (category == "Control") return "External control channel for automation";
  if (category == "Workspace") return "Project files, sessions, and tabs";
  if (category == "Sidebar") return "Sidebar appearance";
  if (category == "General") return "Miscellaneous settings";
  return {};
}

// The subsection label of a group ("Editor → View" -> "View",
// "Editor → Essentials → Block Structure" -> "Essentials → Block Structure"), or
// empty when the group has no subsection. Used to render right-pane sub-headers.
std::string_view SettingsSubsectionLabel(std::string_view group) {
  const std::size_t arrow = group.find(" → ");
  if (arrow == std::string_view::npos) {
    return {};
  }
  return group.substr(arrow + std::string_view(" → ").size());
}
constexpr float kSettingsControlGap = 6.0f;

bool SettingBoolIsOn(std::string_view value) {
  return !(value == "false" || value == "0" || value == "off" || value.empty());
}

}  // namespace

SettingsOverlayViewModel RenderViewModelBuilder::BuildSettingsOverlay(
    const WorkspaceLayout& layout,
    const SettingsOverlayService& service,
    const render::TextRenderer& text_renderer) const {
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

  // Footer band: reserved from the bottom of the card before either mode lays its
  // content out, so the list/pane heights below account for it. Inset by the card
  // border so painting it does not erase the card's bottom edge.
  vm.footer_rect = MakeRect(vm.rect.x + 1.0f, vm.rect.y + vm.rect.h - kSettingsFooterH - 1.0f,
                            vm.rect.w - 2.0f, kSettingsFooterH);
  {
    const bool help = vm.mode == SettingsOverlayMode::HelpAbout;
    const std::size_t shown = service.VisibleRowCount();
    vm.footer_summary = BuildFilteredCountSummary(shown, service.TotalRowCount(),
                                                  help ? "shortcuts" : "settings");
    if (shown == 0) {
      vm.empty_label = help ? "No command or shortcut matches this filter."
                            : "No setting matches this filter.";
    }
  }
  // Help/About rows are read-only, so it says "scroll", not "choose"; Settings has
  // two panes and editable values, so it advertises Tab and Enter.
  vm.footer_hint = vm.mode == SettingsOverlayMode::HelpAbout
                       ? "↑↓ scroll · Esc close"
                       : "Tab pane · ↑↓ move · Enter edit · Esc close";

  if (vm.mode == SettingsOverlayMode::HelpAbout) {
    vm.title = "Help / About";
    vm.filter_placeholder = "Type to filter commands and shortcuts…";
    vm.filter_rect = MakeRect(vm.rect.x + kSettingsPad, vm.rect.y + kSettingsHeaderH + 4.0f,
                              vm.rect.w - 2.0f * kSettingsPad, kSettingsFilterH);
    vm.help_rows = std::span<const HelpAboutRow>(service.HelpRows());

    // Two read-only columns with a word-wrapped detail, scrolled a whole entry at
    // a time. The label column sizes to the widest label, bounded so a long one
    // cannot starve the detail column.
    const float content_x = vm.rect.x + kSettingsHelpPadX;
    const float content_right = vm.rect.x + vm.rect.w - kSettingsHelpPadRight;
    const float inner_width = std::max(1.0f, content_right - content_x);
    float label_column = 0.0f;
    for (const HelpAboutRow& row : vm.help_rows) {
      label_column = std::max(label_column, text_renderer.MeasureWidth(row.label));
    }
    vm.help_label_column = std::clamp(label_column, 100.0f, inner_width * 0.40f);
    vm.help_detail_x = content_x + vm.help_label_column + kSettingsHelpColumnGap;
    vm.help_detail_width = std::max(40.0f, content_right - vm.help_detail_x);
    vm.help_entry_gap = kSettingsHelpEntryGap;

    const float list_top = vm.filter_rect.y + vm.filter_rect.h + kSettingsHelpPadY;
    const float list_bottom = vm.footer_rect.y - kSettingsHelpPadY;
    const float available_height = std::max(0.0f, list_bottom - list_top);
    vm.help_list_rect = MakeRect(vm.rect.x, list_top, vm.rect.w, std::max(1.0f, available_height));

    // Entries are variable-height, so the count that fits is measured from the
    // bottom up: that is exactly the window shown at max scroll.
    const float line_height = text_renderer.LineHeight();
    const int total_rows = static_cast<int>(vm.help_rows.size());
    float accumulated = 0.0f;
    int fit = 0;
    for (int i = total_rows - 1; i >= 0; --i) {
      int wrapped_lines = 0;
      text_renderer.ForEachWrappedLine(vm.help_rows[static_cast<std::size_t>(i)].detail,
                                       vm.help_detail_width,
                                       [&](std::string_view) { ++wrapped_lines; });
      accumulated +=
          static_cast<float>(std::max(1, wrapped_lines)) * line_height + vm.help_entry_gap;
      if (accumulated > available_height) {
        break;
      }
      ++fit;
    }
    vm.visible_rows = std::max(1, fit);
    vm.max_scroll = std::max(0, total_rows - vm.visible_rows);
    vm.scroll_row = std::clamp(vm.scroll_row, 0, vm.max_scroll);
    if (vm.max_scroll > 0) {
      vm.scrollbar = MakeVerticalScrollbarGeometry(
          vm.help_list_rect, static_cast<float>(total_rows),
          static_cast<float>(vm.visible_rows), static_cast<float>(vm.scroll_row), false);
    }
    return vm;
  }

  vm.title = "Settings";
  vm.filter_placeholder = "Type to filter settings…";

  // Header carries the title; a full-width filter bar sits just below it.
  vm.filter_rect = MakeRect(vm.rect.x + kSettingsPad, vm.rect.y + kSettingsHeaderH + 4.0f,
                            vm.rect.w - 2.0f * kSettingsPad, kSettingsFilterH);
  const float content_top = vm.filter_rect.y + vm.filter_rect.h + 8.0f;
  const float content_bottom = vm.footer_rect.y - kSettingsPad;
  const float content_height = std::max(0.0f, content_bottom - content_top);
  const float left_w = std::clamp(vm.rect.w * 0.26f, 150.0f, 240.0f);
  vm.left_pane_rect = MakeRect(vm.rect.x, content_top, left_w, content_height);

  // Carve a fixed header band (section title + one-line subtitle) from the top of the
  // right pane; the value rows scroll below it. The left rail keeps its full height.
  const float section_line_height = text_renderer.LineHeight();
  const float section_header_h =
      kSettingsRowPadTop + 2.0f * section_line_height + kSettingsRowPadBottom;
  vm.section_header_rect =
      MakeRect(vm.rect.x + left_w, content_top, vm.rect.w - left_w, section_header_h);
  if (service.SelectedCategory() >= 0 &&
      service.SelectedCategory() < static_cast<int>(service.Categories().size())) {
    vm.section_title =
        service.Categories()[static_cast<std::size_t>(service.SelectedCategory())];
    vm.section_subtitle = SettingsSectionSubtitle(vm.section_title);
  }
  vm.right_pane_rect = MakeRect(vm.rect.x + left_w, content_top + section_header_h,
                                vm.rect.w - left_w,
                                std::max(0.0f, content_height - section_header_h));

  // Left pane: one clickable rect per category, whole-row scrolled. Categories are
  // fixed-height so the visible count and max scroll are exact; the offset shifts
  // every category rect up so off-top rows land above the pane (render skips them).
  const std::vector<std::string>& categories = service.Categories();
  const int category_count = static_cast<int>(categories.size());
  vm.category_visible_rows =
      std::max(1, static_cast<int>(content_height / kSettingsCatRowH));
  vm.category_max_scroll = std::max(0, category_count - vm.category_visible_rows);
  vm.category_scroll_row = std::clamp(service.CategoryScrollRow(), 0, vm.category_max_scroll);
  const float category_scroll_offset =
      static_cast<float>(vm.category_scroll_row) * kSettingsCatRowH;
  vm.categories.reserve(categories.size());
  for (std::size_t i = 0; i < categories.size(); ++i) {
    SettingsCategoryViewModel cat;
    cat.label = categories[i];
    cat.rect = MakeRect(vm.left_pane_rect.x,
                        vm.left_pane_rect.y + static_cast<float>(i) * kSettingsCatRowH -
                            category_scroll_offset,
                        vm.left_pane_rect.w, kSettingsCatRowH);
    cat.selected = static_cast<int>(i) == service.SelectedCategory();
    vm.categories.push_back(cat);
  }
  if (vm.category_max_scroll > 0) {
    vm.category_scrollbar = MakeVerticalScrollbarGeometry(
        vm.left_pane_rect, static_cast<float>(category_count),
        static_cast<float>(vm.category_visible_rows),
        static_cast<float>(vm.category_scroll_row), false);
  }

  // Right pane: rows of the selected category, variable-height, whole-row scrolled.
  std::vector<const SettingsOverlayRow*> cat_rows;
  for (int i = 0;; ++i) {
    const SettingsOverlayRow* row = service.RowAtVisibleIndex(service.SelectedCategory(), i);
    if (row == nullptr) {
      break;
    }
    cat_rows.push_back(row);
  }
  const int total = static_cast<int>(cat_rows.size());

  // A row is a fixed head band (title + right-aligned controls) plus one line per
  // wrapped description line. N>=1 keeps description-less rows at the previous
  // single-line height, so their layout is unchanged.
  const float line_height = text_renderer.LineHeight();
  const float head_band = kSettingsRowPadTop + 2.0f * line_height + kSettingsRowPadBottom;
  const auto row_height_for = [&](std::size_t desc_lines) {
    const float n = static_cast<float>(std::max<std::size_t>(1, desc_lines));
    return kSettingsRowPadTop + line_height + n * line_height + kSettingsRowPadBottom;
  };
  const float row_w = vm.right_pane_rect.w - kSettingsScrollbarMargin;
  const float content_right = vm.right_pane_rect.x + row_w - kSettingsPad;

  // Build every row at its natural (unscrolled) position, wrapping the description
  // to the row's text column so heights are exact; the scroll offset is applied
  // afterwards. Controls/title/scope center in the fixed head band (anchored to the
  // row top) so they stay aligned with the title however tall the description is.
  vm.rows.reserve(static_cast<std::size_t>(total));
  std::vector<float> heights;
  heights.reserve(static_cast<std::size_t>(total));
  float natural_y = vm.right_pane_rect.y;
  std::string_view prev_subsection;
  for (int i = 0; i < total; ++i) {
    const SettingsOverlayRow& row = *cat_rows[static_cast<std::size_t>(i)];
    SettingsRowViewModel rvm;
    rvm.id = row.id;
    rvm.label = row.label;
    rvm.description = row.description;
    rvm.scope_label = row.scope_label;
    rvm.row_in_category = i;
    rvm.selected = i == service.SelectedRow();
    rvm.resettable = row.resettable;

    // A sub-header labels the first row of each non-empty subsection ("Editor → View"
    // -> "View"). It occupies a reserved line strip above the row's head band; the
    // head-band content (title/controls/description) anchors below it at head_top.
    const std::string_view subsection = SettingsSubsectionLabel(row.group);
    const bool new_subsection = !subsection.empty() && subsection != prev_subsection;
    prev_subsection = subsection;
    const float subheader_h = new_subsection ? line_height : 0.0f;
    if (new_subsection) {
      rvm.group_subheader = subsection;
    }
    const float head_top = natural_y + subheader_h;

    rvm.row_rect = MakeRect(vm.right_pane_rect.x, head_top, row_w, head_band);
    const float cy = head_top + (head_band - kSettingsBtnH) * 0.5f;
    float leftmost = content_right;

    SettingsControlViewModel& control = rvm.control;
    control.kind = row.control_kind;
    control.display_value = row.value_display;
    switch (row.control_kind) {
      case SettingsControlKind::Checkbox: {
        const float box_y = head_top + (head_band - kSettingsCheckBox) * 0.5f;
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
      case SettingsControlKind::TextEdit: {
        control.value_rect = MakeRect(content_right - kSettingsTextEditW, cy, kSettingsTextEditW,
                                      kSettingsBtnH);
        if (service.EditingValue() && service.EditingRowId() == row.id) {
          control.editing = true;
          control.display_value = service.ValueEditor().text();
          control.edit_caret = service.ValueEditor().caret();
        }
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

    // Precompute the value string + caret the render TU draws, so painting the
    // settings overlay never constructs "(default)", truncates the value to the box,
    // or measures a caret prefix per frame (TD-2026-07-17A-007). The truncation width
    // matches the render TU's `value_rect.w - 12` for all three drawn kinds.
    if (control.kind == SettingsControlKind::Segmented ||
        control.kind == SettingsControlKind::Stepper ||
        control.kind == SettingsControlKind::TextEdit) {
      if (control.kind == SettingsControlKind::TextEdit && control.display_value.empty() &&
          !control.editing) {
        control.shown_value = "(default)";
        control.value_is_placeholder = true;
      } else {
        control.shown_value =
            text_renderer.TruncateToWidth(control.display_value, control.value_rect.w - 12.0f);
      }
      if (control.editing) {
        const std::size_t caret = std::min(control.edit_caret, control.display_value.size());
        // display_value is a string_view, so this substr allocates nothing.
        control.caret_offset_x = text_renderer.MeasureWidth(control.display_value.substr(0, caret));
      }
    }

    if (row.resettable) {
      rvm.reset_rect =
          MakeRect(leftmost - kSettingsControlGap - kSettingsResetW, cy, kSettingsResetW, kSettingsBtnH);
      leftmost = rvm.reset_rect.x;
    }
    // Per-row scope chip ("Project" / "Default") for built-in project-scoped
    // settings, placed to the left of the reset affordance. The active write
    // target is "Project" (this project) unless a user-level default exists with
    // no per-project override, in which case edits flow to the shared default.
    if (row.scope_selectable) {
      const bool target_project = row.project_override || !row.has_user_default;
      rvm.scope_is_project = target_project;
      rvm.scope_text = target_project ? "Project" : "Default";
      // Explain what the chip does and which way clicking it will flip the value.
      rvm.scope_help =
          target_project
              ? "Saved for this project only, overriding your user default. "
                "Click to make it your user-wide default instead."
              : "Following your user-wide default. "
                "Click to override it for this project only.";
      rvm.scope_rect =
          MakeRect(leftmost - kSettingsControlGap - kSettingsScopeW, cy, kSettingsScopeW, kSettingsBtnH);
    }

    // Description column: everything left of the controls (and the scope chip, if
    // present). The dim scope label ("User"/"Project") is right-aligned on the
    // first description line; reserve its slot so wrapped text never runs under it.
    const float text_x = rvm.row_rect.x + kSettingsPad;
    float text_left = leftmost;
    if (rvm.scope_rect.w > 0.0f) {
      text_left = std::min(text_left, rvm.scope_rect.x);
    }
    const float full_description_width = std::max(40.0f, text_left - 8.0f - text_x);
    float first_line_width = full_description_width;
    if (!rvm.scope_label.empty()) {
      const float scope_w = text_renderer.MeasureWidth(rvm.scope_label);
      const float scope_x = text_left - 8.0f - scope_w;
      if (scope_x > text_x + 40.0f) {
        rvm.scope_label_x = scope_x;
        first_line_width = std::max(40.0f, scope_x - 8.0f - text_x);
      }
    }
    if (!rvm.description.empty()) {
      if (first_line_width >= full_description_width - 0.5f) {
        // No scope-label reservation in effect: wrap every line at the full width.
        text_renderer.ForEachWrappedLine(
            rvm.description, full_description_width,
            [&](std::string_view wrapped) { rvm.description_lines.push_back(wrapped); });
      } else {
        // The dim scope label sits only on the first line, so wrap just the first
        // line at the reduced width to clear it, then wrap the remainder at the full
        // width — continuation lines have no label above them and need not be
        // narrowed (fewer wrapped lines -> shorter rows -> more settings on screen).
        std::string_view remainder = rvm.description;
        bool first_emitted = false;
        text_renderer.ForEachWrappedLine(
            rvm.description, first_line_width, [&](std::string_view wrapped) {
              if (first_emitted) {
                return;
              }
              first_emitted = true;
              rvm.description_lines.push_back(wrapped);
              const std::size_t consumed =
                  static_cast<std::size_t>(wrapped.data() - rvm.description.data()) +
                  wrapped.size();
              remainder = rvm.description.substr(std::min(consumed, rvm.description.size()));
            });
        while (!remainder.empty() && remainder.front() == ' ') {
          remainder.remove_prefix(1);
        }
        if (!remainder.empty()) {
          text_renderer.ForEachWrappedLine(
              remainder, full_description_width,
              [&](std::string_view wrapped) { rvm.description_lines.push_back(wrapped); });
        }
      }
    }

    const float row_h = row_height_for(rvm.description_lines.size());
    rvm.row_rect.h = row_h;
    // The row advances by its own height plus any reserved sub-header strip above it;
    // row_rect stays the clickable setting row (excludes the sub-header line).
    // advance_height is the scroll-independent per-row advance, exposed so keyboard
    // keep-visible can compute the scroll target in one pass instead of rebuilding
    // the whole overlay per candidate scroll step (TD-2026-07-17A-079).
    rvm.advance_height = row_h + subheader_h;
    heights.push_back(row_h + subheader_h);
    natural_y += row_h + subheader_h;
    vm.rows.push_back(std::move(rvm));
  }

  // Scroll model: whole-row index. max_scroll is the largest first-row index that
  // still fills the pane to the bottom (trailing-fit accumulation), and the pixel
  // offset for the chosen scroll shifts every built row up into place.
  int fit_from_bottom = 0;
  float acc = 0.0f;
  for (int i = total - 1; i >= 0; --i) {
    acc += heights[static_cast<std::size_t>(i)];
    if (acc > vm.right_pane_rect.h) {
      break;
    }
    ++fit_from_bottom;
  }
  vm.max_scroll = std::clamp(total - fit_from_bottom, 0, std::max(0, total - 1));
  const int scroll = std::clamp(vm.scroll_row, 0, vm.max_scroll);
  vm.scroll_row = scroll;

  float scroll_offset = 0.0f;
  for (int i = 0; i < scroll; ++i) {
    scroll_offset += heights[static_cast<std::size_t>(i)];
  }
  SDL_FRect editing_value_rect{};
  bool has_editing_row = false;
  for (SettingsRowViewModel& rvm : vm.rows) {
    if (scroll_offset != 0.0f) {
      rvm.row_rect.y -= scroll_offset;
      rvm.reset_rect.y -= scroll_offset;
      rvm.scope_rect.y -= scroll_offset;
      rvm.control.checkbox_rect.y -= scroll_offset;
      rvm.control.dec_rect.y -= scroll_offset;
      rvm.control.inc_rect.y -= scroll_offset;
      rvm.control.value_rect.y -= scroll_offset;
    }
    if (rvm.control.editing) {
      editing_value_rect = rvm.control.value_rect;
      has_editing_row = true;
    }
  }

  // Fully-visible row count from the current scroll drives keyboard keep-visible.
  const float rows_content_bottom = vm.right_pane_rect.y + vm.right_pane_rect.h;
  float visible_y = vm.right_pane_rect.y;
  int visible = 0;
  for (int i = scroll; i < total; ++i) {
    const float bottom = visible_y + heights[static_cast<std::size_t>(i)];
    if (bottom > rows_content_bottom + 0.5f) {
      break;
    }
    ++visible;
    visible_y = bottom;
  }
  vm.visible_rows = std::max(1, visible);
  if (vm.max_scroll > 0) {
    vm.scrollbar = MakeVerticalScrollbarGeometry(
        vm.right_pane_rect, static_cast<float>(total), static_cast<float>(vm.visible_rows),
        static_cast<float>(scroll), false);
  }

  // Font-picker dropdown: a windowed list of matching installed families plus a
  // pinned "Choose file…" entry, anchored to the editing row's value box.
  if (service.EditingFonts() && has_editing_row) {
    const std::vector<std::string_view> filtered = service.FilteredFontFamilies();
    const int family_count = static_cast<int>(filtered.size());
    const int highlight = service.PickerHighlight();
    // "Choose file…" is pinned after the families, so its index is the family count.
    // Use the already-computed `filtered` rather than PickerChooseFileIndex(), which
    // re-runs the whole FilteredFontFamilies() filter every frame the picker is open.
    const int choose_file_index = family_count;
    constexpr int kMaxVisibleFamilies = SettingsOverlayService::kPickerVisibleFamilies;
    constexpr float kPickerRowH = 22.0f;
    constexpr float kPickerPad = 3.0f;

    // The scroll offset is owned by the service (driven by wheel / scrollbar, kept
    // in view by keyboard nav); the window is just a clamped slice from it.
    const int start =
        std::clamp(service.PickerScroll(), 0, std::max(0, family_count - kMaxVisibleFamilies));
    const int end = std::min(family_count, start + kMaxVisibleFamilies);
    const int visible_family_rows = end - start;
    const int total_rows = visible_family_rows + 1;  // + "Choose file…" footer

    const float card_w = std::max(editing_value_rect.w, 220.0f);
    const float card_h = static_cast<float>(total_rows) * kPickerRowH + kPickerPad * 2.0f;
    // Prefer below the field; flip above when it would overflow the overlay bottom.
    const float below_y = editing_value_rect.y + editing_value_rect.h + 2.0f;
    const float above_y = editing_value_rect.y - 2.0f - card_h;
    const float card_y =
        (below_y + card_h <= vm.rect.y + vm.rect.h - 4.0f || above_y < vm.rect.y) ? below_y
                                                                                  : above_y;
    const float card_x = std::clamp(editing_value_rect.x, vm.rect.x + 4.0f,
                                    vm.rect.x + vm.rect.w - card_w - 4.0f);

    SettingsPickerViewModel& picker = vm.value_picker;
    picker.visible = true;
    picker.rect = MakeRect(card_x, card_y, card_w, card_h);
    picker.more_above = start > 0;
    picker.more_below = end < family_count;
    picker.max_scroll = std::max(0, family_count - kMaxVisibleFamilies);
    if (family_count > kMaxVisibleFamilies) {
      // Scrollbar spans just the family-rows region (above the pinned footer).
      const SDL_FRect families_rect = MakeRect(
          card_x, card_y + kPickerPad, card_w,
          static_cast<float>(visible_family_rows) * kPickerRowH);
      picker.scrollbar = MakeVerticalScrollbarGeometry(
          families_rect, static_cast<float>(family_count),
          static_cast<float>(kMaxVisibleFamilies), static_cast<float>(start), false);
    }
    picker.items.reserve(static_cast<std::size_t>(total_rows));
    float item_y = card_y + kPickerPad;
    for (int i = start; i < end; ++i) {
      picker.items.push_back(SettingsPickerItemViewModel{
          .text = filtered[static_cast<std::size_t>(i)],
          .rect = MakeRect(card_x, item_y, card_w, kPickerRowH),
          .highlighted = i == highlight,
          .is_choose_file = false,
          .dropdown_index = i,
      });
      item_y += kPickerRowH;
    }
    picker.items.push_back(SettingsPickerItemViewModel{
        .text = "Choose file…",
        .rect = MakeRect(card_x, item_y, card_w, kPickerRowH),
        .highlighted = highlight == choose_file_index,
        .is_choose_file = true,
        .dropdown_index = choose_file_index,
    });
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
