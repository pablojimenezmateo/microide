#include "workspace/git/MergeResultValidation.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

#include "compare/MergeConflictKind.h"
#include "editor/SyntaxHighlighter.h"
#include "util/GitConflictMarkers.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResolverContext.h"
#include "workspace/state/SurfaceTokenWindow.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

template <typename LinesRange>
std::size_t MaxVisualColumnsForLines(const LinesRange& lines) {
  std::size_t max_columns = 0;
  for (std::string_view line : lines) {
    max_columns = std::max(max_columns, util::Utf8CodepointCount(line));
  }
  return max_columns;
}

template <typename LhsRange, typename RhsRange>
bool LinesEqual(const LhsRange& lhs, const RhsRange& rhs) {
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
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
  bool resolved = true;
};

SelectedGitConflictBlock SelectGitConflictBlock(const compare::MergeHunk& hunk,
                                                const ParsedGitConflictBlock& block) {
  const bool current_matches = LinesEqual(block.current_lines, hunk.current_lines);
  const bool incoming_matches = LinesEqual(block.incoming_lines, hunk.incoming_lines);

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
  return SelectedGitConflictBlock{.lines = compare::MergeChoiceLines(hunk, hunk.choice),
                                  .choice = hunk.choice,
                                  .resolved = false};
}

struct ParsedGitConflictOutput {
  std::vector<std::string> result_lines;
  std::vector<std::vector<std::string>> conflict_lines;
  std::vector<compare::MergeChoice> conflict_choices;
  std::vector<std::uint8_t> conflict_resolved;
};

struct ParsedGitConflictSegment {
  bool conflict = false;
  std::vector<std::string> plain_lines;
  ParsedGitConflictBlock block;
};

