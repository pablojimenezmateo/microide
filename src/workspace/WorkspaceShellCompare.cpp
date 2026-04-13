#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>

#include "editor/SyntaxHighlighter.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

std::size_t CompareMaxVisualColumns(const compare::CompareModel& model) {
  std::size_t max_columns = 0;
  for (const auto& row : model.rows) {
    max_columns = std::max(
        max_columns,
        std::max(Utf8CodepointCount(row.left_text), Utf8CodepointCount(row.right_text)));
  }
  return max_columns;
}

std::size_t MaxVisualColumnsForLines(const std::vector<std::string>& lines) {
  std::size_t max_columns = 0;
  for (const std::string& line : lines) {
    max_columns = std::max(max_columns, Utf8CodepointCount(line));
  }
  return max_columns;
}

struct ChangedLineSpan {
  std::size_t old_start = 0;
  std::size_t old_end = 0;
  std::size_t new_end = 0;
};

std::optional<ChangedLineSpan> ComputeChangedLineSpan(const std::vector<std::string>& before_lines,
                                                      const std::vector<std::string>& after_lines) {
  std::size_t prefix = 0;
  while (prefix < before_lines.size() && prefix < after_lines.size() &&
         before_lines[prefix] == after_lines[prefix]) {
    ++prefix;
  }
  if (prefix == before_lines.size() && prefix == after_lines.size()) {
    return std::nullopt;
  }

  std::size_t suffix = 0;
  while (suffix < before_lines.size() - prefix && suffix < after_lines.size() - prefix &&
         before_lines[before_lines.size() - 1 - suffix] ==
             after_lines[after_lines.size() - 1 - suffix]) {
    ++suffix;
  }

  return ChangedLineSpan{
      .old_start = prefix,
      .old_end = before_lines.size() - suffix,
      .new_end = after_lines.size() - suffix,
  };
}

bool MatchesLineSegment(const std::vector<std::string>& lines,
                        std::size_t start_line,
                        const std::vector<std::string>& candidate_lines) {
  if (start_line > lines.size() || start_line + candidate_lines.size() > lines.size()) {
    return false;
  }
  return std::equal(candidate_lines.begin(), candidate_lines.end(),
                    lines.begin() + static_cast<std::ptrdiff_t>(start_line));
}

std::vector<compare::MergeChoice> PreferredMergeChoices(const compare::MergeHunk& hunk) {
  std::vector<compare::MergeChoice> choices;
  choices.reserve(6);
  const auto push_unique = [&](compare::MergeChoice choice) {
    if (std::find(choices.begin(), choices.end(), choice) == choices.end()) {
      choices.push_back(choice);
    }
  };
  push_unique(hunk.choice);
  push_unique(compare::BootstrapMergeChoice(hunk));
  push_unique(compare::MergeChoice::Incoming);
  push_unique(compare::MergeChoice::Current);
  push_unique(compare::MergeChoice::Both);
  push_unique(compare::MergeChoice::Base);
  return choices;
}

bool LineStartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ContainsGitConflictMarkers(std::string_view text) {
  return text.find("<<<<<<<") != std::string_view::npos &&
         text.find("=======") != std::string_view::npos &&
         text.find(">>>>>>>") != std::string_view::npos;
}

struct ParsedGitConflictBlock {
  std::vector<std::string> current_lines;
  std::vector<std::string> base_lines;
  std::vector<std::string> incoming_lines;
  bool has_base = false;
};

struct SelectedGitConflictBlock {
  std::vector<std::string> lines;
  compare::MergeChoice choice = compare::MergeChoice::Base;
};

SelectedGitConflictBlock SelectGitConflictBlock(const compare::MergeHunk& hunk,
                                                const ParsedGitConflictBlock& block) {
  const bool current_matches = block.current_lines == hunk.current_lines;
  const bool incoming_matches = block.incoming_lines == hunk.incoming_lines;
  const bool base_matches = !block.has_base || block.base_lines == hunk.base_lines;

  if (!current_matches && !incoming_matches) {
    std::vector<std::string> lines = block.current_lines;
    lines.insert(lines.end(), block.incoming_lines.begin(), block.incoming_lines.end());
    return SelectedGitConflictBlock{.lines = std::move(lines), .choice = compare::MergeChoice::Both};
  }
  if (!current_matches) {
    return SelectedGitConflictBlock{.lines = block.current_lines,
                                    .choice = compare::MergeChoice::Current};
  }
  if (!incoming_matches) {
    return SelectedGitConflictBlock{.lines = block.incoming_lines,
                                    .choice = compare::MergeChoice::Incoming};
  }
  if (!base_matches) {
    return SelectedGitConflictBlock{.lines = block.base_lines,
                                    .choice = compare::MergeChoice::Base};
  }
  return SelectedGitConflictBlock{.lines = hunk.base_lines, .choice = compare::MergeChoice::Base};
}

