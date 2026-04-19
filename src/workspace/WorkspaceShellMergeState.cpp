#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <string_view>

#include "editor/SyntaxHighlighter.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::size_t MaxVisualColumnsForLines(const std::vector<std::string>& lines) {
  std::size_t max_columns = 0;
  for (const std::string& line : lines) {
    max_columns = std::max(max_columns, Utf8CodepointCount(line));
  }
  return max_columns;
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
  const std::size_t best_prefix =
      std::max({base_prefix, incoming_prefix, current_prefix, both_prefix});
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
    if (std::equal(needle.begin(), needle.end(),
                   haystack.begin() + static_cast<std::ptrdiff_t>(i))) {
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
      parsed.result_lines.insert(parsed.result_lines.end(), selected.lines.begin(),
                                 selected.lines.end());
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
            const bool immediate_context_matches = MatchesLineSegment(
                result_lines, result_line + candidate_lines.size(), post_context_lines);
            const bool later_context_exists =
                FindSequence(result_lines, result_line + candidate_lines.size(), post_context)
                    .has_value();
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

  const std::optional<std::string> base_text = util::ReadTextFile(normalized_base);
  const std::optional<std::string> incoming_text = util::ReadTextFile(normalized_incoming);
  const std::optional<std::string> current_text = util::ReadTextFile(normalized_current);
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

}  // namespace microide::workspace
