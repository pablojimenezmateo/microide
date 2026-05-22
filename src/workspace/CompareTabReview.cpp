#include "workspace/CompareTabReview.h"

#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

compare::ComparePresentationOptions PresentationOptionsFromTab(const CompareTabState& compare_tab) {
  return compare::ComparePresentationOptions{
      .show_whitespace = compare_tab.show_whitespace,
  };
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
  };
  if (input.git_entry.has_value() && input.git_entry->old_path.has_value()) {
    semantic_input.old_path = input.git_entry->old_path->relative_path;
  }
  compare_tab.semantic_file = compare::InferCompareSemanticFileMetadata(semantic_input);
}

void RefreshCompareTabPresentation(CompareTabState& compare_tab) {
  compare_tab.presentation = compare::BuildComparePresentationModel(
      compare_tab.model, compare_tab.semantic_file, PresentationOptionsFromTab(compare_tab),
      compare_tab.presentation.collapse_state, compare_tab.model_revision);
  if (!compare_tab.presentation.rows.empty()) {
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

}  // namespace microide::workspace