std::optional<ParsedGitConflictOutput> ParseGitConflictOutput(const compare::MergeModel& model,
                                                              std::string_view text) {
  bool saw_conflict_block = false;

  const std::vector<std::string_view> lines = util::SplitOnByte(text, '\n');
  ParsedGitConflictOutput parsed;
  std::vector<ParsedGitConflictSegment> segments;
  std::size_t line_index = 0;
  using util::IsConflictBaseLine;
  using util::IsConflictEndLine;
  using util::IsConflictSeparatorLine;
  using util::IsConflictStartLine;
  while (line_index < lines.size()) {
    if (!IsConflictStartLine(lines[line_index])) {
      ParsedGitConflictSegment segment;
      segment.plain_lines.emplace_back(lines[line_index]);
      ++line_index;
      while (line_index < lines.size() && !IsConflictStartLine(lines[line_index])) {
        segment.plain_lines.emplace_back(lines[line_index]);
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
    while (line_index < lines.size() && !IsConflictBaseLine(lines[line_index]) &&
           !IsConflictSeparatorLine(lines[line_index])) {
      block.current_lines.emplace_back(lines[line_index]);
      ++line_index;
    }
    if (line_index < lines.size() && IsConflictBaseLine(lines[line_index])) {
      block.has_base = true;
      ++line_index;
      while (line_index < lines.size() && !IsConflictSeparatorLine(lines[line_index])) {
        block.base_lines.emplace_back(lines[line_index]);
        ++line_index;
      }
    }
    if (line_index >= lines.size() || !IsConflictSeparatorLine(lines[line_index])) {
      return std::nullopt;
    }
    ++line_index;
    while (line_index < lines.size() && !IsConflictEndLine(lines[line_index])) {
      block.incoming_lines.emplace_back(lines[line_index]);
      ++line_index;
    }
    if (line_index >= lines.size() || !IsConflictEndLine(lines[line_index])) {
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
      parsed.result_lines.insert(parsed.result_lines.end(), selected.lines.begin(),
                                 selected.lines.end());
      parsed.conflict_lines.push_back(selected.lines);
      parsed.conflict_choices.push_back(selected.choice);
      parsed.conflict_resolved.push_back(selected.resolved ? 1 : 0);
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

void ResyncMergeTabViewState(MergeTabState& merge_tab) {
  merge_tab.hover_state.reset();
  merge_tab.max_visual_columns =
      std::max({MaxVisualColumnsForLines(merge_tab.model.incoming_lines),
                MaxVisualColumnsForLines(merge_tab.model.current_lines),
                merge_tab.result_viewport.max_visual_columns()});
  merge_tab.result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, merge_tab.scroll_row)));
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

}

void WorkspaceShell::UpdateMergeMaxVisualColumns(
    MergeTabState& merge_tab,
    std::span<const std::string> result_lines) const {
  merge_tab.max_visual_columns =
      std::max(merge_tab.max_visual_columns, MaxVisualColumnsForLines(result_lines));
}

void WorkspaceShell::RefreshMergeTabDerivedState(MergeTabState& merge_tab) const {
  ++merge_tab.model_revision;
  merge_tab.scrollbar_marker_cache_valid = false;
  merge_tab.scrollbar_marker_cache.clear();
  const std::string result_text = compare::MergeResultText(merge_tab.model);
  merge_tab.result_viewport.LoadContent(result_text, merge_tab.output_path, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetPath(merge_tab.output_path);
  merge_tab.result_viewport.SetDirty(
      !merge_tab.persisted_output_baseline.has_value() ||
      *merge_tab.persisted_output_baseline !=
          merge_tab.result_viewport.SerializeDocumentText(merge_tab.result_line_ending));
  merge_tab.conflicts = BuildMergeTrackedConflicts(merge_tab.model);
  ResyncMergeTabViewState(merge_tab);
}

void WorkspaceShell::PopulateMergeSyntaxTokensForWindow(MergeTabState& merge_tab,
                                                        const SDL_FRect& rect) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(rect, merge_tab);
  ClampMergeScrollRow(merge_tab, surface.visible_rows);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, merge_tab.scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  constexpr std::size_t kMergeSyntaxRowsPerFrame = 256;
  const SurfaceTokenWindow::WalkBudget walk_budget{
      kMergeSyntaxRowsPerFrame, std::chrono::steady_clock::now() + kSyntaxWalkTimePerFrame};
  merge_tab.incoming_token_window.EnsureWindow(
      visible_start_row, visible_end_row, merge_tab.output_path, walk_budget,
      [&](std::size_t row) -> std::string_view { return merge_tab.model.incoming_lines[row]; });
  merge_tab.current_token_window.EnsureWindow(
      visible_start_row, visible_end_row, merge_tab.output_path, walk_budget,
      [&](std::size_t row) -> std::string_view { return merge_tab.model.current_lines[row]; });
  const auto behind = [&](const SurfaceTokenWindow& window) {
    return window.frontier() < std::min(visible_end_row, window.line_count());
  };
  if (behind(merge_tab.incoming_token_window) || behind(merge_tab.current_token_window)) {
    frame_state_.syntax_catch_up_owed = true;
  }
}

void WorkspaceShell::UpdateMergeTrackingAfterViewportEdit(MergeTabState& merge_tab) {
  const std::optional<editor::AppliedEditLineSpan>& applied_span =
      merge_tab.result_viewport.last_applied_edit_line_span();
  if (!applied_span.has_value()) {
    merge_tab.scroll_row = static_cast<int>(merge_tab.result_viewport.scroll_line());
    merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
    return;
  }

  const editor::AppliedEditLineSpan& change = *applied_span;
  const long long line_delta =
      static_cast<long long>(change.new_end) - static_cast<long long>(change.old_end);

  for (auto& conflict : merge_tab.conflicts) {
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

  const std::size_t line_count = merge_tab.result_viewport.lines().size();
  const std::size_t changed_start = std::min(change.old_start, line_count);
  const std::size_t changed_end = std::min(change.new_end, line_count);
  if (changed_start < changed_end) {
    std::vector<std::string> changed_rows;
    changed_rows.reserve(changed_end - changed_start);
    for (std::size_t ln = changed_start; ln < changed_end; ++ln) {
      changed_rows.emplace_back(merge_tab.result_viewport.lines().LineView(ln));
    }
    UpdateMergeMaxVisualColumns(merge_tab, changed_rows);
  }

  merge_tab.hover_state.reset();
  merge_tab.scrollbar_marker_cache_valid = false;
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

  const util::TextFileReadResult base_read = util::ReadTextFileClassified(normalized_base);
  const util::TextFileReadResult incoming_read = util::ReadTextFileClassified(normalized_incoming);
  const util::TextFileReadResult current_read = util::ReadTextFileClassified(normalized_current);
  if (!base_read.ok() || !incoming_read.ok() || !current_read.ok()) {
    return std::nullopt;
  }

  auto merge_tab = BuildMergeTabFromBuffers(
      normalized_output.empty() ? normalized_current : normalized_output, base_read.content,
      incoming_read.content, current_read.content,
      RelativePathLabel(context_.current_project_state.root, normalized_incoming),
      RelativePathLabel(context_.current_project_state.root, normalized_output.empty() ? normalized_current : normalized_output),
      RelativePathLabel(context_.current_project_state.root, normalized_current), 0, true);
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
      normalized_output.empty() ? std::nullopt : util::ReadTextFile(normalized_output);

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
  merge_tab.result_line_ending = util::DetectLineEnding(line_ending_source);
  merge_tab.model = compare::BuildMergeModel(base_content, incoming_content, current_content);
  merge_tab.model.file_conflict = compare::ClassifyMergeFileConflict(
      compare::MergeConflictClassificationInput{
          .base_exists = !base_content.empty(),
          .incoming_exists = !incoming_content.empty(),
          .current_exists = !current_content.empty(),
          .base_content = base_content,
          .incoming_content = incoming_content,
          .current_content = current_content,
      });
  merge_tab.file_conflict = merge_tab.model.file_conflict;
  merge_tab.incoming_token_window.Reset(
      merge_tab.model.incoming_lines.size(),
      editor::SyntaxHighlighter::InitialState(normalized_output, merge_tab.model.incoming_lines));
  merge_tab.current_token_window.Reset(
      merge_tab.model.current_lines.size(),
      editor::SyntaxHighlighter::InitialState(normalized_output, merge_tab.model.current_lines));
  merge_tab.persisted_output_baseline = output_text;
  merge_tab.selected_hunk = selected_hunk;
  merge_tab.scroll_row = 0;
  merge_tab.result_viewport.SetPath(merge_tab.output_path);
  if (output_text.has_value()) {
    std::optional<ParsedGitConflictOutput> parsed_output;
    if (util::ContainsCompleteConflictMarkers(*output_text)) {
      parsed_output = ParseGitConflictOutput(merge_tab.model, *output_text);
    }
    const std::string rendered_text =
        parsed_output.has_value()
            ? util::SerializeLines(parsed_output->result_lines, merge_tab.result_line_ending)
            : *output_text;
    merge_tab.result_viewport.LoadContent(rendered_text, merge_tab.output_path,
                                          merge_tab.result_line_ending);
    merge_tab.result_viewport.SetPath(merge_tab.output_path);
    merge_tab.result_viewport.SetDirty(false);
    if (parsed_output.has_value()) {
      merge_tab.conflicts = BuildMergeTrackedConflictsForResult(
          merge_tab.model, merge_tab.result_viewport.lines().Snapshot(), parsed_output->conflict_lines,
          parsed_output->conflict_choices, parsed_output->conflict_resolved);
    } else {
      merge_tab.conflicts =
          BuildMergeTrackedConflictsForResult(merge_tab.model, merge_tab.result_viewport.lines().Snapshot());
    }
    ResyncMergeTabViewState(merge_tab);
  } else {
    RefreshMergeTabDerivedState(merge_tab);
  }
  return TabEntry{
      .kind = TabEntry::Kind::Merge,
      .path = merge_tab.output_path,
      .title = merge_tab.title,
      .editor_state = std::nullopt,
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::move(merge_tab),
  };
}

void WorkspaceShell::InvalidateStaleMergeTabs() {
  const GitRepositoryService::Summary repository = git_repository_service_.CurrentSummary();
  for (TabEntry& tab : context_.current_project_state.focused_group().open_tabs) {
    if (!tab.merge.has_value()) {
      continue;
    }
    MergeTabState& merge_tab = tab.merge.value();
    if (merge_tab.open_index_generation != 0 &&
        merge_tab.open_index_generation != repository.generation) {
      merge_tab.index_stale = true;
      merge_tab.marked_resolved = false;
    }
    if (MergeResultChangedOnDisk(merge_tab)) {
      merge_tab.external_result_stale = true;
      merge_tab.marked_resolved = false;
    }
    merge_tab.remaining_conflicted_files = repository.conflicted_entry_count;
  }
}

void WorkspaceShell::FinalizeGitMergeTab(MergeTabState& merge_tab,
                                         const std::filesystem::path& path) {
  const project::GitRepositoryState repository_state = git_repository_service_.CurrentState();
  const MergeResolverLabels labels =
      BuildMergeResolverLabels(context_.current_project_state.root, path, repository_state);
  merge_tab.incoming_label = labels.incoming_label;
  merge_tab.current_label = labels.current_label;
  merge_tab.result_label = labels.result_label;
  merge_tab.base_label = labels.base_label;

  const std::optional<project::GitRepositoryEntry> entry =
      FindConflictRepositoryEntry(repository_state, path);
  const std::string base_content =
      util::SerializeLinesStreaming(merge_tab.model.base_lines, merge_tab.result_line_ending);
  const std::string incoming_content =
      util::SerializeLinesStreaming(merge_tab.model.incoming_lines, merge_tab.result_line_ending);
  const std::string current_content =
      util::SerializeLinesStreaming(merge_tab.model.current_lines, merge_tab.result_line_ending);
  const compare::MergeConflictClassificationInput classification{
      .repository_entry = entry.has_value() ? &*entry : nullptr,
      .base_exists = !merge_tab.model.base_lines.empty(),
      .incoming_exists = !merge_tab.model.incoming_lines.empty(),
      .current_exists = !merge_tab.model.current_lines.empty(),
      .base_content = base_content,
      .incoming_content = incoming_content,
      .current_content = current_content,
  };
  merge_tab.model.file_conflict = compare::ClassifyMergeFileConflict(classification);
  merge_tab.file_conflict = merge_tab.model.file_conflict;

  merge_tab.open_index_generation = repository_state.generation;
  std::size_t remaining_files = 0;
  for (const project::GitRepositoryEntry& repository_entry : repository_state.entries) {
    if (repository_entry.conflicted) {
      ++remaining_files;
    }
  }
  merge_tab.remaining_conflicted_files = remaining_files;
  std::error_code output_exists_error;
  if (!merge_tab.output_path.empty() &&
      std::filesystem::exists(merge_tab.output_path, output_exists_error) && !output_exists_error) {
    std::error_code error;
    const auto tick = std::filesystem::last_write_time(merge_tab.output_path, error);
    if (!error) {
      merge_tab.disk_result_tick =
          static_cast<std::uint64_t>(tick.time_since_epoch().count());
    }
  }
}

}
