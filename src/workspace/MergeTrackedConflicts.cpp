#include "workspace/MergeTrackedConflicts.h"

#include <algorithm>
#include <string_view>

#include "workspace/MergeWrapRows.h"

namespace microide::workspace {

namespace {

template <typename LinesRange, typename CandidateRange>
bool MatchesLineSegment(const LinesRange& lines, std::size_t start_line, const CandidateRange& candidate_lines) {
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

template <typename LhsRange, typename RhsRange>
std::size_t CommonPrefixLength(const LhsRange& lhs, const RhsRange& rhs) {
  std::size_t prefix = 0;
  while (prefix < lhs.size() && prefix < rhs.size() && lhs[prefix] == rhs[prefix]) {
    ++prefix;
  }
  return prefix;
}

compare::MergeChoice InferMergeChoiceFromLines(const compare::MergeHunk& hunk, std::span<const std::string> lines) {
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

template <typename HaystackRange, typename NeedleRange>
std::optional<std::size_t> FindSequence(const HaystackRange& haystack, std::size_t start, const NeedleRange& needle) {
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

}  // namespace

std::vector<MergeTrackedConflict> BuildMergeTrackedConflicts(const compare::MergeModel& model) {
  std::vector<MergeTrackedConflict> conflicts;
  std::size_t incoming_line = 0;
  std::size_t current_line = 0;
  std::size_t result_line = 0;
  int base_cursor = 0;
  for (const auto& hunk : model.hunks) {
    const std::size_t unchanged_lines = static_cast<std::size_t>(std::max(0, hunk.base_start - base_cursor));
    incoming_line += unchanged_lines;
    current_line += unchanged_lines;
    result_line += unchanged_lines;

    const std::size_t result_line_count = compare::MergeChoiceLineCount(hunk, hunk.choice);
    if (hunk.conflict) {
      conflicts.push_back(MergeTrackedConflict{
          .hunk_index = static_cast<std::size_t>(hunk.index),
          .incoming_start_line = incoming_line,
          .incoming_end_line = incoming_line + hunk.incoming_lines.size(),
          .current_start_line = current_line,
          .current_end_line = current_line + hunk.current_lines.size(),
          .start_line = result_line,
          .end_line = result_line + result_line_count,
          .last_choice = hunk.choice,
          .bootstrap_choice = hunk.bootstrap_choice,
          .valid = true,
          .resolved = false,
      });
    }

    incoming_line += hunk.incoming_lines.size();
    current_line += hunk.current_lines.size();
    result_line += result_line_count;
    base_cursor = hunk.base_end;
  }
  return conflicts;
}

std::vector<MergeTrackedConflict> BuildMergeTrackedConflictsForResult(
    compare::MergeModel& model, const std::vector<std::string>& result_lines,
    std::span<const std::vector<std::string>> conflict_line_hints, std::span<const compare::MergeChoice> choice_hints,
    std::span<const std::uint8_t> resolved_hints) {
  std::vector<MergeTrackedConflict> conflicts;
  std::size_t incoming_line = 0;
  std::size_t current_line = 0;
  std::size_t result_line = 0;
  int base_cursor = 0;
  std::size_t conflict_index = 0;
  for (std::size_t hunk_index = 0; hunk_index < model.hunks.size(); ++hunk_index) {
    auto& hunk = model.hunks[hunk_index];
    const std::size_t unchanged_lines = static_cast<std::size_t>(std::max(0, hunk.base_start - base_cursor));
    incoming_line += unchanged_lines;
    current_line += unchanged_lines;
    result_line += unchanged_lines;

    if (hunk.conflict) {
      const std::size_t next_base_start =
          hunk_index + 1 < model.hunks.size()
              ? static_cast<std::size_t>(std::max(0, model.hunks[hunk_index + 1].base_start))
              : model.base_lines.size();
      const std::size_t post_context_start = static_cast<std::size_t>(std::max(0, hunk.base_end));
      const std::span<const std::string_view> post_context =
          post_context_start <= model.base_lines.size() && post_context_start <= next_base_start
              ? std::span<const std::string_view>(
                    model.base_lines.data() + static_cast<std::ptrdiff_t>(post_context_start),
                    next_base_start - post_context_start)
              : std::span<const std::string_view>{};

      std::vector<std::string> committed_lines;
      bool valid = false;
      bool resolved = false;
      if (conflict_index < conflict_line_hints.size() && conflict_index < choice_hints.size()) {
        hunk.choice = choice_hints[conflict_index];
        committed_lines = conflict_line_hints[conflict_index];
        valid = true;
        // A hint is a real committed span, so it is valid geometry — but only
        // "resolved" if the parser said so. An untouched raw block reports
        // resolved=0 and must stay a pending conflict (default true keeps back-compat
        // for callers that pass no resolved_hints).
        resolved = conflict_index >= resolved_hints.size() || resolved_hints[conflict_index] != 0;
      } else {
        committed_lines = compare::MergeChoiceLines(hunk, hunk.choice);
        for (const compare::MergeChoice choice : PreferredMergeChoices(hunk)) {
          const std::vector<std::string> candidate_lines = compare::MergeChoiceLines(hunk, choice);
          if (!MatchesLineSegment(result_lines, result_line, candidate_lines)) {
            continue;
          }
          if (!post_context.empty()) {
            // Compared straight against the model's views. This used to copy the
            // post-context into an owned vector purely to bridge the two types --
            // once per candidate choice per conflict.
            const bool immediate_context_matches =
                MatchesLineSegment(result_lines, result_line + candidate_lines.size(), post_context);
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
        // Fall back to anchoring the committed span on the next base context. Guard
        // on a NON-EMPTY post_context: FindSequence returns `start` for an empty
        // needle, so an empty post_context (last conflict, or adjacent conflicts with
        // no base between them) would otherwise match at result_line and produce a
        // zero-length span that gets marked resolved — dropping the user's manual
        // edit from conflict tracking. With no anchor we leave it unresolved instead.
        if (!valid && !post_context.empty()) {
          if (const auto next_context = FindSequence(result_lines, result_line, post_context);
              next_context.has_value()) {
            committed_lines =
                std::vector<std::string>(result_lines.begin() + static_cast<std::ptrdiff_t>(result_line),
                                         result_lines.begin() + static_cast<std::ptrdiff_t>(*next_context));
            hunk.choice = InferMergeChoiceFromLines(hunk, committed_lines);
            valid = true;
          }
        }
        // Inferred spans (no hint) are resolved exactly when we could pin them down.
        resolved = valid;
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
          .bootstrap_choice = hunk.bootstrap_choice,
          .valid = valid,
          .resolved = resolved,
      });
      result_line += committed_lines.size();
      ++conflict_index;
    } else {
      result_line += compare::MergeChoiceLineCount(hunk, hunk.choice);
    }

    incoming_line += hunk.incoming_lines.size();
    current_line += hunk.current_lines.size();
    base_cursor = hunk.base_end;
  }
  return conflicts;
}

std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(const MergeTabState& merge_tab, std::size_t line,
                                                                bool incoming) {
  // `line` is an on-screen row from a hit test, so it resolves against the
  // row-space projection of the conflicts.
  return microide::workspace::FindMergeTrackedConflictAtSourceLine(MergeVisualConflicts(merge_tab), line, incoming);
}

std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(const MergeTabState& merge_tab, std::size_t line) {
  return microide::workspace::FindMergeTrackedConflictAtResultLine(MergeVisualConflicts(merge_tab), line);
}

}  // namespace microide::workspace
