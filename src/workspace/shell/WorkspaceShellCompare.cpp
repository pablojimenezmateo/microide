#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>

#include "editor/SyntaxHighlighter.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareVisibleLayoutCache.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::size_t CompareMaxVisualColumns(const compare::CompareModel& model) {
  std::size_t max_columns = 0;
  for (const auto& row : model.rows) {
    max_columns = std::max(
        max_columns,
        std::max(util::Utf8CodepointCount(row.left_text),
                 util::Utf8CodepointCount(row.right_text)));
  }
  return max_columns;
}

}  // namespace

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const project::GitCommitEntry& commit,
    std::size_t selected_row) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const auto content = project::ReadGitFileAtCommit(context_.current_project_state.root, normalized_path, commit.hash);
  if (!content.has_value() || content->truncated) {
    // Absent revision -> nullopt; a truncated blob was clipped at the subprocess
    // capture ceiling, so diffing the partial bytes as truth would be bogus.
    return std::nullopt;
  }

  // Distinguish an absent working-tree file (a legitimate whole-file-deleted diff)
  // from an unreadable or binary one; the latter must not masquerade as empty text
  // or an editable compare could save a false empty file over it.
  const util::TextFileReadResult working = util::ReadTextFileClassified(normalized_path);
  if (working.is_error()) {
    return std::nullopt;
  }
  auto compare_tab = BuildCompareTabFromBuffers(
      normalized_path, content->exists ? content->content : "", working.content,
      commit.short_hash, "Working tree", selected_row, true, content->exists,
      working.status != util::TextFileReadStatus::Missing);
  if (compare_tab.has_value() && compare_tab->compare.has_value()) {
    compare_tab->compare->left_path = normalized_path;
    compare_tab->compare->right_path = normalized_path;
    compare_tab->compare->commit_hash = commit.hash;
    compare_tab->compare->right_ref = "WORKTREE";
    compare_tab->compare->right_editable = true;
    compare_tab->compare->right_view_active = true;
    RefreshCompareTabDerivedState(*compare_tab->compare);
  }
  return compare_tab;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const CompareTabState& compare_tab) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path left_source_path =
      (compare_tab.left_path.empty() ? normalized_path : compare_tab.left_path).lexically_normal();
  const std::filesystem::path right_source_path =
      (compare_tab.right_path.empty() ? normalized_path : compare_tab.right_path).lexically_normal();
  const auto left_content =
      project::ReadGitFileAtCommit(context_.current_project_state.root, left_source_path, compare_tab.commit_hash);
  if (!left_content.has_value() || left_content->truncated) {
    return std::nullopt;
  }

  std::string right_content;
  bool right_exists = true;
  if (compare_tab.right_ref == "WORKTREE") {
    if (compare_tab.right_viewport.dirty()) {
      right_content = compare_tab.right_viewport.SerializeDocumentText();
    } else {
      // Only a truly-absent worktree file maps to empty; an unreadable or binary
      // file is an error state, not a whole-file-deleted diff.
      const util::TextFileReadResult working = util::ReadTextFileClassified(right_source_path);
      if (working.is_error()) {
        return std::nullopt;
      }
      right_content = working.content;
      right_exists = working.status != util::TextFileReadStatus::Missing;
    }
  } else {
    const auto right_commit_content =
        project::ReadGitFileAtCommit(context_.current_project_state.root, right_source_path, compare_tab.right_ref);
    if (!right_commit_content.has_value() || right_commit_content->truncated) {
      return std::nullopt;
    }
    right_content =
        right_commit_content->exists ? right_commit_content->content : std::string{};
    right_exists = right_commit_content->exists;
  }

  auto rebuilt = BuildCompareTabFromBuffers(
      normalized_path, left_content->exists ? left_content->content : "",
      std::move(right_content), compare_tab.left_label, compare_tab.right_label,
      compare_tab.selected_row, compare_tab.persistable, left_content->exists, right_exists);
  if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
    return std::nullopt;
  }

  rebuilt->compare->commit_hash = compare_tab.commit_hash;
  rebuilt->compare->right_ref = compare_tab.right_ref;
  rebuilt->compare->left_path = left_source_path;
  rebuilt->compare->right_path = right_source_path;
  rebuilt->compare->scroll_row = compare_tab.scroll_row;
  rebuilt->compare->horizontal_scroll = compare_tab.horizontal_scroll;
  // Only the user's toggle carries over: the existence flags were just set from
  // what this rebuild read, and the old tab's could be stale (a file deleted or
  // recreated since).
  rebuilt->compare->build_options.ignore_whitespace = compare_tab.build_options.ignore_whitespace;
  rebuilt->compare->show_whitespace = compare_tab.show_whitespace;
  rebuilt->compare->opened_from_commit_picker = compare_tab.opened_from_commit_picker;
  rebuilt->compare->review_files = compare_tab.review_files;
  rebuilt->compare->review_file_index = compare_tab.review_file_index;
  rebuilt->compare->presentation.collapse_state = compare_tab.presentation.collapse_state;
  rebuilt->compare->right_editable = compare_tab.right_ref == "WORKTREE";
  rebuilt->compare->right_view_active =
      compare_tab.right_view_active || !rebuilt->compare->right_editable;
  rebuilt->compare->persistable = compare_tab.persistable;
  if (compare_tab.right_editable) {
    rebuilt->compare->right_viewport.MoveCursorTo(compare_tab.right_viewport.cursor_line(),
                                                  compare_tab.right_viewport.cursor_column());
    rebuilt->compare->right_viewport.SetScrollLine(compare_tab.right_viewport.scroll_line());
    rebuilt->compare->right_viewport.SetHorizontalScroll(compare_tab.right_viewport.horizontal_scroll());
    rebuilt->compare->right_viewport.SetDirty(compare_tab.right_viewport.dirty());
    rebuilt->compare->selected_row =
        rebuilt->compare->model.rows.empty()
            ? 0
            : std::min(compare_tab.selected_row, rebuilt->compare->model.rows.size() - 1);
  }
  RefreshCompareTabDerivedState(*rebuilt->compare);
  return rebuilt;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabFromBuffers(
    const std::filesystem::path& path,
    std::string left_content,
    std::string right_content,
    std::string left_label,
    std::string right_label,
    std::size_t selected_row,
    bool persistable,
    bool left_exists,
    bool right_exists) const {
  const std::filesystem::path normalized_path = path.lexically_normal();

  CompareTabState compare_tab;
  compare_tab.build_options.left_exists = left_exists;
  compare_tab.build_options.right_exists = right_exists;
  compare_tab.path = normalized_path;
  compare_tab.left_path = normalized_path;
  compare_tab.right_path = normalized_path;
  compare_tab.title = "compare: " + normalized_path.filename().string();
  compare_tab.commit_hash = left_label;
  compare_tab.left_label = std::move(left_label);
  compare_tab.right_label = std::move(right_label);
  compare_tab.left_content = compare::MakeCompareText(std::move(left_content));
  compare_tab.persistable = persistable;
  compare_tab.right_viewport.LoadContent(right_content, normalized_path);
  ApplyEditorPreferences(compare_tab.right_viewport);
  RefreshCompareTabDerivedState(compare_tab);
  compare_tab.selected_row =
      compare_tab.model.rows.empty() ? 0 : std::min(selected_row, compare_tab.model.rows.size() - 1);
  compare_tab.right_view_active = true;

  return TabEntry{
      .kind = TabEntry::Kind::Compare,
      .path = normalized_path,
      .title = compare_tab.title,
      .editor_state = std::nullopt,
      .deferred_handle = std::nullopt,
      .compare = std::move(compare_tab),
      .merge = std::nullopt,
  };
}

