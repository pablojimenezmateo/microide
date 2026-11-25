#include "util/InlineVector.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <functional>
#include <cstring>
#include <limits>
#include <utility>

#include "editor/SyntaxHighlighter.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/state/SurfaceTokenWindow.h"
#include "workspace/git/CompareTabLoad.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareVisibleLayoutCache.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::size_t CommonPrefixLength(std::string_view a, std::string_view b) {
  const std::size_t limit = std::min(a.size(), b.size());
  constexpr std::size_t kBlock = 4096;
  std::size_t at = 0;
  while (at + kBlock <= limit && std::memcmp(a.data() + at, b.data() + at, kBlock) == 0) {
    at += kBlock;
  }
  while (at < limit && a[at] == b[at]) {
    ++at;
  }
  return at;
}

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

}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const project::GitCommitEntry& commit,
    std::size_t selected_row) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const auto content =
      project::ReadGitFileAtCommit(context_.current_project_state.root,
                                   context_.current_project_state.launcher(), normalized_path,
                                   commit.hash);
  if (!content.has_value() || content->truncated) {
    return std::nullopt;
  }

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
    AdoptLiveBufferForCompareRightSide(context_.current_project_state.editor_groups,
                                       *compare_tab->compare);
    RefreshCompareTabDerivedState(*compare_tab->compare);
  }
  return compare_tab;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const CompareTabState& compare_tab,
    const CompareRevisionSides* sides) const {
  if (compare_tab.load_pending) {
    return std::nullopt;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path left_source_path =
      (compare_tab.left_path.empty() ? normalized_path : compare_tab.left_path).lexically_normal();
  const std::filesystem::path right_source_path =
      (compare_tab.right_path.empty() ? normalized_path : compare_tab.right_path).lexically_normal();
  std::optional<project::GitFileContentAtCommit> left_content;
  if (sides != nullptr && sides->left.has_value()) {
    left_content = sides->left;
  } else if (CompareLeftSideIsHeld(compare_tab)) {
    const bool exists = *compare_tab.build_options.left_exists;
    left_content = project::GitFileContentAtCommit{
        .exists = exists, .content = exists ? *compare_tab.left_content : std::string{}};
  }
  if (!left_content.has_value()) {
    left_content = project::ReadGitFileAtCommit(context_.current_project_state.root,
                                                context_.current_project_state.launcher(), left_source_path,
                                                compare_tab.commit_hash);
  }
  if (!left_content.has_value() || left_content->truncated) {
    return std::nullopt;
  }

  std::string right_content;
  bool right_exists = true;
  if (compare_tab.right_ref == "WORKTREE") {
    if (compare_tab.right_viewport.dirty()) {
      right_content = compare_tab.right_viewport.SerializeDocumentText();
    } else {
      const util::TextFileReadResult working = util::ReadTextFileClassified(right_source_path);
      if (working.is_error()) {
        return std::nullopt;
      }
      right_content = working.content;
      right_exists = working.status != util::TextFileReadStatus::Missing;
    }
  } else {
    const auto right_commit_content =
        sides != nullptr && sides->right.has_value()
            ? sides->right
            : project::ReadGitFileAtCommit(context_.current_project_state.root,
                                           context_.current_project_state.launcher(), right_source_path,
                                           compare_tab.right_ref);
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
    AdoptLiveBufferForCompareRightSide(context_.current_project_state.editor_groups,
                                       *rebuilt->compare, &compare_tab.right_viewport);
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
  return ActiveCompareTab() != nullptr;
}

WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() {
  return const_cast<CompareTabState*>(std::as_const(*this).ActiveCompareTab());
}

const WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() const {
  return context_.current_project_state.focused_group().active_compare();
}

workspace::CompareSurfaceLayout WorkspaceShell::ComputeCompareSurfaceLayout(const SDL_FRect& rect,
                                                                            const CompareTabState& compare_tab) const {
  const std::size_t gutter_max_line = std::max<std::size_t>(
      {compare_tab.derived_left_line_count, compare_tab.right_viewport.lines().size(), 1});
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    CompareSurfaceLayout layout;
    layout.line_height = editor_text_renderer_.LineHeight();
    std::array<char, 20> line_count_buf;
    const auto [line_count_end, _] = std::to_chars(
        line_count_buf.data(), line_count_buf.data() + line_count_buf.size(), gutter_max_line);
    layout.gutter_width = std::max(
        28.0f,
        editor_text_renderer_.MeasureWidth(std::string_view{
            line_count_buf.data(),
            static_cast<std::size_t>(line_count_end - line_count_buf.data())}) +
            12.0f);
    layout.divider_width = std::max(1.0f, std::ceil(editor_text_renderer_.CharWidth()));
    layout.review_summary_y = rect.y + 4.0f;
    layout.header_y = layout.review_summary_y + layout.line_height;
    layout.rows_y = layout.header_y + layout.line_height + 6.0f;

    const float reserved_width =
        reserve_vertical ? (kWorkspaceScrollbarThickness + kWorkspaceScrollbarInset) : 0.0f;
    const float reserved_height =
        reserve_horizontal ? (kWorkspaceScrollbarThickness + kWorkspaceScrollbarInset) : 0.0f;
    const float char_width = std::max(1.0f, editor_text_renderer_.CharWidth());
    const float min_pane_width = 8.0f + char_width;
    layout.divider_width = std::min(layout.divider_width, std::max(1.0f, rect.w - reserved_width));
    const float margin = std::clamp((rect.w - reserved_width - layout.divider_width) * 0.5f, 0.0f, 8.0f);
    layout.left_x = rect.x + margin;
    const float fixed_width = reserved_width + layout.divider_width + margin * 2.0f;
    layout.gutter_width = std::min(layout.gutter_width, std::max(0.0f, (rect.w - fixed_width) * 0.5f));
    const float content_width = std::max(0.0f, rect.w - fixed_width - layout.gutter_width * 2.0f);
    const float min_fraction = content_width >= min_pane_width * 2.0f
                                   ? min_pane_width / content_width
                                   : 0.5f;
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

  if (compare_tab.right_viewport.soft_wrap()) {
    CompareSurfaceLayout layout = measure(true, false);
    EnsureCompareWrapLayout(compare_tab, true, layout.left_visible_columns,
                            layout.right_visible_columns);
    return layout;
  }
  EnsureCompareWrapLayout(compare_tab, false, 0, 0);

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
      editor_text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab.scroll_row)),
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

  const SDL_FRect surface = ActiveTabSurface(*layout);
  const CompareSurfaceLayout surface_layout = ComputeCompareSurfaceLayout(surface, *compare_tab);
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
  return DirtyRectWithHalo(MakeRect(surface.x, y, surface.w, h));
}

