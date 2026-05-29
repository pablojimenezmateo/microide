#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

#include "editor/SyntaxHighlighter.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/CompareTabReview.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr float kCompareDividerHitWidth = 12.0f;

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
  if (!content.has_value()) {
    return std::nullopt;
  }

  const std::optional<std::string> working_content = util::ReadTextFile(normalized_path);
  auto compare_tab = BuildCompareTabFromBuffers(normalized_path, content->exists ? content->content : "",
                                                working_content.value_or(""), commit.short_hash,
                                                "Working tree", selected_row, true);
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
  if (!left_content.has_value()) {
    return std::nullopt;
  }

  std::string right_content;
  if (compare_tab.right_ref == "WORKTREE") {
    right_content = compare_tab.right_viewport.dirty()
                        ? util::SerializeLines(compare_tab.right_viewport.lines(),
                                               compare_tab.right_viewport.line_ending())
                        : util::ReadTextFile(right_source_path).value_or("");
  } else {
    const auto right_commit_content =
        project::ReadGitFileAtCommit(context_.current_project_state.root, right_source_path, compare_tab.right_ref);
    if (!right_commit_content.has_value()) {
      return std::nullopt;
    }
    right_content =
        right_commit_content->exists ? right_commit_content->content : std::string{};
  }

  auto rebuilt = BuildCompareTabFromBuffers(normalized_path,
                                            left_content->exists ? left_content->content : "",
                                            std::move(right_content), compare_tab.left_label,
                                            compare_tab.right_label, compare_tab.selected_row,
                                            compare_tab.persistable);
  if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
    return std::nullopt;
  }

  rebuilt->compare->commit_hash = compare_tab.commit_hash;
  rebuilt->compare->right_ref = compare_tab.right_ref;
  rebuilt->compare->left_path = left_source_path;
  rebuilt->compare->right_path = right_source_path;
  rebuilt->compare->scroll_row = compare_tab.scroll_row;
  rebuilt->compare->horizontal_scroll = compare_tab.horizontal_scroll;
  rebuilt->compare->build_options = compare_tab.build_options;
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
    bool persistable) const {
  const std::filesystem::path normalized_path = path.lexically_normal();

  CompareTabState compare_tab;
  compare_tab.path = normalized_path;
  compare_tab.left_path = normalized_path;
  compare_tab.right_path = normalized_path;
  compare_tab.title = "compare: " + normalized_path.filename().string();
  compare_tab.commit_hash = left_label;
  compare_tab.left_label = std::move(left_label);
  compare_tab.right_label = std::move(right_label);
  compare_tab.left_content = std::move(left_content);
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

SDL_FRect WorkspaceShell::CompareDividerHitRect(const SDL_FRect& editor_surface,
                                                const CompareSurfaceLayout& surface) const {
  const float hit_width = std::max(kCompareDividerHitWidth, surface.divider_width);
  const float center_x = surface.center_x + surface.divider_width * 0.5f;
  return MakeRect(center_x - hit_width * 0.5f, editor_surface.y, hit_width, editor_surface.h);
}

bool WorkspaceShell::ActiveTabIsCompare() const {
  return context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size() &&
         context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].kind == TabEntry::Kind::Compare &&
         context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].compare.has_value();
}

WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].compare.value();
}

const WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() const {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].compare.value();
}

WorkspaceShell::CompareSurfaceLayout WorkspaceShell::ComputeCompareSurfaceLayout(
    const SDL_FRect& rect,
    const CompareTabState& compare_tab) const {
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    CompareSurfaceLayout layout;
    layout.line_height = text_renderer_.LineHeight();
    std::array<char, 20> line_count_buf;
    const auto [line_count_end, _] = std::to_chars(
        line_count_buf.data(), line_count_buf.data() + line_count_buf.size(),
        CompareTabPresentationRowCount(compare_tab) + 1);
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
  return ComputeScrollSurfaceLayout(rect, CompareTabPresentationRowCount(compare_tab),
                                  surface.visible_rows,
                                    compare_tab.scroll_row, compare_tab.max_visual_columns,
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
      compare_tab.model.rows.size(), compare_tab.horizontal_scroll,
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
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab->scroll_row));
  const std::size_t visible_end_row = scroll_row + static_cast<std::size_t>(surface_layout.visible_rows);
  const std::size_t rect_start = std::max(start_row, scroll_row);
  const std::size_t rect_end = std::min(end_row, visible_end_row);
  if (rect_end <= rect_start) {
    return std::nullopt;
  }

  const float y = surface_layout.rows_y +
                  static_cast<float>(rect_start - scroll_row) * surface_layout.line_height;
  const float h = static_cast<float>(rect_end - rect_start) * surface_layout.line_height;
  const float width = surface_layout.gutter_width + surface_layout.left_width +
                      surface_layout.divider_width + surface_layout.gutter_width +
                      surface_layout.right_width;
  return MakeRect(surface_layout.left_x, y - 1.0f, width, h);
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
  return std::max(0, static_cast<int>(CompareTabPresentationRowCount(compare_tab)) -
                            std::max(1, visible_rows));
}

