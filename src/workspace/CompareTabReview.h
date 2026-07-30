#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitRepositoryState.h"
#include "workspace/CompareMergeRender.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

enum class CompareCollapsedContextAction {
  ShowPrevious,
  ShowAll,
  ShowNext,
};

struct CompareTabReviewRefreshInput {
  std::filesystem::path repository_root;
  std::optional<project::GitRepositoryEntry> git_entry;
  std::uint64_t snapshot_generation = 0;
  std::string merge_base_commit;
  bool opened_from_commit_picker = false;
};

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input);

void RefreshCompareTabPresentation(CompareTabState& compare_tab);
void RefreshCompareReviewHeader(CompareTabState& compare_tab);

void ApplyBranchReviewPresentationMarkers(CompareTabState& compare_tab,
                                          const compare::BranchReviewStateService& review_service);

std::size_t CompareTabPresentationRowCount(const CompareTabState& compare_tab);
std::size_t CompareTabSelectedModelRow(const CompareTabState& compare_tab);
const compare::CompareRow& CompareTabSelectedModelRowRef(const CompareTabState& compare_tab);
int CompareTabSelectedHunkIndex(const CompareTabState& compare_tab);
void SetCompareTabSelectedPresentationRow(CompareTabState& compare_tab, std::size_t row);

const compare::ComparePresentationRow* CompareTabPresentationRowAt(const CompareTabState& compare_tab,
                                                                 std::size_t presentation_row);

// The collapsed-context summary row under a y coordinate in the compare surface,
// with the exact block rect its action buttons are laid out in.
//
// Three paths have to agree about this: the click that runs the action, the hover
// highlight, and the cursor shape. Each used to re-derive it, and when those drift
// the hand cursor appears over something the click will not act on. Resolve
// through here, then through CompareCollapsedContextActionAt, so they cannot.
struct CompareCollapsedContextRowHit {
  const compare::ComparePresentationRow* row = nullptr;
  std::size_t presentation_row = 0;
  int visible_row = 0;  // 0-based offset from the first painted row
  SDL_FRect block_rect{};
};
std::optional<CompareCollapsedContextRowHit> CompareCollapsedContextRowAt(
    const CompareTabState& compare_tab,
    const SDL_FRect& editor_surface,
    float rows_y,
    float line_height,
    bool show_vertical_scrollbar,
    float y);

// Which of the block's three action buttons (if any) is under (x, y).
std::optional<CompareHoverKind> CompareCollapsedContextActionAt(
    const CollapsedContextActionRects& rects, float x, float y);
std::optional<std::size_t> CompareTabPresentationRowForHunk(const CompareTabState& compare_tab,
                                                            int hunk_index);
std::size_t CompareTabModelRowForRightLine(const CompareTabState& compare_tab,
                                           std::size_t right_line_index);
bool ExpandCompareCollapsedContext(CompareTabState& compare_tab,
                                   std::size_t presentation_row,
                                   CompareCollapsedContextAction action,
                                   std::size_t reveal_lines = 20);

}  // namespace microide::workspace