void WorkspaceShell::PopulateCompareSyntaxTokensForWindow(CompareTabState& compare_tab,
                                                          const SDL_FRect& rect) {
  if (!compare_tab.syntax_highlighting_enabled || compare_tab.model.rows.empty() ||
      rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  const CompareSurfaceLayout surface = ComputeCompareSurfaceLayout(rect, compare_tab);
  ClampCompareScrollRow(compare_tab, surface.visible_rows);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  const std::size_t presentation_row_count = compare_tab.presentation.rows.size();
  const auto model_row_for_visual = [&](std::size_t visual_index) -> std::size_t {
    if (presentation_row_count == 0) {
      return compare_tab.model.rows.size();
    }
    const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(
        compare_tab,
        std::min(CompareVisualRowToPresentationRow(compare_tab, visual_index),
                 presentation_row_count - 1));
    if (row == nullptr) {
      return compare_tab.model.rows.size();
    }
    if (row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
      return row->collapsed_run_start_model_row + row->collapsed_run_length;
    }
    return row->model_row_index;
  };
  const std::size_t window_start = model_row_for_visual(visible_start_row);
  const std::size_t window_end =
      std::min(compare_tab.model.rows.size(), model_row_for_visual(visible_end_row) + 1);
  util::InlineVector<std::pair<std::size_t, std::size_t>, 64> hidden_runs;
  if (presentation_row_count > 0) {
    const std::size_t first_presentation =
        std::min(CompareVisualRowToPresentationRow(compare_tab, visible_start_row), presentation_row_count - 1);
    const std::size_t last_presentation =
        std::min(CompareVisualRowToPresentationRow(compare_tab, visible_end_row), presentation_row_count - 1);
    for (std::size_t index = first_presentation; index <= last_presentation; ++index) {
      const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, index);
      if (row == nullptr || row->kind != compare::ComparePresentationRowKind::CollapsedContext ||
          row->collapsed_line_count <= 0) {
        continue;
      }
      std::size_t hidden_start = row->collapsed_run_start_model_row;
      if (row->context_above && index > 0) {
        if (const compare::ComparePresentationRow* above = CompareTabPresentationRowAt(compare_tab, index - 1);
            above != nullptr && above->kind == compare::ComparePresentationRowKind::Model) {
          hidden_start = above->model_row_index + 1;
        }
      }
      if (hidden_runs.size() == hidden_runs.capacity()) {
        break;
      }
      hidden_runs.push_back({hidden_start, hidden_start + static_cast<std::size_t>(row->collapsed_line_count)});
    }
  }
  const auto row_is_drawn = [&hidden_runs](std::size_t row) {
    const auto after = std::upper_bound(hidden_runs.begin(), hidden_runs.end(), row,
                                        [](std::size_t value, const auto& run) { return value < run.first; });
    return after == hidden_runs.begin() || row >= std::prev(after)->second;
  };
  util::PerformanceTrace::Scope syntax_scope("WorkspaceShell::RenderCompareSurface::SyntaxTokens");

  if (compare_tab.token_windows_stale) {
    compare_tab.left_token_window.Reset(
        compare_tab.left_line_rows.size(),
        compare_tab.left_content != nullptr
            ? editor::SyntaxHighlighter::InitialState(compare_tab.path, *compare_tab.left_content)
            : editor::SyntaxState{});
    compare_tab.right_token_window.Reset(
        compare_tab.right_line_rows.size(),
        editor::SyntaxHighlighter::InitialState(compare_tab.path, compare_tab.right_viewport.lines()));
    compare_tab.token_windows_stale = false;
  }

  const std::vector<compare::CompareRow>& rows = compare_tab.model.rows;
  const auto lines_of = [&](int compare::CompareRow::* line, std::size_t line_count) {
    std::size_t first = line_count;
    for (std::size_t row = window_start; row < window_end; ++row) {
      if (rows[row].*line > 0) {
        first = static_cast<std::size_t>(rows[row].*line - 1);
        break;
      }
    }
    std::size_t end = 0;
    for (std::size_t row = window_end; row-- > 0;) {
      if (rows[row].*line > 0) {
        end = static_cast<std::size_t>(rows[row].*line);
        break;
      }
    }
    return std::pair<std::size_t, std::size_t>{std::min(first, end), end};
  };
  const auto [left_start, left_end] = lines_of(&compare::CompareRow::left_line, compare_tab.left_line_rows.size());
  const auto [right_start, right_end] =
      lines_of(&compare::CompareRow::right_line, compare_tab.right_line_rows.size());
  constexpr std::size_t kCompareSyntaxRowsPerFrame = 256;
  const SurfaceTokenWindow::WalkBudget walk_budget{
      kCompareSyntaxRowsPerFrame, std::chrono::steady_clock::now() + kSyntaxWalkTimePerFrame};
  compare_tab.left_token_window.EnsureWindow(
      left_start, left_end, compare_tab.path, walk_budget,
      [&](std::size_t line) -> std::string_view { return rows[compare_tab.left_line_rows[line]].left_text; },
      [&](std::size_t line) { return row_is_drawn(compare_tab.left_line_rows[line]); });
  compare_tab.right_token_window.EnsureWindow(
      right_start, right_end, compare_tab.path, walk_budget,
      [&](std::size_t line) -> std::string_view { return rows[compare_tab.right_line_rows[line]].right_text; },
      [&](std::size_t line) {
        const std::size_t row = compare_tab.right_line_rows[line];
        return row_is_drawn(row) && !CompareRowRightTokensAliasLeft(compare_tab, row);
      });
  const auto behind = [](const SurfaceTokenWindow& window, std::size_t end) {
    return window.frontier() < std::min(end, window.line_count());
  };
  if (behind(compare_tab.left_token_window, left_end) || behind(compare_tab.right_token_window, right_end)) {
    frame_state_.syntax_catch_up_owed = true;
  }
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
      ComputeCompareSurfaceLayout(ActiveTabSurface(layout), *compare_tab);
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
      surface_layout.line_height, editor_text_renderer_.CharWidth(),
      static_cast<std::size_t>(std::max(0, compare_tab->scroll_row)),
      CompareTabVisualRowCount(*compare_tab), compare_tab->horizontal_scroll,
      static_cast<std::size_t>(surface_layout.visible_rows), surface_layout.right_visible_columns);
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
  const std::uint64_t right_content_revision = compare_tab.right_viewport.content_revision();
  const util::LineEnding right_line_ending = compare_tab.right_viewport.line_ending();
  const bool ignore_whitespace = compare_tab.build_options.ignore_whitespace;
  compare_tab.build_options.syntax =
      language_contract_.ResolveDiffSyntax(compare_tab.right_viewport.language_id());
  std::size_t left_first_changed_line = 0;
  std::size_t right_first_changed_line = 0;
  const bool model_stale =
      !compare_tab.derived_fingerprint_valid ||
      compare_tab.derived_right_content_revision != right_content_revision ||
      compare_tab.derived_right_line_ending != right_line_ending ||
      compare_tab.derived_left_content != compare_tab.left_content ||
      compare_tab.derived_ignore_whitespace != ignore_whitespace ||
      compare_tab.derived_syntax != compare_tab.build_options.syntax;
  if (model_stale) {
    util::PerformanceTrace::Scope rebuild_scope(
        "WorkspaceShell::RefreshCompareTabDerivedState::RebuildModel");
    std::string right_content = compare_tab.right_viewport.SerializeDocumentText(right_line_ending);
    const bool windows_describe_model =
        !compare_tab.token_windows_stale && compare_tab.derived_fingerprint_valid &&
        compare_tab.derived_syntax == compare_tab.build_options.syntax && compare_tab.model.right_source != nullptr;
    left_first_changed_line =
        windows_describe_model && compare_tab.derived_left_content == compare_tab.left_content
            ? std::numeric_limits<std::size_t>::max()
            : 0;
    if (windows_describe_model) {
      const std::string& before = *compare_tab.model.right_source;
      const std::size_t shared = CommonPrefixLength(before, right_content);
      right_first_changed_line =
          shared == before.size() && shared == right_content.size()
              ? std::numeric_limits<std::size_t>::max()
              : static_cast<std::size_t>(std::count(right_content.begin(),
                                                    right_content.begin() + static_cast<std::ptrdiff_t>(shared), '\n'));
    }
    compare::BuildCompareModelInto(compare_tab.model, compare_tab.left_content,
                                   compare::MakeCompareText(std::move(right_content)),
                                   compare_tab.build_options);
    const std::size_t left_line_count =
        compare_tab.derived_fingerprint_valid &&
                compare_tab.derived_left_content == compare_tab.left_content
            ? compare_tab.derived_left_line_count
            : CountCompareTextLines(*compare_tab.left_content);
    MarkCompareModelBuilt(compare_tab, compare_tab.build_options, left_line_count);
  }
  const bool content_changed = compare_tab.token_windows_model_revision != compare_tab.model_revision;
  CompareTabReviewRefreshInput review_input{
      .repository_root = context_.current_project_state.root,
      .git_entry = std::nullopt,
      .snapshot_generation = context_.current_project_state.sidebar.git.snapshot_generation,
      .merge_base_commit = context_.current_project_state.sidebar.git.base_ref,
      .content_changed = content_changed,
  };
  const compare::CompareSemanticFileMetadata semantic_before = compare_tab.semantic_file;
  ApplyCompareTabReviewMetadata(compare_tab, review_input);
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
    compare_tab.left_token_window.ResetFrom(
        left_first_changed_line, compare_tab.left_line_rows.size(),
        editor::SyntaxHighlighter::InitialState(compare_tab.path, *compare_tab.left_content));
    compare_tab.right_token_window.ResetFrom(
        right_first_changed_line, compare_tab.right_line_rows.size(),
        editor::SyntaxHighlighter::InitialState(compare_tab.path, compare_tab.right_viewport.lines()));
    compare_tab.token_windows_stale = false;
    compare_tab.syntax_highlighting_enabled = true;
    compare_tab.max_visual_columns = CompareMaxVisualColumns(compare_tab.model);
    compare_tab.token_windows_model_revision = compare_tab.model_revision;
  }
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