struct ParsedGitConflictOutput {
  std::vector<std::string> result_lines;
  std::vector<std::vector<std::string>> conflict_lines;
  std::vector<compare::MergeChoice> conflict_choices;
};

struct ParsedGitConflictSegment {
  bool conflict = false;
  std::vector<std::string> plain_lines;
  ParsedGitConflictBlock block;
};

std::size_t CommonPrefixLength(std::span<const std::string> lhs, std::span<const std::string> rhs) {
  std::size_t prefix = 0;
  while (prefix < lhs.size() && prefix < rhs.size() && lhs[prefix] == rhs[prefix]) {
    ++prefix;
  }
  return prefix;
}

compare::MergeChoice InferMergeChoiceFromLines(const compare::MergeHunk& hunk,
                                               std::span<const std::string> lines) {
  const std::vector<std::string> both_lines = compare::MergeChoiceLines(hunk, compare::MergeChoice::Both);
  if (std::equal(lines.begin(), lines.end(), hunk.base_lines.begin(), hunk.base_lines.end())) {
    return compare::MergeChoice::Base;
  }
  if (std::equal(lines.begin(), lines.end(), hunk.incoming_lines.begin(), hunk.incoming_lines.end())) {
    return compare::MergeChoice::Incoming;
  }
  if (std::equal(lines.begin(), lines.end(), hunk.current_lines.begin(), hunk.current_lines.end())) {
    return compare::MergeChoice::Current;
  }
  if (std::equal(lines.begin(), lines.end(), both_lines.begin(), both_lines.end())) {
    return compare::MergeChoice::Both;
  }

  const std::size_t base_prefix = CommonPrefixLength(lines, hunk.base_lines);
  const std::size_t incoming_prefix = CommonPrefixLength(lines, hunk.incoming_lines);
  const std::size_t current_prefix = CommonPrefixLength(lines, hunk.current_lines);
  const std::size_t both_prefix = CommonPrefixLength(lines, both_lines);
  const std::size_t best_prefix = std::max({base_prefix, incoming_prefix, current_prefix, both_prefix});
  if (best_prefix == 0) {
    return hunk.choice;
  }
  if (current_prefix == best_prefix) {
    return compare::MergeChoice::Current;
  }
  if (incoming_prefix == best_prefix) {
    return compare::MergeChoice::Incoming;
  }
  if (both_prefix == best_prefix) {
    return compare::MergeChoice::Both;
  }
  return compare::MergeChoice::Base;
}

std::optional<std::size_t> FindSequence(std::span<const std::string> haystack,
                                        std::size_t start,
                                        std::span<const std::string> needle) {
  if (needle.empty()) {
    return start;
  }
  if (haystack.size() < needle.size() || start > haystack.size() - needle.size()) {
    return std::nullopt;
  }
  for (std::size_t i = start; i + needle.size() <= haystack.size(); ++i) {
    if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(i))) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<ParsedGitConflictOutput> ParseGitConflictOutput(const compare::MergeModel& model,
                                                              std::string_view text) {
  bool saw_conflict_block = false;

  const std::vector<std::string> lines = SplitSyntaxLines(text);
  ParsedGitConflictOutput parsed;
  std::vector<ParsedGitConflictSegment> segments;
  std::size_t line_index = 0;
  while (line_index < lines.size()) {
    if (!LineStartsWith(lines[line_index], "<<<<<<<")) {
      ParsedGitConflictSegment segment;
      segment.plain_lines.push_back(lines[line_index]);
      ++line_index;
      while (line_index < lines.size() && !LineStartsWith(lines[line_index], "<<<<<<<")) {
        segment.plain_lines.push_back(lines[line_index]);
        ++line_index;
      }
      segments.push_back(std::move(segment));
      continue;
    }
    saw_conflict_block = true;

    ParsedGitConflictSegment segment;
    segment.conflict = true;

    ParsedGitConflictBlock block;
    ++line_index;
    while (line_index < lines.size() && !LineStartsWith(lines[line_index], "|||||||") &&
           lines[line_index] != "=======") {
      block.current_lines.push_back(lines[line_index]);
      ++line_index;
    }
    if (line_index < lines.size() && LineStartsWith(lines[line_index], "|||||||")) {
      block.has_base = true;
      ++line_index;
      while (line_index < lines.size() && lines[line_index] != "=======") {
        block.base_lines.push_back(lines[line_index]);
        ++line_index;
      }
    }
    if (line_index >= lines.size() || lines[line_index] != "=======") {
      return std::nullopt;
    }
    ++line_index;
    while (line_index < lines.size() && !LineStartsWith(lines[line_index], ">>>>>>>")) {
      block.incoming_lines.push_back(lines[line_index]);
      ++line_index;
    }
    if (line_index >= lines.size() || !LineStartsWith(lines[line_index], ">>>>>>>")) {
      return std::nullopt;
    }
    ++line_index;

    segment.block = std::move(block);
    segments.push_back(std::move(segment));
  }

  if (!saw_conflict_block) {
    return std::nullopt;
  }

  std::vector<const compare::MergeHunk*> conflict_hunks;
  conflict_hunks.reserve(model.hunks.size());
  for (const auto& hunk : model.hunks) {
    if (hunk.conflict) {
      conflict_hunks.push_back(&hunk);
    }
  }

  const std::size_t conflict_segment_count = std::count_if(
      segments.begin(), segments.end(), [](const auto& segment) { return segment.conflict; });
  if (conflict_segment_count == conflict_hunks.size()) {
    std::size_t conflict_segment_index = 0;
    for (const auto& segment : segments) {
      if (!segment.conflict) {
        parsed.result_lines.insert(parsed.result_lines.end(), segment.plain_lines.begin(),
                                   segment.plain_lines.end());
        continue;
      }
      const SelectedGitConflictBlock selected =
          SelectGitConflictBlock(*conflict_hunks[conflict_segment_index], segment.block);
      parsed.result_lines.insert(parsed.result_lines.end(), selected.lines.begin(), selected.lines.end());
      parsed.conflict_lines.push_back(selected.lines);
      parsed.conflict_choices.push_back(selected.choice);
      ++conflict_segment_index;
    }
  } else {
    for (const auto& segment : segments) {
      if (!segment.conflict) {
        parsed.result_lines.insert(parsed.result_lines.end(), segment.plain_lines.begin(),
                                   segment.plain_lines.end());
        continue;
      }
      parsed.result_lines.insert(parsed.result_lines.end(), segment.block.current_lines.begin(),
                                 segment.block.current_lines.end());
    }
  }

  if (parsed.result_lines.empty()) {
    parsed.result_lines.push_back("");
  }
  return parsed;
}

}  // namespace