bool WorkspaceShell::ActiveTabIsCompare() const {
  return context_.current_project_state.focused_group().active_tab_index < context_.current_project_state.focused_group().open_tabs.size() &&
         context_.current_project_state.focused_group().open_tabs[context_.current_project_state.focused_group().active_tab_index].kind == TabEntry::Kind::Compare &&
         context_.current_project_state.focused_group().open_tabs[context_.current_project_state.focused_group().active_tab_index].compare.has_value();
}

WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &context_.current_project_state.focused_group().open_tabs[context_.current_project_state.focused_group().active_tab_index].compare.value();
}

const WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() const {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &context_.current_project_state.focused_group().open_tabs[context_.current_project_state.focused_group().active_tab_index].compare.value();
}

WorkspaceShell::CompareSurfaceLayout WorkspaceShell::ComputeCompareSurfaceLayout(
    const SDL_FRect& rect,
    const CompareTabState& compare_tab) const {
  // The gutter renders actual file line numbers (left_line/right_line), so it must
  // be sized from the largest displayed line number — i.e. the longer of the two
  // files — not the presentation-row count. In a collapsed diff of large files the
  // presentation collapses to a handful of rows while the visible line numbers stay
  // in the thousands, and sizing by row count clipped them. The left line count is
  // cached in derived state (recomputed only on a fingerprint rebuild), so this hot
  // path — render/hit-test/scroll/cursor — no longer rescans left_content for '\n'
  // per call (TD-2026-07-17A-094).
  const std::size_t gutter_max_line = std::max<std::size_t>(
      {compare_tab.derived_left_line_count, compare_tab.right_viewport.lines().size(), 1});
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    CompareSurfaceLayout layout;
    layout.line_height = text_renderer_.LineHeight();
    std::array<char, 20> line_count_buf;
    const auto [line_count_end, _] = std::to_chars(
        line_count_buf.data(), line_count_buf.data() + line_count_buf.size(), gutter_max_line);
    layout.gutter_width = std::max(
        28.0f,
        text_renderer_.MeasureWidth(std::string_view{
            line_count_buf.data(),
            static_cast<std::size_t>(line_count_end - line_count_buf.data())}) +
            12.0f);
    layout.divider_width = std::max(1.0f, std::ceil(text_renderer_.CharWidth()));
    layout.left_x = rect.x + 8.0f;
    layout.review_summary_y = rect.y + 4.0f;
    layout.action_hint_y = layout.review_summary_y + layout.line_height;
    layout.header_y = layout.review_summary_y + layout.line_height;
    layout.rows_y = layout.header_y + layout.line_height + 6.0f;

    const float reserved_width =
        reserve_vertical ? (kWorkspaceScrollbarThickness + kWorkspaceScrollbarInset) : 0.0f;
    const float reserved_height =
        reserve_horizontal ? (kWorkspaceScrollbarThickness + kWorkspaceScrollbarInset) : 0.0f;
    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const float min_pane_width = 8.0f + char_width;
    const float content_width = std::max(
        min_pane_width * 2.0f,
        rect.w - reserved_width - layout.gutter_width * 2.0f - layout.divider_width - 16.0f);
    const float min_fraction =
        std::min(0.5f, min_pane_width / std::max(content_width, 1.0f));
    const float divider_fraction =
        std::clamp(compare_tab.divider_fraction, min_fraction, 1.0f - min_fraction);
    layout.min_divider_fraction = min_fraction;
    layout.left_width = std::floor(content_width * divider_fraction);
    layout.right_width = std::max(0.0f, content_width - layout.left_width);
    layout.center_x = layout.left_x + layout.gutter_width + layout.left_width;
    layout.right_x = layout.center_x + layout.divider_width;

    const float row_region_height = rect.h - reserved_height - (layout.rows_y - rect.y) - 8.0f;
    layout.visible_rows = std::max(
        1, static_cast<int>(row_region_height / std::max(1.0f, layout.line_height)));

    const float left_pane_text_width = std::max(0.0f, layout.left_width - 8.0f);
    const float right_pane_text_width = std::max(0.0f, layout.right_width - 8.0f);
    layout.left_visible_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::floor(left_pane_text_width / char_width)));
    layout.right_visible_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::floor(right_pane_text_width / char_width)));
    layout.visible_columns = std::min(layout.left_visible_columns, layout.right_visible_columns);
    layout.show_vertical = reserve_vertical;
    layout.show_horizontal = reserve_horizontal;
    return layout;
  };

  // Soft wrap: the panes never scroll horizontally, and the wrapped row count
  // depends on the pane width — which would depend on whether a vertical scrollbar
  // is reserved, which depends on the row count. That loop does not converge, and
  // re-wrapping the whole diff up to four times per layout call would be the most
  // expensive thing on the surface. Reserve the vertical scrollbar strip
  // unconditionally instead: the wrap columns are then a pure function of the rect,
  // one measure() answers, and the strip is where the overview lane lives anyway.
  if (compare_tab.right_viewport.soft_wrap()) {
    CompareSurfaceLayout layout = measure(/*reserve_vertical=*/true, /*reserve_horizontal=*/false);
    EnsureCompareWrapLayout(compare_tab, /*soft_wrap=*/true, layout.left_visible_columns,
                            layout.right_visible_columns);
    return layout;
  }
  EnsureCompareWrapLayout(compare_tab, /*soft_wrap=*/false, 0, 0);

  bool show_vertical = false;
  bool show_horizontal = false;
  for (int iteration = 0; iteration < 4; ++iteration) {
    const CompareSurfaceLayout layout = measure(show_vertical, show_horizontal);
    const bool next_vertical =
        CompareTabPresentationRowCount(compare_tab) > static_cast<std::size_t>(layout.visible_rows);
    const bool next_horizontal = compare_tab.max_visual_columns > layout.visible_columns;
    if (next_vertical == show_vertical && next_horizontal == show_horizontal) {
      return layout;
    }
    show_vertical = next_vertical;
    show_horizontal = next_horizontal;
  }

  return measure(show_vertical, show_horizontal);
}

