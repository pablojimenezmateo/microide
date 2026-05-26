#include "workspace/CompareTabReview.h"

#include "compare/BranchReviewStateTypes.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

compare::ComparePresentationOptions PresentationOptionsFromTab(const CompareTabState& compare_tab) {
  return compare::ComparePresentationOptions{
      .show_whitespace = compare_tab.show_whitespace,
  };
}

compare::ComparePresentationCollapsedRunState* FindCollapsedRunState(
    compare::ComparePresentationCollapseState& collapse_state,
    std::size_t run_start_model_row,
    std::size_t run_length) {
  for (compare::ComparePresentationCollapsedRunState& state : collapse_state.collapsed_runs) {
    if (state.run_start_model_row == run_start_model_row && state.run_length == run_length) {
      return &state;
    }
  }
  return nullptr;
}

}  // namespace

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input) {
  compare_tab.review_mode = compare::InferCompareReviewMode(compare_tab.commit_hash,
                                                            compare_tab.right_ref,
                                                            input.opened_from_commit_picker);
  compare_tab.staging_view =
      compare::InferWorkingTreeStagingView(compare_tab.commit_hash, compare_tab.right_ref);
  if (compare_tab.review_mode == compare::CompareReviewMode::Branch) {
    compare_tab.branch_target = compare::MakeBranchReviewTargetIdentity(
        input.repository_root, compare_tab.commit_hash, compare_tab.right_ref,
        input.merge_base_commit, input.snapshot_generation);
  }
  compare::CompareSemanticMetadataInput semantic_input{
      .path = compare_tab.path,
      .left_content = compare_tab.left_content,
      .right_content =
          util::SerializeLines(compare_tab.right_viewport.lines(),
                               compare_tab.right_viewport.line_ending()),
      .git_entry = input.git_entry,
      .old_path = {},
  };
  if (input.git_entry.has_value() && input.git_entry->old_path.has_value()) {
    semantic_input.old_path = input.git_entry->old_path->relative_path;
  }
  compare_tab.semantic_file = compare::InferCompareSemanticFileMetadata(semantic_input);
}

void ApplyBranchReviewPresentationMarkers(
    CompareTabState& compare_tab,
    const compare::BranchReviewStateService& review_service) {
  if (compare_tab.review_mode != compare::CompareReviewMode::Branch) {
    return;
  }
  const compare::BranchReviewStateQueryInput base_query{
      .target = compare_tab.branch_target,
      .path = compare_tab.path,
      .model = &compare_tab.model,
  };
  for (compare::ComparePresentationRow& row : compare_tab.presentation.rows) {
    row.review_marker_label.clear();
    row.has_review_note = false;
    if (row.kind == compare::ComparePresentationRowKind::HunkHeader && row.hunk_index >= 0) {
      compare::BranchReviewStateQueryInput query = base_query;
      query.selected_hunk_index = row.hunk_index;
      row.review_marker_label =
          compare::BranchReviewMarkerLabel(review_service.HunkStatus(query));
      row.has_review_note =
          review_service.HasNote(query, compare::BranchReviewNoteScope::Hunk);
      continue;
    }
    if (row.kind == compare::ComparePresentationRowKind::Model && row.hunk_index >= 0) {
      compare::BranchReviewStateQueryInput query = base_query;
      query.selected_hunk_index = row.hunk_index;
      row.review_marker_label =
          compare::BranchReviewMarkerLabel(review_service.HunkStatus(query));
      row.has_review_note =
          review_service.HasNote(query, compare::BranchReviewNoteScope::Hunk);
    }
  }
}

void RefreshCompareTabPresentation(CompareTabState& compare_tab) {
  compare_tab.presentation = compare::BuildComparePresentationModel(
      compare_tab.model, compare_tab.semantic_file, PresentationOptionsFromTab(compare_tab),
      compare_tab.presentation.collapse_state, compare_tab.model_revision);
  ++compare_tab.presentation_revision;
  if (!compare_tab.presentation.rows.empty()) {
    compare_tab.selected_row =
        std::min(compare_tab.selected_row, compare_tab.presentation.rows.size() - 1);
    const auto& selected = compare_tab.presentation.rows[compare_tab.selected_row];
    if (selected.kind != compare::ComparePresentationRowKind::Model) {
      for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
        if (compare_tab.presentation.rows[i].kind == compare::ComparePresentationRowKind::Model) {
          compare_tab.selected_row = i;
          break;
        }
      }
    }
  }
}

std::size_t CompareTabPresentationRowCount(const CompareTabState& compare_tab) {
  if (!compare_tab.presentation.rows.empty()) {
    return compare_tab.presentation.rows.size();
  }
  return compare_tab.model.rows.size();
}

std::size_t CompareTabSelectedModelRow(const CompareTabState& compare_tab) {
  if (!compare_tab.presentation.rows.empty()) {
    return compare::ComparePresentationToModelRow(compare_tab.presentation, compare_tab.selected_row);
  }
  return compare_tab.selected_row;
}

const compare::CompareRow& CompareTabSelectedModelRowRef(const CompareTabState& compare_tab) {
  const std::size_t model_row = CompareTabSelectedModelRow(compare_tab);
  return compare_tab.model.rows[std::min(model_row, compare_tab.model.rows.size() - 1)];
}

int CompareTabSelectedHunkIndex(const CompareTabState& compare_tab) {
  if (compare_tab.model.rows.empty()) {
    return -1;
  }
  const std::size_t model_row = CompareTabSelectedModelRow(compare_tab);
  return compare_tab.model.rows[model_row].hunk;
}

