#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>

#include "editor/SyntaxHighlighter.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kLargeCompareByteThreshold = 512 * 1024;
constexpr std::size_t kLargeCompareRowThreshold = 6000;

bool ShouldSyntaxHighlightCompareTab(std::string_view left_content,
                                     std::string_view right_content,
                                     const compare::CompareModel& model) {
  return left_content.size() + right_content.size() <= kLargeCompareByteThreshold &&
         model.rows.size() <= kLargeCompareRowThreshold;
}

std::size_t CompareMaxVisualColumns(const compare::CompareModel& model) {
  std::size_t max_columns = 0;
  for (const auto& row : model.rows) {
    max_columns = std::max(
        max_columns,
        std::max(Utf8CodepointCount(row.left_text), Utf8CodepointCount(row.right_text)));
  }
  return max_columns;
}

std::size_t MergeMaxVisualColumns(const compare::MergeDisplayModel& model) {
  std::size_t max_columns = 0;
  for (const auto& row : model.rows) {
    max_columns =
        std::max({max_columns, Utf8CodepointCount(row.incoming_text),
                  Utf8CodepointCount(row.result_text), Utf8CodepointCount(row.current_text)});
  }
  return max_columns;
}

}  // namespace

void WorkspaceShell::OpenComparePicker() {
  if (!sidebar_visible_ || sidebar_mode_ != SidebarMode::Tree) {
    return;
  }

  const auto& entries = directory_tree_.entries();
  if (directory_tree_.selected_index() >= entries.size()) {
    return;
  }

  const auto& entry = entries[directory_tree_.selected_index()];
  if (entry.is_directory) {
    return;
  }

  OpenComparePickerForPath(entry.path);
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return false;
  }
  if (path.empty()) {
    LogMessage("No file is available for compare");
    return false;
  }

  compare_picker_path_ = path.lexically_normal();
  compare_picker_query_.clear();
  compare_picker_commits_ = project::CollectGitFileHistory(project_root_, compare_picker_path_);
  RefreshComparePicker();
  if (compare_picker_matches_.empty()) {
    LogMessage("No git history available for file");
    return false;
  }

  if (!commit_spec.empty()) {
    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < compare_picker_matches_.size(); ++i) {
      const auto& commit = compare_picker_matches_[i];
      const std::string lowered_hash = ToLower(commit.hash);
      const std::string lowered_short_hash = ToLower(commit.short_hash);
      if (StartsWith(lowered_hash, lowered_commit_spec) ||
          StartsWith(lowered_short_hash, lowered_commit_spec)) {
        matching_indices.push_back(i);
      }
    }

    if (matching_indices.empty()) {
      LogMessage("No compare commit matches: " + std::string(commit_spec));
      return false;
    }
    if (matching_indices.size() > 1) {
      LogMessage("Compare commit is ambiguous: " + std::string(commit_spec));
      return false;
    }

    compare_picker_selected_index_ = matching_indices.front();
    OpenSelectedCompareCommit();
    return true;
  }

  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::CommitPicker;
  focus_ = FocusTarget::Overlay;
  ResetOverlayScroll();
  LogMessage("Compare picker opened");
  return true;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const project::GitCommitEntry& commit,
    std::size_t selected_row) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const auto content = project::ReadGitFileAtCommit(project_root_, normalized_path, commit.hash);
  if (!content.has_value()) {
    return std::nullopt;
  }

  const std::optional<std::string> working_content = ReadFileText(normalized_path);
  auto compare_tab = BuildCompareTabFromBuffers(normalized_path, content->exists ? content->content : "",
                                                working_content.value_or(""), commit.short_hash,
                                                "Working tree", selected_row, true);
  if (compare_tab.has_value() && compare_tab->compare.has_value()) {
    compare_tab->compare->commit_hash = commit.hash;
    compare_tab->compare->right_ref = "WORKTREE";
  }
  return compare_tab;
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
  compare_tab.title = "compare: " + normalized_path.filename().string();
  compare_tab.commit_hash = left_label;
  compare_tab.left_label = std::move(left_label);
  compare_tab.right_label = std::move(right_label);
  compare_tab.persistable = persistable;
  compare_tab.model = compare::BuildCompareModel(left_content, right_content);
  if (ShouldSyntaxHighlightCompareTab(left_content, right_content, compare_tab.model)) {
    const auto left_lines = SplitSyntaxLines(left_content);
    const auto right_lines = SplitSyntaxLines(right_content);
    compare_tab.left_initial_syntax_state =
        editor::SyntaxHighlighter::InitialState(normalized_path, left_lines);
    compare_tab.right_initial_syntax_state =
        editor::SyntaxHighlighter::InitialState(normalized_path, right_lines);
    compare_tab.left_tokens_by_row.reserve(compare_tab.model.rows.size());
    compare_tab.right_tokens_by_row.reserve(compare_tab.model.rows.size());
    editor::SyntaxState left_state = compare_tab.left_initial_syntax_state;
    editor::SyntaxState right_state = compare_tab.right_initial_syntax_state;
    for (const auto& compare_row : compare_tab.model.rows) {
      const bool reuse_tokens =
          compare_row.kind == compare::CompareRowKind::Unchanged && compare_row.left_line > 0 &&
          compare_row.right_line > 0 && compare_row.left_text == compare_row.right_text &&
          left_state.definition_id == right_state.definition_id &&
          left_state.region_id == right_state.region_id;
      if (reuse_tokens) {
        editor::HighlightedLine highlighted =
            editor::SyntaxHighlighter::HighlightLine(compare_row.left_text, normalized_path, left_state);
        left_state = highlighted.end_state;
        right_state = highlighted.end_state;
        compare_tab.left_tokens_by_row.push_back(highlighted.tokens);
        compare_tab.right_tokens_by_row.push_back(std::move(highlighted.tokens));
        continue;
      }

      if (compare_row.left_line > 0) {
        editor::HighlightedLine highlighted =
            editor::SyntaxHighlighter::HighlightLine(compare_row.left_text, normalized_path, left_state);
        left_state = highlighted.end_state;
        compare_tab.left_tokens_by_row.push_back(std::move(highlighted.tokens));
      } else {
        compare_tab.left_tokens_by_row.push_back({});
      }

      if (compare_row.right_line > 0) {
        editor::HighlightedLine highlighted =
            editor::SyntaxHighlighter::HighlightLine(compare_row.right_text, normalized_path, right_state);
        right_state = highlighted.end_state;
        compare_tab.right_tokens_by_row.push_back(std::move(highlighted.tokens));
      } else {
        compare_tab.right_tokens_by_row.push_back({});
      }
    }
  }
  compare_tab.selected_row = compare_tab.model.rows.empty()
                                 ? 0
                                 : std::min(selected_row, compare_tab.model.rows.size() - 1);
  compare_tab.scroll_row = 0;
  compare_tab.max_visual_columns = CompareMaxVisualColumns(compare_tab.model);

  return TabEntry{
      .kind = TabEntry::Kind::Compare,
      .path = normalized_path,
      .title = compare_tab.title,
      .editor_state = std::nullopt,
      .compare = std::move(compare_tab),
      .merge = std::nullopt,
  };
}

