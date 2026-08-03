#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitRepositoryState.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/state/WorkspaceTabState.h"

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
  // Set by the caller when the compared content actually changed since the last
  // refresh. The cheap review/staging/branch-target inference below always reruns;
  // this only gates the semantic classification, which has to read both whole
  // buffers. Callers that cannot tell leave it true and pay the old cost.
  bool content_changed = true;
};

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input);

void RefreshCompareTabPresentation(CompareTabState& compare_tab);
// Clamps the selected presentation row into range and moves it off a non-Model row
// (a metadata or collapsed-context summary line) onto the first real row. Split out
// of RefreshCompareTabPresentation so a refresh that skips the presentation rebuild
// still normalizes the selection — the rebuild is now conditional, the invariant
// is not.
void NormalizeCompareSelectionToModelRow(CompareTabState& compare_tab);
void RefreshCompareReviewHeader(CompareTabState& compare_tab);

void ApplyBranchReviewPresentationMarkers(CompareTabState& compare_tab,
                                          const compare::BranchReviewStateService& review_service);

std::size_t CompareTabPresentationRowCount(const CompareTabState& compare_tab);
std::size_t CompareTabSelectedModelRow(const CompareTabState& compare_tab);
const compare::CompareRow& CompareTabSelectedModelRowRef(const CompareTabState& compare_tab);
int CompareTabSelectedHunkIndex(const CompareTabState& compare_tab);
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