std::vector<WorkspaceShell::MergeTrackedConflict> WorkspaceShell::BuildMergeTrackedConflicts(
    const compare::MergeModel& model) const {
  std::vector<MergeTrackedConflict> conflicts;
  std::size_t incoming_line = 0;
  std::size_t current_line = 0;
  std::size_t result_line = 0;
  int base_cursor = 0;
  for (const auto& hunk : model.hunks) {
    const std::size_t unchanged_lines =
        static_cast<std::size_t>(std::max(0, hunk.base_start - base_cursor));
    incoming_line += unchanged_lines;
    current_line += unchanged_lines;
    result_line += unchanged_lines;

    const std::vector<std::string> result_lines = compare::MergeChoiceLines(hunk, hunk.choice);
    if (hunk.conflict) {
      conflicts.push_back(MergeTrackedConflict{
          .hunk_index = static_cast<std::size_t>(hunk.index),
          .incoming_start_line = incoming_line,
          .incoming_end_line = incoming_line + hunk.incoming_lines.size(),
          .current_start_line = current_line,
          .current_end_line = current_line + hunk.current_lines.size(),
          .start_line = result_line,
          .end_line = result_line + result_lines.size(),
          .last_choice = hunk.choice,
          .valid = true,
      });
    }

    incoming_line += hunk.incoming_lines.size();
    current_line += hunk.current_lines.size();
    result_line += result_lines.size();
    base_cursor = hunk.base_end;
  }
  return conflicts;
}