ScrollSurfaceLayout WorkspaceShell::ComputeCompareScrollLayout(
    const SDL_FRect& rect,
    const CompareSurfaceLayout& surface,
    const CompareTabState& compare_tab) const {
  // Row space is the on-screen (wrapped) one in both modes -- identical to the
  // presentation row count while wrap is off. Wrap also removes horizontal scroll:
  // report a content width that fits, so no horizontal scrollbar is produced.
  const std::size_t content_columns =
      compare_tab.right_viewport.soft_wrap() ? 0 : compare_tab.max_visual_columns;
  return ComputeScrollSurfaceLayout(rect, CompareTabVisualRowCount(compare_tab),
                                  surface.visible_rows,
                                    compare_tab.scroll_row, content_columns,
                                    surface.visible_columns, compare_tab.horizontal_scroll);
}

TextGridInteractionLayout WorkspaceShell::BuildCompareRightInteractionLayout(
    const CompareSurfaceLayout& surface,
    CompareTabState& compare_tab) const {
  compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(surface.visible_rows),
                                             surface.right_visible_columns);
  compare_tab.right_viewport.SetHorizontalScroll(compare_tab.horizontal_scroll);
  return ComputeTextGridInteractionLayout(
      MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
               static_cast<float>(surface.visible_rows) * surface.line_height),
      surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
      text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab.scroll_row)),
      // On-screen rows, not model rows: `scroll_line` above is one, so a mismatched
      // `line_count` clamped hit-tests into a different row space (the pointer-drag
      // path resolved a presentation row as if it were a model row).
      CompareTabVisualRowCount(compare_tab), compare_tab.horizontal_scroll,
      static_cast<std::size_t>(surface.visible_rows), surface.right_visible_columns);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentCompareRowRangeRect(std::size_t start_row,
                                                                    std::size_t end_row) const {
  const auto layout = CurrentWorkspaceLayout();
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (!layout.has_value() || compare_tab == nullptr || end_row <= start_row) {
    return std::nullopt;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout->editor_surface, *compare_tab);
  // Callers name PRESENTATION rows (a selection, a hover); the surface scrolls in
  // on-screen rows, which soft wrap makes a different space. Project the range,
  // covering every wrapped row of the last presentation row in it.
  const std::size_t visual_start = ComparePresentationRowToVisualRow(*compare_tab, start_row);
  const std::size_t visual_end =
      end_row == std::numeric_limits<std::size_t>::max() || end_row <= start_row
          ? end_row
          : ComparePresentationRowToVisualRow(*compare_tab, end_row - 1) +
                compare_tab->wrap_layout.RowSpanForUnit(end_row - 1);
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab->scroll_row));
  const std::size_t visible_end_row = scroll_row + static_cast<std::size_t>(surface_layout.visible_rows);
  const std::size_t rect_start = std::max(visual_start, scroll_row);
  const std::size_t rect_end = std::min(visual_end, visible_end_row);
  if (rect_end <= rect_start) {
    return std::nullopt;
  }

  const float y = surface_layout.rows_y +
                  static_cast<float>(rect_start - scroll_row) * surface_layout.line_height;
  const float h = static_cast<float>(rect_end - rect_start) * surface_layout.line_height;
  const float width = surface_layout.gutter_width + surface_layout.left_width +
                      surface_layout.divider_width + surface_layout.gutter_width +
                      surface_layout.right_width;
  return DirtyRectWithHalo(MakeRect(surface_layout.left_x, y, width, h));
}