void SetCompareTabSelectedPresentationRow(CompareTabState& compare_tab, std::size_t row) {
  const std::size_t row_count = CompareTabPresentationRowCount(compare_tab);
  compare_tab.selected_row = row_count == 0 ? 0 : std::min(row, row_count - 1);
}

const compare::ComparePresentationRow* CompareTabPresentationRowAt(
    const CompareTabState& compare_tab,
    std::size_t presentation_row) {
  if (presentation_row >= compare_tab.presentation.rows.size()) {
    return nullptr;
  }
  return &compare_tab.presentation.rows[presentation_row];
}

std::size_t CompareTabModelRowForRightLine(const CompareTabState& compare_tab,
                                           std::size_t right_line_index) {
  if (compare_tab.model.rows.empty()) {
    return 0;
  }

  const int target_line = static_cast<int>(right_line_index + 1);
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

std::optional<std::size_t> CompareTabPresentationRowForHunk(const CompareTabState& compare_tab,
                                                            int hunk_index) {
  if (hunk_index < 0) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
    const auto& row = compare_tab.presentation.rows[i];
    if (row.kind == compare::ComparePresentationRowKind::HunkHeader && row.hunk_index == hunk_index) {
      return i;
    }
    if (row.kind == compare::ComparePresentationRowKind::Model &&
        row.model_row_index < compare_tab.model.rows.size() &&
        compare_tab.model.rows[row.model_row_index].hunk == hunk_index) {
      return i;
    }
  }
  return std::nullopt;
}

bool ExpandCompareCollapsedContext(CompareTabState& compare_tab,
                                   std::size_t presentation_row,
                                   CompareCollapsedContextAction action,
                                   std::size_t reveal_lines) {
  const compare::ComparePresentationRow* row =
      CompareTabPresentationRowAt(compare_tab, presentation_row);
  if (row == nullptr || row->kind != compare::ComparePresentationRowKind::CollapsedContext ||
      row->collapsed_line_count <= 0) {
    return false;
  }

  const std::size_t collapsed_lines = static_cast<std::size_t>(row->collapsed_line_count);
  const std::size_t previous_selected_row = compare_tab.selected_row;
  const int previous_scroll_row = compare_tab.scroll_row;
  const std::size_t collapsed_run_start_model_row = row->collapsed_run_start_model_row;
  const std::size_t collapsed_run_length = row->collapsed_run_length;
  std::size_t revealed_before = 0;
  compare::ComparePresentationCollapsedRunState* collapsed_run_state =
      FindCollapsedRunState(compare_tab.presentation.collapse_state, collapsed_run_start_model_row,
                            collapsed_run_length);
  if (collapsed_run_state == nullptr) {
    compare_tab.presentation.collapse_state.collapsed_runs.push_back(
        compare::ComparePresentationCollapsedRunState{
            .run_start_model_row = collapsed_run_start_model_row,
            .run_length = collapsed_run_length,
        });
    collapsed_run_state =
        &compare_tab.presentation.collapse_state.collapsed_runs.back();
  }
  auto grow_reveal = [&](std::size_t& current,
                         std::size_t opposite,
                         std::size_t amount,
                         std::size_t* revealed_delta) {
    const std::size_t max_reveal =
        collapsed_run_length > opposite ? collapsed_run_length - opposite : 0;
    const std::size_t target = std::min(max_reveal, current + amount);
    if (current >= target) {
      return false;
    }
    if (revealed_delta != nullptr) {
      *revealed_delta += target - current;
    }
    current = target;
    return true;
  };

  bool changed = false;
  switch (action) {
    case CompareCollapsedContextAction::ShowPrevious:
      changed = grow_reveal(collapsed_run_state->expanded_above,
                            collapsed_run_state->expanded_below, reveal_lines,
                            &revealed_before);
      break;
    case CompareCollapsedContextAction::ShowAll:
      changed = grow_reveal(collapsed_run_state->expanded_above,
                            collapsed_run_state->expanded_below, collapsed_lines,
                            &revealed_before) ||
                changed;
      changed = grow_reveal(collapsed_run_state->expanded_below,
                            collapsed_run_state->expanded_above, collapsed_lines, nullptr) ||
                changed;
      break;
    case CompareCollapsedContextAction::ShowNext:
      changed = grow_reveal(collapsed_run_state->expanded_below,
                            collapsed_run_state->expanded_above, reveal_lines, nullptr);
      break;
  }
  if (!changed) {
    return false;
  }
  RefreshCompareTabPresentation(compare_tab);
  if (revealed_before > 0) {
    compare_tab.scroll_row =
        std::max(0, previous_scroll_row + static_cast<int>(revealed_before));
  }
  const auto matches_collapsed_row = [&](const compare::ComparePresentationRow& candidate) {
    return candidate.kind == compare::ComparePresentationRowKind::CollapsedContext &&
           candidate.collapsed_run_start_model_row == collapsed_run_start_model_row &&
           candidate.collapsed_run_length == collapsed_run_length;
  };
  for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
    if (matches_collapsed_row(compare_tab.presentation.rows[i])) {
      compare_tab.selected_row = i;
      return true;
    }
  }
  const std::size_t target_selected_row = previous_selected_row + revealed_before;
  compare_tab.selected_row =
      compare_tab.presentation.rows.empty()
          ? 0
          : std::min(target_selected_row, compare_tab.presentation.rows.size() - 1);
  return true;
}

}  // namespace microide::workspace