void WorkspaceShell::SyncCompareViewportScroll(CompareTabState& compare_tab) const {
  if (!compare_tab.right_view_active) {
    return;
  }

  if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(ActiveTabSurface(*layout_state, &compare_tab), compare_tab);
    compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(surface_layout.visible_rows),
                                               surface_layout.right_visible_columns);
    ClampCompareHorizontalScroll(compare_tab, surface_layout.visible_columns);
  }
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

  const std::size_t model_row = CompareTabModelRowForRightLine(compare_tab, compare_tab.right_viewport.cursor_line());
  compare_tab.selected_row =
      compare::ComparePresentationRowForModelRow(compare_tab.presentation, model_row);
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
  if (reveal_selection) {
    if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
      const WorkspaceLayout layout = *layout_state;
      const CompareSurfaceLayout surface_layout =
          ComputeCompareSurfaceLayout(ActiveTabSurface(layout, &compare_tab), compare_tab);
      ClampCompareScrollRow(compare_tab, surface_layout.visible_rows);
      ClampCompareHorizontalScroll(compare_tab, surface_layout.visible_columns);
      RevealCompareSelectedRow(compare_tab, surface_layout.visible_rows);
    }
    SyncCompareViewportScroll(compare_tab);
  } else {
    const std::size_t scroll_model_row =
        CompareTabModelRowForRightLine(compare_tab, compare_tab.right_viewport.scroll_line());
    compare_tab.scroll_row = static_cast<int>(ComparePresentationRowToVisualRow(
        compare_tab,
        compare::ComparePresentationRowForModelRow(compare_tab.presentation, scroll_model_row)));
    SyncCompareViewportScroll(compare_tab);
  }
}

}