std::vector<std::vector<editor::SyntaxTokenKind>> WorkspaceShell::HighlightBufferTokens(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines) const {
  std::vector<std::vector<editor::SyntaxTokenKind>> tokens_by_line;
  tokens_by_line.reserve(lines.size());
  editor::SyntaxState state = editor::SyntaxHighlighter::InitialState(path, lines);
  for (const std::string& line : lines) {
    editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(line, path, state);
    state = highlighted.end_state;
    tokens_by_line.push_back(std::move(highlighted.tokens));
  }
  return tokens_by_line;
}

void WorkspaceShell::RefreshMergeTabDerivedState(MergeTabState& merge_tab) const {
  merge_tab.display_model = compare::BuildMergeDisplayModel(merge_tab.model);
  const std::string result_text = compare::MergeResultText(merge_tab.model);
  merge_tab.result_viewport.LoadContent(result_text, merge_tab.output_path, merge_tab.result_line_ending);
  const std::optional<std::string> persisted_output = ReadFileText(merge_tab.output_path);
  merge_tab.result_viewport.SetDirty(!persisted_output.has_value() || *persisted_output != result_text);
  merge_tab.result_tokens = HighlightBufferTokens(merge_tab.output_path, merge_tab.result_viewport.lines());
  merge_tab.max_visual_columns = MergeMaxVisualColumns(merge_tab.display_model);
  if (merge_tab.display_model.hunks.empty()) {
    merge_tab.selected_hunk = 0;
    merge_tab.scroll_row = 0;
    return;
  }
  merge_tab.selected_hunk =
      std::min(merge_tab.selected_hunk, merge_tab.display_model.hunks.size() - 1);
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildMergeTabEntry(
    const std::filesystem::path& base_path,
    const std::filesystem::path& incoming_path,
    const std::filesystem::path& current_path,
    const std::filesystem::path& output_path) const {
  const std::filesystem::path normalized_base = base_path.lexically_normal();
  const std::filesystem::path normalized_incoming = incoming_path.lexically_normal();
  const std::filesystem::path normalized_current = current_path.lexically_normal();
  const std::filesystem::path normalized_output = output_path.lexically_normal();

  const std::optional<std::string> base_text = ReadFileText(normalized_base);
  const std::optional<std::string> incoming_text = ReadFileText(normalized_incoming);
  const std::optional<std::string> current_text = ReadFileText(normalized_current);
  if (!base_text.has_value() || !incoming_text.has_value() || !current_text.has_value()) {
    return std::nullopt;
  }

  auto merge_tab = BuildMergeTabFromBuffers(
      normalized_output.empty() ? normalized_current : normalized_output, *base_text, *incoming_text,
      *current_text, RelativePathLabel(project_root_, normalized_incoming),
      RelativePathLabel(project_root_, normalized_output.empty() ? normalized_current : normalized_output),
      RelativePathLabel(project_root_, normalized_current), 0, true);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    return std::nullopt;
  }
  merge_tab->merge->base_path = normalized_base;
  merge_tab->merge->incoming_path = normalized_incoming;
  merge_tab->merge->current_path = normalized_current;
  return merge_tab;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildMergeTabFromBuffers(
    const std::filesystem::path& output_path,
    std::string base_content,
    std::string incoming_content,
    std::string current_content,
    std::string incoming_label,
    std::string result_label,
    std::string current_label,
    std::size_t selected_hunk,
    bool persistable) const {
  const std::filesystem::path normalized_output = output_path.lexically_normal();
  const std::optional<std::string> output_text =
      normalized_output.empty() ? std::nullopt : ReadFileText(normalized_output);

  MergeTabState merge_tab;
  merge_tab.base_path = normalized_output;
  merge_tab.incoming_path = normalized_output;
  merge_tab.current_path = normalized_output;
  merge_tab.output_path = normalized_output;
  merge_tab.title = "merge: " + normalized_output.filename().string();
  merge_tab.incoming_label = std::move(incoming_label);
  merge_tab.result_label = std::move(result_label);
  merge_tab.current_label = std::move(current_label);
  merge_tab.persistable = persistable;
  const std::string& line_ending_source =
      output_text.has_value() ? *output_text : (!current_content.empty() ? current_content : base_content);
  merge_tab.result_line_ending = DetectLineEnding(line_ending_source);
  merge_tab.model = compare::BuildMergeModel(base_content, incoming_content, current_content);
  merge_tab.incoming_tokens = HighlightBufferTokens(normalized_output, merge_tab.model.incoming_lines);
  merge_tab.current_tokens = HighlightBufferTokens(normalized_output, merge_tab.model.current_lines);
  merge_tab.selected_hunk = selected_hunk;
  merge_tab.scroll_row = 0;
  merge_tab.result_viewport.SetPath(merge_tab.output_path);
  RefreshMergeTabDerivedState(merge_tab);
  return TabEntry{
      .kind = TabEntry::Kind::Merge,
      .path = merge_tab.output_path,
      .title = merge_tab.title,
      .editor_state = std::nullopt,
      .compare = std::nullopt,
      .merge = std::move(merge_tab),
  };
}

void WorkspaceShell::RefreshComparePicker() {
  compare_picker_matches_.clear();
  compare_picker_selected_index_ = 0;

  const std::string lowered_query = ToLower(compare_picker_query_);
  for (const auto& commit : compare_picker_commits_) {
    if (!lowered_query.empty()) {
      const std::string text = ToLower(commit.short_hash + " " + commit.subject);
      if (text.find(lowered_query) == std::string::npos) {
        continue;
      }
    }
    compare_picker_matches_.push_back(commit);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  if (compare_picker_matches_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(compare_picker_selected_index_);
  const int max_index = static_cast<int>(compare_picker_matches_.size()) - 1;
  compare_picker_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  if (compare_picker_matches_.empty() ||
      compare_picker_selected_index_ >= compare_picker_matches_.size()) {
    return;
  }

  OpenComparison(compare_picker_matches_[compare_picker_selected_index_]);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  if (const auto existing_index =
          FindOpenCompareTabIndex(compare_picker_path_, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    SyncActiveEditorTab();
    active_tab_index_ = *existing_index;
    RevealActiveCompareSelection();
    EnsureActiveTabVisible();
    overlay_visible_ = false;
    focus_ = FocusTarget::Editor;
    return;
  }
  auto compare_tab = BuildCompareTabEntry(compare_picker_path_, commit);
  if (!compare_tab.has_value()) {
    LogMessage("Failed to read file content at selected commit");
    return;
  }

  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*compare_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveCompareSelection();
  EnsureActiveTabVisible();
  overlay_visible_ = false;
  focus_ = FocusTarget::Editor;
  LogMessage("Comparison opened");
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  if (const auto existing_index = FindOpenMergeTabIndex(output_path); existing_index.has_value()) {
    SyncActiveEditorTab();
    active_tab_index_ = *existing_index;
    RevealActiveMergeSelection();
    EnsureActiveTabVisible();
    focus_ = FocusTarget::Editor;
    return true;
  }
  auto merge_tab = BuildMergeTabEntry(base_path, incoming_path, current_path, output_path);
  if (!merge_tab.has_value()) {
    LogMessage("Failed to open merge inputs");
    return false;
  }

  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*merge_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveMergeSelection();
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  LogMessage("Merge editor opened");
  return true;
}

void WorkspaceShell::OpenWorkingFileFromCompare() {
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty()) {
    return;
  }

  const auto& row = compare_tab->model.rows[compare_tab->selected_row];
  int target_line = row.right_line;
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row + 1; i < compare_tab->model.rows.size(); ++i) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row; i-- > 0;) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }

  OpenFile(compare_tab->path);
  if (target_line > 0) {
    text_viewport_.MoveCursorTo(static_cast<std::size_t>(target_line - 1), 0);
  }
}