std::vector<WorkspaceShell::MergeTrackedConflict> WorkspaceShell::BuildMergeTrackedConflictsForResult(
    compare::MergeModel& model,
    const std::vector<std::string>& result_lines,
    std::span<const std::vector<std::string>> conflict_line_hints,
    std::span<const compare::MergeChoice> choice_hints) const {
  std::vector<MergeTrackedConflict> conflicts;
  std::size_t incoming_line = 0;
  std::size_t current_line = 0;
  std::size_t result_line = 0;
  int base_cursor = 0;
  std::size_t conflict_index = 0;
  for (std::size_t hunk_index = 0; hunk_index < model.hunks.size(); ++hunk_index) {
    auto& hunk = model.hunks[hunk_index];
    const std::size_t unchanged_lines =
        static_cast<std::size_t>(std::max(0, hunk.base_start - base_cursor));
    incoming_line += unchanged_lines;
    current_line += unchanged_lines;
    result_line += unchanged_lines;

    if (hunk.conflict) {
      const std::size_t next_base_start =
          hunk_index + 1 < model.hunks.size()
              ? static_cast<std::size_t>(std::max(0, model.hunks[hunk_index + 1].base_start))
              : model.base_lines.size();
      const std::size_t post_context_start =
          static_cast<std::size_t>(std::max(0, hunk.base_end));
      const std::span<const std::string> post_context =
          post_context_start <= model.base_lines.size() && post_context_start <= next_base_start
              ? std::span<const std::string>(
                    model.base_lines.data() + static_cast<std::ptrdiff_t>(post_context_start),
                    next_base_start - post_context_start)
              : std::span<const std::string>{};

      std::vector<std::string> committed_lines;
      bool valid = false;
      if (conflict_index < conflict_line_hints.size() && conflict_index < choice_hints.size()) {
        hunk.choice = choice_hints[conflict_index];
        committed_lines = conflict_line_hints[conflict_index];
        valid = true;
      } else {
        committed_lines = compare::MergeChoiceLines(hunk, hunk.choice);
        for (const compare::MergeChoice choice : PreferredMergeChoices(hunk)) {
          const std::vector<std::string> candidate_lines = compare::MergeChoiceLines(hunk, choice);
          if (!MatchesLineSegment(result_lines, result_line, candidate_lines)) {
            continue;
          }
          if (!post_context.empty()) {
            const std::vector<std::string> post_context_lines(post_context.begin(), post_context.end());
            const bool immediate_context_matches =
                MatchesLineSegment(result_lines, result_line + candidate_lines.size(), post_context_lines);
            const bool later_context_exists =
                FindSequence(result_lines, result_line + candidate_lines.size(), post_context).has_value();
            if (!immediate_context_matches && later_context_exists) {
              continue;
            }
          }
          hunk.choice = choice;
          committed_lines = candidate_lines;
          valid = true;
          break;
        }
        if (!valid) {
          if (const auto next_context = FindSequence(result_lines, result_line, post_context);
              next_context.has_value() && *next_context >= result_line) {
            committed_lines = std::vector<std::string>(
                result_lines.begin() + static_cast<std::ptrdiff_t>(result_line),
                result_lines.begin() + static_cast<std::ptrdiff_t>(*next_context));
            hunk.choice = InferMergeChoiceFromLines(hunk, committed_lines);
            valid = true;
          }
        }
      }

      conflicts.push_back(MergeTrackedConflict{
          .hunk_index = static_cast<std::size_t>(hunk.index),
          .incoming_start_line = incoming_line,
          .incoming_end_line = incoming_line + hunk.incoming_lines.size(),
          .current_start_line = current_line,
          .current_end_line = current_line + hunk.current_lines.size(),
          .start_line = result_line,
          .end_line = result_line + committed_lines.size(),
          .last_choice = hunk.choice,
          .valid = valid,
      });
      result_line += committed_lines.size();
      ++conflict_index;
    } else {
      result_line += compare::MergeChoiceLines(hunk, hunk.choice).size();
    }

    incoming_line += hunk.incoming_lines.size();
    current_line += hunk.current_lines.size();
    base_cursor = hunk.base_end;
  }
  return conflicts;
}

void WorkspaceShell::UpdateMergeMaxVisualColumns(
    MergeTabState& merge_tab,
    std::span<const std::string> result_lines) const {
  std::vector<std::string> owned_lines;
  owned_lines.reserve(result_lines.size());
  for (std::string_view line : result_lines) {
    owned_lines.emplace_back(line);
  }
  merge_tab.max_visual_columns =
      std::max(merge_tab.max_visual_columns, MaxVisualColumnsForLines(owned_lines));
}

void WorkspaceShell::OpenComparePicker() {
  if (!surface_.sidebar_visible || surface_.sidebar_mode != SidebarMode::Tree) {
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
    return false;
  }
  if (path.empty()) {
    return false;
  }

  overlay_workflow_.compare_picker.path = path.lexically_normal();
  overlay_workflow_.compare_picker.query.clear();
  overlay_workflow_.compare_picker.commits = project::CollectGitFileHistory(project_root_, overlay_workflow_.compare_picker.path);
  RefreshComparePicker();
  if (overlay_workflow_.compare_picker.matches.empty()) {
    return false;
  }

  if (!commit_spec.empty()) {
    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < overlay_workflow_.compare_picker.matches.size(); ++i) {
      const auto& commit = overlay_workflow_.compare_picker.matches[i];
      const std::string lowered_hash = ToLower(commit.hash);
      const std::string lowered_short_hash = ToLower(commit.short_hash);
      if (StartsWith(lowered_hash, lowered_commit_spec) ||
          StartsWith(lowered_short_hash, lowered_commit_spec)) {
        matching_indices.push_back(i);
      }
    }

    if (matching_indices.empty()) {
      return false;
    }
    if (matching_indices.size() > 1) {
      return false;
    }

    overlay_workflow_.compare_picker.selected_index = matching_indices.front();
    OpenSelectedCompareCommit();
    return true;
  }

  ShowOverlay(OverlayMode::CommitPicker);
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
    compare_tab->compare->left_path = normalized_path;
    compare_tab->compare->right_path = normalized_path;
    compare_tab->compare->commit_hash = commit.hash;
    compare_tab->compare->right_ref = "WORKTREE";
    compare_tab->compare->right_editable = true;
    compare_tab->compare->right_view_active = true;
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
      project::ReadGitFileAtCommit(project_root_, left_source_path, compare_tab.commit_hash);
  if (!left_content.has_value()) {
    return std::nullopt;
  }

  std::string right_content;
  if (compare_tab.right_ref == "WORKTREE") {
    right_content = compare_tab.right_viewport.dirty()
                        ? SerializeLines(compare_tab.right_viewport.lines(),
                                         compare_tab.right_viewport.line_ending())
                        : ReadFileText(right_source_path).value_or("");
  } else {
    const auto right_commit_content =
        project::ReadGitFileAtCommit(project_root_, right_source_path, compare_tab.right_ref);
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
  rebuilt->compare->right_editable = compare_tab.right_ref == "WORKTREE";
  rebuilt->compare->right_view_active =
      rebuilt->compare->right_editable && (compare_tab.right_editable ? compare_tab.right_view_active : true);
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

  return TabEntry{
      .kind = TabEntry::Kind::Compare,
      .path = normalized_path,
      .title = compare_tab.title,
      .editor_state = std::nullopt,
      .compare = std::move(compare_tab),
      .merge = std::nullopt,
  };
}