void WorkspaceShell::ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const {
  compare_tab.scroll_row =
      std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

std::size_t WorkspaceShell::CompareMaxScrollColumn(const CompareTabState& compare_tab,
                                                   std::size_t visible_columns) const {
  if (compare_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return compare_tab.max_visual_columns - visible_columns;
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
  if (compare_tab->selected_row < static_cast<std::size_t>(compare_tab->scroll_row)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row);
  } else if (compare_tab->selected_row >=
             static_cast<std::size_t>(compare_tab->scroll_row + surface_layout.visible_rows)) {
    compare_tab->scroll_row =
        static_cast<int>(compare_tab->selected_row) - surface_layout.visible_rows + 1;
  }
  ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
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
      compare_tab->model.rows.size(), compare_tab->horizontal_scroll,
      static_cast<std::size_t>(surface_layout.visible_rows), surface_layout.right_visible_columns);
  const std::size_t model_row =
      CompareRowIndexForRightLine(*compare_tab, compare_tab->right_viewport.cursor_line());
  const float cursor_x =
      TextGridCursorX(interaction, compare_tab->right_viewport.cursor_visual_column());
  const float cursor_y = TextGridLineY(interaction, model_row);
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
  const std::string right_content =
      util::SerializeLines(compare_tab.right_viewport.lines(),
                           compare_tab.right_viewport.line_ending());
  compare_tab.model =
      compare::BuildCompareModel(compare_tab.left_content, right_content, compare_tab.build_options);
  ++compare_tab.model_revision;
  CompareTabReviewRefreshInput review_input{
      .repository_root = context_.current_project_state.root,
      .git_entry = std::nullopt,
      .snapshot_generation = context_.current_project_state.sidebar.git.snapshot_generation,
      .merge_base_commit = context_.current_project_state.sidebar.git.base_ref,
  };
  ApplyCompareTabReviewMetadata(compare_tab, review_input);
  RefreshCompareTabPresentation(compare_tab);
  ApplyBranchReviewPresentationMarkers(compare_tab,
                                       context_.current_project_state.branch_review);
  RefreshCompareReviewHeader(compare_tab);
  const auto left_lines = SplitSyntaxLines(compare_tab.left_content);
  const auto right_lines = SplitSyntaxLines(right_content);
  compare_tab.left_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(compare_tab.path, left_lines);
  compare_tab.right_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(compare_tab.path, right_lines);
  compare_tab.left_current_syntax_state = compare_tab.left_initial_syntax_state;
  compare_tab.right_current_syntax_state = compare_tab.right_initial_syntax_state;
  compare_tab.left_tokens_by_row.assign(compare_tab.model.rows.size(), {});
  compare_tab.right_tokens_by_row.assign(compare_tab.model.rows.size(), {});
  compare_tab.syntax_rows_tokenized = 0;
  compare_tab.syntax_highlighting_enabled = true;
  compare_tab.max_visual_columns = CompareMaxVisualColumns(compare_tab.model);
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
  if (compare_tab.model.rows.empty()) {
    return 0;
  }

  const int target_line = static_cast<int>(line_index + 1);
  for (std::size_t i = 0; i < compare_tab.model.rows.size(); ++i) {
    const auto& row = compare_tab.model.rows[i];
    if (row.right_line == target_line) {
      return i;
    }
    if (row.right_line > target_line) {
      return i;
    }
  }
  return compare_tab.model.rows.size() - 1;
}

std::size_t WorkspaceShell::CompareRightLineForRow(const CompareTabState& compare_tab,
                                                   std::size_t row_index) const {
  if (compare_tab.right_viewport.line_count() == 0) {
    return 0;
  }
  if (row_index >= compare_tab.model.rows.size()) {
    return compare_tab.right_viewport.line_count() - 1;
  }

  const auto& row = compare_tab.model.rows[row_index];
  if (row.right_line > 0) {
    return static_cast<std::size_t>(row.right_line - 1);
  }
  for (std::size_t i = row_index + 1; i < compare_tab.model.rows.size(); ++i) {
    if (compare_tab.model.rows[i].right_line > 0) {
      return static_cast<std::size_t>(compare_tab.model.rows[i].right_line - 1);
    }
  }
  return compare_tab.right_viewport.line_count() - 1;
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
  }
  compare_tab.right_viewport.SetHorizontalScroll(compare_tab.horizontal_scroll);
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));
  const std::size_t model_scroll_row =
      compare_tab.presentation.rows.empty()
          ? scroll_row
          : compare::ComparePresentationToModelRow(compare_tab.presentation, scroll_row);
  compare_tab.right_viewport.SetScrollLine(CompareRightLineForRow(compare_tab, model_scroll_row));
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
}

void WorkspaceShell::SyncCompareSelectionFromViewport(CompareTabState& compare_tab,
                                                      bool reveal_selection) const {
  if (!compare_tab.right_view_active || compare_tab.model.rows.empty()) {
    return;
  }

  const std::size_t model_row =
      CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.cursor_line());
  compare_tab.selected_row =
      compare::ComparePresentationModelRowIndex(compare_tab.presentation, model_row)
          .value_or(model_row);
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
  if (reveal_selection) {
    if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
      const WorkspaceLayout layout = *layout_state;
      const CompareSurfaceLayout surface_layout =
          ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
      ClampCompareScrollRow(compare_tab, surface_layout.visible_rows);
      ClampCompareHorizontalScroll(compare_tab, surface_layout.visible_columns);
      if (compare_tab.selected_row < static_cast<std::size_t>(compare_tab.scroll_row)) {
        compare_tab.scroll_row = static_cast<int>(compare_tab.selected_row);
      } else if (compare_tab.selected_row >=
                 static_cast<std::size_t>(compare_tab.scroll_row + surface_layout.visible_rows)) {
        compare_tab.scroll_row =
            static_cast<int>(compare_tab.selected_row) - surface_layout.visible_rows + 1;
      }
      ClampCompareScrollRow(compare_tab, surface_layout.visible_rows);
    }
    SyncCompareViewportScroll(compare_tab);
  } else {
    const std::size_t scroll_model_row =
        CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.scroll_line());
    compare_tab.scroll_row = static_cast<int>(
        compare::ComparePresentationModelRowIndex(compare_tab.presentation, scroll_model_row)
            .value_or(scroll_model_row));
    SyncCompareViewportScroll(compare_tab);
  }
}

}  // namespace microide::workspace