void WorkspaceShell::OpenMergeResultFile() {
  const MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->output_path.empty()) {
    return;
  }
  OpenFile(merge_tab->output_path);
}

void WorkspaceShell::MoveCompareSelection(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(compare_tab->selected_row);
  const int max_index = static_cast<int>(compare_tab->model.rows.size()) - 1;
  compare_tab->selected_row = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealActiveCompareSelection();
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.hunks.empty()) {
    return;
  }

  int target = 0;
  for (std::size_t i = 0; i < compare_tab->model.hunks.size(); ++i) {
    if (compare_tab->model.hunks[i].start_row >= static_cast<int>(compare_tab->selected_row)) {
      target = static_cast<int>(i);
      break;
    }
    target = static_cast<int>(i);
  }
  target = std::clamp(target + delta, 0, static_cast<int>(compare_tab->model.hunks.size()) - 1);
  compare_tab->selected_row =
      static_cast<std::size_t>(compare_tab->model.hunks[static_cast<std::size_t>(target)].start_row);
  RevealActiveCompareSelection();
}

void WorkspaceShell::ScrollCompareRows(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0 || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const int max_scroll = CompareMaxScrollRow(*compare_tab, surface_layout.visible_rows);
  compare_tab->scroll_row = std::clamp(compare_tab->scroll_row + delta, 0, max_scroll);
}