void WorkspaceShell::RefreshCompareTabDerivedState(CompareTabState& compare_tab) const {
  const std::string right_content =
      SerializeLines(compare_tab.right_viewport.lines(), compare_tab.right_viewport.line_ending());
  compare_tab.model = compare::BuildCompareModel(compare_tab.left_content, right_content);
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
  if (compare_tab.model.rows.empty()) {
    compare_tab.selected_row = 0;
    compare_tab.scroll_row = 0;
    return;
  }
  compare_tab.selected_row = std::min(compare_tab.selected_row, compare_tab.model.rows.size() - 1);
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
  if (!compare_tab.right_editable) {
    return;
  }

  compare_tab.right_viewport.SetHorizontalScroll(compare_tab.horizontal_scroll);
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));
  compare_tab.right_viewport.SetScrollLine(CompareRightLineForRow(compare_tab, scroll_row));
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
}

void WorkspaceShell::SyncCompareSelectionFromViewport(CompareTabState& compare_tab,
                                                      bool reveal_selection) const {
  if (!compare_tab.right_editable || compare_tab.model.rows.empty()) {
    return;
  }

  compare_tab.selected_row = CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.cursor_line());
  compare_tab.horizontal_scroll = compare_tab.right_viewport.horizontal_scroll();
  if (reveal_selection) {
    if (last_window_width_ > 0 && last_window_height_ > 0) {
      const WorkspaceLayout layout =
          ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                        surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width,
                        surface_.bottom_panel_height);
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
    compare_tab.scroll_row = static_cast<int>(
        CompareRowIndexForRightLine(compare_tab, compare_tab.right_viewport.scroll_line()));
    SyncCompareViewportScroll(compare_tab);
  }
}

void WorkspaceShell::RefreshMergeTabDerivedState(MergeTabState& merge_tab) const {
  const std::string result_text = compare::MergeResultText(merge_tab.model);
  merge_tab.result_viewport.LoadContent(result_text, merge_tab.output_path, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetPath(merge_tab.output_path);
  merge_tab.result_viewport.SetDirty(
      !merge_tab.persisted_output_baseline.has_value() ||
      *merge_tab.persisted_output_baseline !=
          SerializeLines(merge_tab.result_viewport.lines(), merge_tab.result_line_ending));
  merge_tab.conflicts = BuildMergeTrackedConflicts(merge_tab.model);
  merge_tab.hover_state.reset();
  merge_tab.max_visual_columns =
      std::max({MaxVisualColumnsForLines(merge_tab.model.incoming_lines),
                MaxVisualColumnsForLines(merge_tab.model.current_lines),
                merge_tab.result_viewport.max_visual_columns()});
  merge_tab.result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab.scroll_row)));
  merge_tab.result_viewport.SetHorizontalScroll(merge_tab.horizontal_scroll);
  merge_tab.scroll_row = static_cast<int>(merge_tab.result_viewport.scroll_line());
  merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
  if (merge_tab.conflicts.empty()) {
    merge_tab.selected_hunk = 0;
    merge_tab.scroll_row = 0;
    return;
  }
  merge_tab.selected_hunk = std::min(merge_tab.selected_hunk, merge_tab.conflicts.size() - 1);
}