std::optional<SDL_FRect> WorkspaceShell::CurrentCompareRowToBottomRect(std::size_t start_row) const {
  const auto row_rect =
      CurrentCompareRowRangeRect(start_row, std::numeric_limits<std::size_t>::max());
  if (!row_rect.has_value()) {
    return std::nullopt;
  }

  const auto layout = CurrentWorkspaceLayout();
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (!layout.has_value() || compare_tab == nullptr) {
    return row_rect;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout->editor_surface, *compare_tab);
  const SDL_FRect content_rect = MakeRect(surface_layout.left_x, surface_layout.rows_y,
                                          row_rect->w,
                                          static_cast<float>(surface_layout.visible_rows) *
                                              surface_layout.line_height);
  return MakeRect(content_rect.x, row_rect->y, content_rect.w,
                  std::max(0.0f, content_rect.y + content_rect.h - row_rect->y));
}

int WorkspaceShell::CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const {
  return std::max(0, static_cast<int>(CompareTabVisualRowCount(compare_tab)) -
                            std::max(1, visible_rows));
}

void WorkspaceShell::ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const {
  compare_tab.scroll_row =
      std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

std::size_t WorkspaceShell::CompareMaxScrollColumn(const CompareTabState& compare_tab,
                                                   std::size_t visible_columns) const {
  // Wrapped panes have no horizontal extent past the viewport by construction.
  if (compare_tab.right_viewport.soft_wrap() ||
      compare_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return compare_tab.max_visual_columns - visible_columns;
}

// Scroll the smallest amount that brings the selected presentation row's rows on
// screen. Under soft wrap a presentation row spans several on-screen rows, so
// revealing it means bringing its FIRST row above the fold and its LAST row below
// it; with wrap off both collapse to the row itself and this is the old two-branch
// nudge. Kept in one place because the reveal ran from three call sites, each with
// its own copy of the arithmetic.
void WorkspaceShell::RevealCompareSelectedRow(CompareTabState& compare_tab,
                                              int visible_rows) const {
  const int rows = std::max(1, visible_rows);
  const int first =
      static_cast<int>(ComparePresentationRowToVisualRow(compare_tab, compare_tab.selected_row));
  const int span = static_cast<int>(compare_tab.wrap_layout.RowSpanForUnit(compare_tab.selected_row));
  const int last = first + std::max(1, span) - 1;
  if (first < compare_tab.scroll_row) {
    compare_tab.scroll_row = first;
  } else if (last >= compare_tab.scroll_row + rows) {
    compare_tab.scroll_row = last - rows + 1;
  }
  ClampCompareScrollRow(compare_tab, visible_rows);
}

void WorkspaceShell::ClampCompareHorizontalScroll(CompareTabState& compare_tab,
                                                  std::size_t visible_columns) const {
  compare_tab.horizontal_scroll =
      std::min(compare_tab.horizontal_scroll, CompareMaxScrollColumn(compare_tab, visible_columns));
}

void WorkspaceShell::RevealActiveCompareSelection() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    return;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
  ClampCompareHorizontalScroll(*compare_tab, surface_layout.visible_columns);
  RevealCompareSelectedRow(*compare_tab, surface_layout.visible_rows);
  if (compare_tab->right_view_active) {
    SyncCompareViewportScroll(*compare_tab);
  }
}

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildCompareTextInputVisual(
    const SDL_FRect& editor_surface) const {
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || !compare_tab->right_view_active) {
    return std::nullopt;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(editor_surface, *compare_tab);
  const TextGridInteractionLayout interaction = ComputeTextGridInteractionLayout(
      MakeRect(surface_layout.right_x, surface_layout.rows_y,
               surface_layout.gutter_width + surface_layout.right_width,
               static_cast<float>(surface_layout.visible_rows) * surface_layout.line_height),
      surface_layout.right_x + surface_layout.gutter_width, surface_layout.rows_y,
      surface_layout.line_height, text_renderer_.CharWidth(),
      static_cast<std::size_t>(std::max(0, compare_tab->scroll_row)),
      CompareTabVisualRowCount(*compare_tab), compare_tab->horizontal_scroll,
      static_cast<std::size_t>(surface_layout.visible_rows), surface_layout.right_visible_columns);
  // The IME box follows the caret's ON-SCREEN cell. It used to be placed at the
  // caret's MODEL row against a scroll offset in presentation rows, which drifts
  // apart as soon as the diff collapses a run -- and wraps a line.
  const CompareRightCaretPlacement placement = CompareRightCaretPlacementFor(
      *compare_tab, compare_tab->right_viewport.cursor_line(),
      compare_tab->right_viewport.cursor_visual_column());
  const float cursor_x = TextGridCursorX(interaction, placement.column);
  const float cursor_y = TextGridLineY(interaction, placement.visual_row);
  return TextInputVisual{
      .surface = TextInputSurface::Editor,
      .area = MakeRect(cursor_x, cursor_y - 1.0f, interaction.char_width, interaction.line_height),
      .text_x = cursor_x,
      .text_y = cursor_y,
      .cursor_x = cursor_x,
      .foreground = theme_.text_primary,
      .background = theme_.editor_background,
      .displayed_text = {},
      .selection_bytes = std::nullopt,
  };
}

void WorkspaceShell::RefreshCompareTabDerivedState(CompareTabState& compare_tab) const {
  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::RefreshCompareTabDerivedState");
  // The model, syntax state, and per-row token buffers derive purely from the two
  // content buffers and the build options. This refresh fires from ~10 call sites (key
  // input, mouse, focus, plugin refresh, external change), many of which leave the
  // compared content untouched. Detect a real change with O(1)/allocation-free signals
  // instead of serializing the whole right buffer and hashing both buffers on every
  // refresh: the editable right pane changes exactly when its viewport `content_revision`
  // (or line ending) advances, so it needs no per-refresh serialize; the read-only left
  // string is hashed directly (allocation-free); the ignore-whitespace option changes the
  // model with no content change. The serialize below then runs only on an actual rebuild.
  // The review/presentation markers further down depend on external git + branch-review
  // state and always refresh.
  const std::uint64_t right_content_revision = compare_tab.right_viewport.content_revision();
  const util::LineEnding right_line_ending = compare_tab.right_viewport.line_ending();
  const std::size_t left_content_hash = std::hash<std::string_view>{}(*compare_tab.left_content);
  const bool ignore_whitespace = compare_tab.build_options.ignore_whitespace;
  const bool content_changed =
      !compare_tab.derived_fingerprint_valid ||
      compare_tab.derived_right_content_revision != right_content_revision ||
      compare_tab.derived_right_line_ending != right_line_ending ||
      compare_tab.derived_left_content_hash != left_content_hash ||
      compare_tab.derived_ignore_whitespace != ignore_whitespace;
  if (content_changed) {
    util::PerformanceTrace::Scope rebuild_scope(
        "WorkspaceShell::RefreshCompareTabDerivedState::RebuildModel");
    std::string right_content = compare_tab.right_viewport.SerializeDocumentText(right_line_ending);
    // In place, recycling the previous build's row storage: a fresh model is two
    // string allocations per row, and this runs on every keystroke in the
    // editable pane (TD-2026-08-13-208).
    // Both buffers are ADOPTED, not copied: the left side is the tab's own
    // shared buffer and the right side is the serialize above, moved in.
    compare::BuildCompareModelInto(compare_tab.model, compare_tab.left_content,
                                   compare::MakeCompareText(std::move(right_content)),
                                   compare_tab.build_options);
    ++compare_tab.model_revision;
    compare_tab.visible_layouts.model_revision = compare_tab.model_revision;
    ResetCompareVisibleLayoutCache(compare_tab);
    compare_tab.derived_right_content_revision = right_content_revision;
    compare_tab.derived_right_line_ending = right_line_ending;
    compare_tab.derived_left_content_hash = left_content_hash;
    compare_tab.derived_left_line_count =
        compare_tab.left_content->empty()
            ? 0
            : static_cast<std::size_t>(std::count(compare_tab.left_content->begin(),
                                                  compare_tab.left_content->end(), '\n')) +
                  1;
    compare_tab.derived_ignore_whitespace = ignore_whitespace;
    compare_tab.derived_fingerprint_valid = true;
  }
  CompareTabReviewRefreshInput review_input{
      .repository_root = context_.current_project_state.root,
      .git_entry = std::nullopt,
      .snapshot_generation = context_.current_project_state.sidebar.git.snapshot_generation,
      .merge_base_commit = context_.current_project_state.sidebar.git.base_ref,
      .content_changed = content_changed,
  };
  const compare::CompareSemanticFileMetadata semantic_before = compare_tab.semantic_file;
  ApplyCompareTabReviewMetadata(compare_tab, review_input);
  // The presentation model is a whole-file row list built from the compare model,
  // the semantic metadata, and the whitespace option — nothing else. Rebuilding it
  // unconditionally made every keystroke and mouse event on a compare tab O(rows)
  // with a fresh row vector; the guard below keeps that cost on the events that
  // actually change one of its inputs. Collapse-state edits refresh the
  // presentation through RefreshCompareTabPresentation directly, which revalidates.
  const bool presentation_inputs_changed =
      !compare_tab.presentation_valid ||
      compare_tab.presentation_built_model_revision != compare_tab.model_revision ||
      compare_tab.presentation_built_show_whitespace != compare_tab.show_whitespace ||
      compare_tab.semantic_file != semantic_before;
  if (presentation_inputs_changed) {
    RefreshCompareTabPresentation(compare_tab);
  } else {
    NormalizeCompareSelectionToModelRow(compare_tab);
  }
  ApplyBranchReviewPresentationMarkers(compare_tab,
                                       context_.current_project_state.branch_review);
  RefreshCompareReviewHeader(compare_tab);
  if (content_changed) {
    // Straight off the blob: detection reads a bounded head, and splitting the
    // whole left side into owned lines to hand it over was one allocation per line
    // of the file — 14 % of a large compare's open (TD-2026-08-06-159).
    compare_tab.left_token_window.Reset(
        compare_tab.model.rows.size(),
        editor::SyntaxHighlighter::InitialState(compare_tab.path, *compare_tab.left_content));
    // The buffer itself, not `Snapshot()`. Detection reads a bounded head through
    // `LineWindow`, whereas asking for the snapshot materialized the WHOLE document
    // into a second owned copy — 20 % of a large compare's open, and retained until
    // the next edit. Nothing downstream here reads the snapshot (the serialization
    // above walks the piece tree directly), so warming it bought nothing.
    compare_tab.right_token_window.Reset(
        compare_tab.model.rows.size(),
        editor::SyntaxHighlighter::InitialState(compare_tab.path,
                                                compare_tab.right_viewport.lines()));
    compare_tab.syntax_highlighting_enabled = true;
    compare_tab.max_visual_columns = CompareMaxVisualColumns(compare_tab.model);
  }
  // Review/presentation markers may have changed even when content did not, so
  // rebuild the scrollbar overlay cache lazily regardless.
  compare_tab.scrollbar_marker_cache_valid = false;
  compare_tab.scrollbar_marker_cache.clear();
  const std::size_t presentation_rows = CompareTabPresentationRowCount(compare_tab);
  if (presentation_rows == 0) {
    compare_tab.selected_row = 0;
    compare_tab.scroll_row = 0;
    return;
  }
  compare_tab.selected_row = std::min(compare_tab.selected_row, presentation_rows - 1);
}

std::size_t WorkspaceShell::CompareRowIndexForRightLine(const CompareTabState& compare_tab,
                                                        std::size_t line_index) const {
  // Thin forwarder onto the canonical free helper so the right-line -> model-row
  // scan lives in exactly one place (CompareTabReview.cpp).
  return CompareTabModelRowForRightLine(compare_tab, line_index);
}

void WorkspaceShell::SyncCompareViewportScroll(CompareTabState& compare_tab) const {
  if (!compare_tab.right_view_active) {
    return;
  }

  if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout_state->editor_surface, compare_tab);
    compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(surface_layout.visible_rows),
                                               surface_layout.right_visible_columns);
    // The two panes scroll horizontally in lock-step off this shared offset. Clamp it
    // against the longest line across BOTH panes; the editable right viewport would
    // otherwise bottleneck the shared scroll to its own document, killing horizontal
    // scrolling whenever the long content lives on the (read-only) left pane.
    ClampCompareHorizontalScroll(compare_tab, surface_layout.visible_columns);
  }
  // Push the (combined-clamped) offset into the right viewport for caret geometry and
  // edit reveal. SetHorizontalScroll re-clamps to the right document internally, but we
  // intentionally do NOT read that narrower value back as the shared offset. Caret-driven
  // horizontal reveal flows the other way through SyncCompareSelectionFromViewport.
  compare_tab.right_viewport.SetHorizontalScroll(compare_tab.horizontal_scroll);
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));
  const std::size_t presentation_scroll_row =
      CompareVisualRowToPresentationRow(compare_tab, scroll_row);
  const std::size_t model_scroll_row =
      compare_tab.presentation.rows.empty()
          ? presentation_scroll_row
          : compare::ComparePresentationToModelRow(compare_tab.presentation,
                                                   presentation_scroll_row);
  compare_tab.right_viewport.SetScrollLine(
      CompareTabRightLineForModelRow(compare_tab, model_scroll_row));
}

void WorkspaceShell::SyncCompareSelectionFromViewport(CompareTabState& compare_tab,
                                                      bool reveal_selection) const {
  if (!compare_tab.right_view_active || compare_tab.model.rows.empty()) {
    return;
  }

  const std::size_t model_row =
      CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.cursor_line());
  compare_tab.selected_row =
      compare::ComparePresentationRowForModelRow(compare_tab.presentation, model_row);
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
  if (reveal_selection) {
    if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
      const WorkspaceLayout layout = *layout_state;
      const CompareSurfaceLayout surface_layout =
          ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
      ClampCompareScrollRow(compare_tab, surface_layout.visible_rows);
      ClampCompareHorizontalScroll(compare_tab, surface_layout.visible_columns);
      RevealCompareSelectedRow(compare_tab, surface_layout.visible_rows);
    }
    SyncCompareViewportScroll(compare_tab);
  } else {
    const std::size_t scroll_model_row =
        CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.scroll_line());
    compare_tab.scroll_row = static_cast<int>(ComparePresentationRowToVisualRow(
        compare_tab,
        compare::ComparePresentationRowForModelRow(compare_tab.presentation, scroll_model_row)));
    SyncCompareViewportScroll(compare_tab);
  }
}

}  // namespace microide::workspace