void WorkspaceShell::ScrollCompareColumns(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0 || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const std::size_t max_scroll =
      CompareMaxScrollColumn(*compare_tab, surface_layout.visible_columns);
  const long long target_scroll =
      static_cast<long long>(compare_tab->horizontal_scroll) + static_cast<long long>(delta);
  compare_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL, static_cast<long long>(max_scroll)));
}

void WorkspaceShell::MoveMergeSelection(int delta) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->display_model.hunks.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(merge_tab->selected_hunk);
  const int max_index = static_cast<int>(merge_tab->display_model.hunks.size()) - 1;
  merge_tab->selected_hunk = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealActiveMergeSelection();
}

void WorkspaceShell::ScrollMergeColumns(int delta) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || delta == 0 || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const std::size_t max_scroll =
      MergeMaxScrollColumn(*merge_tab, surface_layout.visible_columns);
  const long long target_scroll =
      static_cast<long long>(merge_tab->horizontal_scroll) + static_cast<long long>(delta);
  merge_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL, static_cast<long long>(max_scroll)));
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->model.hunks.empty()) {
    return;
  }

  const std::size_t selected_hunk =
      std::min(merge_tab->selected_hunk, merge_tab->model.hunks.size() - 1);
  merge_tab->model.hunks[selected_hunk].choice = choice;
  RefreshMergeTabDerivedState(*merge_tab);
  RevealActiveMergeSelection();
}

void WorkspaceShell::ApplyMergeChoiceToAll(compare::MergeChoice choice) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->model.hunks.empty()) {
    return;
  }

  for (auto& hunk : merge_tab->model.hunks) {
    hunk.choice = choice;
  }
  RefreshMergeTabDerivedState(*merge_tab);
  RevealActiveMergeSelection();
}

}  // namespace microide::workspace