void WorkspaceShell::PopulateMergeSyntaxTokensForWindow(MergeTabState& merge_tab,
                                                        std::size_t visible_start_row,
                                                        std::size_t visible_end_row) {
  const std::size_t clamped_incoming_end =
      std::min(visible_end_row, merge_tab.model.incoming_lines.size());
  if (visible_start_row < clamped_incoming_end) {
    constexpr std::size_t kMergeSyntaxRowsPerFrame = 256;
    const std::size_t target_row =
        std::min(clamped_incoming_end, merge_tab.incoming_syntax_rows_tokenized + kMergeSyntaxRowsPerFrame);
    while (merge_tab.incoming_syntax_rows_tokenized < target_row) {
      const std::size_t index = merge_tab.incoming_syntax_rows_tokenized;
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          merge_tab.model.incoming_lines[index], merge_tab.output_path,
          merge_tab.incoming_current_syntax_state);
      merge_tab.incoming_current_syntax_state = highlighted.end_state;
      merge_tab.incoming_tokens[index] = std::move(highlighted.tokens);
      ++merge_tab.incoming_syntax_rows_tokenized;
    }
  }

  const std::size_t clamped_current_end =
      std::min(visible_end_row, merge_tab.model.current_lines.size());
  if (visible_start_row < clamped_current_end) {
    constexpr std::size_t kMergeSyntaxRowsPerFrame = 256;
    const std::size_t target_row =
        std::min(clamped_current_end, merge_tab.current_syntax_rows_tokenized + kMergeSyntaxRowsPerFrame);
    while (merge_tab.current_syntax_rows_tokenized < target_row) {
      const std::size_t index = merge_tab.current_syntax_rows_tokenized;
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          merge_tab.model.current_lines[index], merge_tab.output_path,
          merge_tab.current_current_syntax_state);
      merge_tab.current_current_syntax_state = highlighted.end_state;
      merge_tab.current_tokens[index] = std::move(highlighted.tokens);
      ++merge_tab.current_syntax_rows_tokenized;
    }
  }
}

void WorkspaceShell::UpdateMergeTrackingAfterViewportEdit(
    MergeTabState& merge_tab,
    const std::vector<std::string>& before_lines,
    std::optional<editor::SelectionRange> selection_before,
    editor::TextPosition cursor_before) {
  const auto changed_span = ComputeChangedLineSpan(before_lines, merge_tab.result_viewport.lines());
  if (!changed_span.has_value()) {
    merge_tab.scroll_row = static_cast<int>(merge_tab.result_viewport.scroll_line());
    merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
    return;
  }

  const auto& change = *changed_span;
  const long long line_delta =
      static_cast<long long>(change.new_end) - static_cast<long long>(change.old_end);
  const bool pure_insertion = !selection_before.has_value() && change.old_start == change.old_end &&
                              merge_tab.result_viewport.lines().size() >= before_lines.size();
  const std::size_t insertion_anchor_line =
      selection_before.has_value() ? selection_before->start.line : cursor_before.line;

  for (auto& conflict : merge_tab.conflicts) {
    if (pure_insertion) {
      if (conflict.end_line <= insertion_anchor_line) {
        continue;
      }
      if (conflict.start_line > insertion_anchor_line) {
        conflict.start_line = static_cast<std::size_t>(
            static_cast<long long>(conflict.start_line) + line_delta);
        conflict.end_line = static_cast<std::size_t>(
            static_cast<long long>(conflict.end_line) + line_delta);
        continue;
      }
      conflict.valid = false;
      continue;
    }

    if (conflict.end_line <= change.old_start) {
      continue;
    }
    if (conflict.start_line >= change.old_end) {
      conflict.start_line = static_cast<std::size_t>(
          static_cast<long long>(conflict.start_line) + line_delta);
      conflict.end_line = static_cast<std::size_t>(
          static_cast<long long>(conflict.end_line) + line_delta);
      continue;
    }
    conflict.valid = false;
  }

  const std::size_t changed_start = std::min(change.old_start, merge_tab.result_viewport.lines().size());
  const std::size_t changed_end = std::min(change.new_end, merge_tab.result_viewport.lines().size());
  if (changed_start < changed_end) {
    UpdateMergeMaxVisualColumns(
        merge_tab, std::vector<std::string>(merge_tab.result_viewport.lines().begin() +
                                                static_cast<std::ptrdiff_t>(changed_start),
                                            merge_tab.result_viewport.lines().begin() +
                                                static_cast<std::ptrdiff_t>(changed_end)));
  }

  merge_tab.hover_state.reset();
  merge_tab.scroll_row = static_cast<int>(merge_tab.result_viewport.scroll_line());
  merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
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
  merge_tab.incoming_tokens.resize(merge_tab.model.incoming_lines.size());
  merge_tab.current_tokens.resize(merge_tab.model.current_lines.size());
  merge_tab.incoming_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(normalized_output, merge_tab.model.incoming_lines);
  merge_tab.current_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(normalized_output, merge_tab.model.current_lines);
  merge_tab.incoming_current_syntax_state = merge_tab.incoming_initial_syntax_state;
  merge_tab.current_current_syntax_state = merge_tab.current_initial_syntax_state;
  merge_tab.incoming_syntax_rows_tokenized = 0;
  merge_tab.current_syntax_rows_tokenized = 0;
  merge_tab.persisted_output_baseline = output_text;
  merge_tab.selected_hunk = selected_hunk;
  merge_tab.scroll_row = 0;
  merge_tab.result_viewport.SetPath(merge_tab.output_path);
  if (output_text.has_value()) {
    std::optional<ParsedGitConflictOutput> parsed_output;
    if (ContainsGitConflictMarkers(*output_text)) {
      parsed_output = ParseGitConflictOutput(merge_tab.model, *output_text);
    }
    const std::string rendered_text =
        parsed_output.has_value()
            ? SerializeLines(parsed_output->result_lines, merge_tab.result_line_ending)
            : *output_text;
    merge_tab.result_viewport.LoadContent(rendered_text, merge_tab.output_path,
                                          merge_tab.result_line_ending);
    merge_tab.result_viewport.SetPath(merge_tab.output_path);
    merge_tab.result_viewport.SetDirty(false);
    if (parsed_output.has_value()) {
      merge_tab.conflicts = BuildMergeTrackedConflictsForResult(
          merge_tab.model, merge_tab.result_viewport.lines(), parsed_output->conflict_lines,
          parsed_output->conflict_choices);
    } else {
      merge_tab.conflicts =
          BuildMergeTrackedConflictsForResult(merge_tab.model, merge_tab.result_viewport.lines());
    }
    merge_tab.hover_state.reset();
    merge_tab.max_visual_columns =
        std::max({MaxVisualColumnsForLines(merge_tab.model.incoming_lines),
                  MaxVisualColumnsForLines(merge_tab.model.current_lines),
                  merge_tab.result_viewport.max_visual_columns()});
    merge_tab.result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab.scroll_row)));
    merge_tab.result_viewport.SetHorizontalScroll(merge_tab.horizontal_scroll);
    merge_tab.scroll_row = static_cast<int>(merge_tab.result_viewport.scroll_line());
    merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
    if (merge_tab.conflicts.empty()) {
      merge_tab.selected_hunk = 0;
      merge_tab.scroll_row = 0;
    } else {
      merge_tab.selected_hunk = std::min(merge_tab.selected_hunk, merge_tab.conflicts.size() - 1);
    }
  } else {
    RefreshMergeTabDerivedState(merge_tab);
  }
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
  overlay_workflow_.compare_picker.matches.clear();
  overlay_workflow_.compare_picker.selected_index = 0;

  const std::string lowered_query = ToLower(overlay_workflow_.compare_picker.query);
  for (const auto& commit : overlay_workflow_.compare_picker.commits) {
    if (!lowered_query.empty()) {
      const std::string text = ToLower(commit.short_hash + " " + commit.subject);
      if (text.find(lowered_query) == std::string::npos) {
        continue;
      }
    }
    overlay_workflow_.compare_picker.matches.push_back(commit);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  if (overlay_workflow_.compare_picker.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(overlay_workflow_.compare_picker.selected_index);
  const int max_index = static_cast<int>(overlay_workflow_.compare_picker.matches.size()) - 1;
  overlay_workflow_.compare_picker.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (surface_.overlay_visible && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  if (overlay_workflow_.compare_picker.matches.empty() ||
      overlay_workflow_.compare_picker.selected_index >= overlay_workflow_.compare_picker.matches.size()) {
    return;
  }

  OpenComparison(overlay_workflow_.compare_picker.matches[overlay_workflow_.compare_picker.selected_index]);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  if (const auto existing_index =
          FindOpenCompareTabIndex(overlay_workflow_.compare_picker.path, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    SyncActiveEditorTab();
    if (open_tabs_[*existing_index].compare.has_value()) {
      auto rebuilt =
          BuildCompareTabEntry(overlay_workflow_.compare_picker.path, open_tabs_[*existing_index].compare.value());
      if (rebuilt.has_value() && rebuilt->compare.has_value()) {
        open_tabs_[*existing_index] = std::move(*rebuilt);
      }
    }
    active_tab_index_ = *existing_index;
    RevealActiveCompareSelection();
    EnsureActiveTabVisible();
    DismissOverlay(true);
    return;
  }
  auto compare_tab = BuildCompareTabEntry(overlay_workflow_.compare_picker.path, commit);
  if (!compare_tab.has_value()) {
    return;
  }

  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*compare_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveCompareSelection();
  EnsureActiveTabVisible();
  DismissOverlay(true);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  const std::filesystem::path normalized_base = base_path.lexically_normal();
  const std::filesystem::path normalized_incoming = incoming_path.lexically_normal();
  const std::filesystem::path normalized_current = current_path.lexically_normal();
  const std::filesystem::path normalized_output = output_path.lexically_normal();

  if (const auto existing_index = FindOpenMergeTabIndex(normalized_output); existing_index.has_value()) {
    SyncActiveEditorTab();
    if (open_tabs_[*existing_index].merge.has_value() &&
        !open_tabs_[*existing_index].merge->result_viewport.dirty()) {
      auto rebuilt =
          BuildMergeTabEntry(normalized_base, normalized_incoming, normalized_current, normalized_output);
      if (rebuilt.has_value() && rebuilt->merge.has_value()) {
        const auto& previous_merge = open_tabs_[*existing_index].merge.value();
        auto& rebuilt_merge = rebuilt->merge.value();
        rebuilt_merge.selected_hunk =
            rebuilt_merge.conflicts.empty()
                ? 0
                : std::min(previous_merge.selected_hunk, rebuilt_merge.conflicts.size() - 1);
        rebuilt_merge.scroll_row = previous_merge.scroll_row;
        rebuilt_merge.horizontal_scroll = previous_merge.horizontal_scroll;
        rebuilt_merge.left_divider_fraction = previous_merge.left_divider_fraction;
        rebuilt_merge.right_divider_fraction = previous_merge.right_divider_fraction;
        rebuilt_merge.persistable = previous_merge.persistable;
        rebuilt_merge.result_viewport.SetViewportSize(previous_merge.result_viewport.visible_lines(),
                                                      previous_merge.result_viewport.visible_columns());
        rebuilt_merge.result_viewport.SetScrollLine(
            static_cast<std::size_t>(std::max(0, rebuilt_merge.scroll_row)));
        rebuilt_merge.result_viewport.SetHorizontalScroll(rebuilt_merge.horizontal_scroll);
        rebuilt_merge.scroll_row = static_cast<int>(rebuilt_merge.result_viewport.scroll_line());
        rebuilt_merge.horizontal_scroll = rebuilt_merge.result_viewport.horizontal_scroll();
        open_tabs_[*existing_index] = std::move(*rebuilt);
      }
    }
    active_tab_index_ = *existing_index;
    RevealActiveMergeSelection();
    EnsureActiveTabVisible();
    surface_.focus = FocusTarget::Editor;
    return true;
  }
  auto merge_tab =
      BuildMergeTabEntry(normalized_base, normalized_incoming, normalized_current, normalized_output);
  if (!merge_tab.has_value()) {
    return false;
  }

  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*merge_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveMergeSelection();
  EnsureActiveTabVisible();
  surface_.focus = FocusTarget::Editor;
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
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row =
      std::clamp(scroll_layout.vertical_scroll + delta, 0, scroll_layout.max_vertical_scroll);
  SyncCompareViewportScroll(*compare_tab);
}

void WorkspaceShell::ScrollCompareColumns(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0 || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  const long long target_scroll =
      static_cast<long long>(scroll_layout.horizontal_scroll) + static_cast<long long>(delta);
  compare_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL,
                 static_cast<long long>(scroll_layout.max_horizontal_scroll)));
  SyncCompareViewportScroll(*compare_tab);
}

void WorkspaceShell::MoveMergeSelection(int delta) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(merge_tab->selected_hunk);
  const int max_index = static_cast<int>(merge_tab->conflicts.size()) - 1;
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
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
  const long long target_scroll =
      static_cast<long long>(scroll_layout.horizontal_scroll) + static_cast<long long>(delta);
  merge_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL,
                 static_cast<long long>(scroll_layout.max_horizontal_scroll)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }

  const std::size_t selected_hunk = std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);
  auto& conflict = merge_tab->conflicts[selected_hunk];
  if (!conflict.valid || conflict.hunk_index >= merge_tab->model.hunks.size()) {
    return;
  }

  const std::vector<std::string> replacement_lines =
      compare::MergeChoiceLines(merge_tab->model.hunks[conflict.hunk_index], choice);
  const std::size_t previous_end = conflict.end_line;
  if (!merge_tab->result_viewport.ReplaceLines(conflict.start_line, previous_end, replacement_lines)) {
    return;
  }

  merge_tab->model.hunks[conflict.hunk_index].choice = choice;
  conflict.end_line = conflict.start_line + replacement_lines.size();
  conflict.last_choice = choice;
  conflict.valid = true;
  const long long line_delta =
      static_cast<long long>(conflict.end_line) - static_cast<long long>(previous_end);
  for (std::size_t i = selected_hunk + 1; i < merge_tab->conflicts.size(); ++i) {
    merge_tab->conflicts[i].start_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].start_line) + line_delta);
    merge_tab->conflicts[i].end_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].end_line) + line_delta);
  }
  UpdateMergeMaxVisualColumns(*merge_tab, replacement_lines);
  merge_tab->hover_state.reset();
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  RevealActiveMergeSelection();
}

}  // namespace microide::workspace
