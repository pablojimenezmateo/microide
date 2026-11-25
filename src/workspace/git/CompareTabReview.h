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
  bool content_changed = true;
};

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input);

void RefreshCompareTabPresentation(CompareTabState& compare_tab);
void NormalizeCompareSelectionToModelRow(CompareTabState& compare_tab);
void RefreshCompareReviewHeader(CompareTabState& compare_tab);

bool CompareTabTakesReviewMarks(const CompareTabState& compare_tab);

void ApplyBranchReviewPresentationMarkers(CompareTabState& compare_tab,
                                          const compare::BranchReviewStateService& review_service);

std::size_t CompareTabPresentationRowCount(const CompareTabState& compare_tab);
std::size_t CompareTabSelectedModelRow(const CompareTabState& compare_tab);
const compare::CompareRow& CompareTabSelectedModelRowRef(const CompareTabState& compare_tab);
int CompareTabSelectedHunkIndex(const CompareTabState& compare_tab);
const compare::ComparePresentationRow* CompareTabPresentationRowAt(const CompareTabState& compare_tab,
                                                                 std::size_t presentation_row);

void EnsureCompareWrapLayout(const CompareTabState& compare_tab,
                             bool soft_wrap,
                             std::size_t left_columns,
                             std::size_t right_columns);
void RefreshCompareWrapLayoutForContent(const CompareTabState& compare_tab);

std::size_t CompareTabVisualRowCount(const CompareTabState& compare_tab);
std::size_t CompareVisualRowToPresentationRow(const CompareTabState& compare_tab,
                                              std::size_t visual_row);
std::size_t ComparePresentationRowToVisualRow(const CompareTabState& compare_tab,
                                              std::size_t presentation_row);

std::size_t CompareTabRightLineForModelRow(const CompareTabState& compare_tab,
                                           std::size_t model_row);

std::size_t CompareFirstChangePresentationRow(const CompareTabState& compare_tab);

void SyncCompareCaretToSelectedRow(CompareTabState& compare_tab);

struct CompareRightCaretPlacement {
  std::size_t visual_row = 0;
  std::size_t column = 0;
};
CompareRightCaretPlacement CompareRightCaretPlacementFor(const CompareTabState& compare_tab,
                                                         std::size_t right_line,
                                                         std::size_t visual_column);

struct CompareRightPaneHit {
  std::size_t presentation_row = 0;
  std::size_t model_row = 0;
  std::size_t line = 0;
  std::size_t visual_column = 0;
};
CompareRightPaneHit CompareRightPaneHitAt(const CompareTabState& compare_tab,
                                          std::size_t visual_row,
                                          std::size_t screen_column);

struct CompareRightLineAnchor {
  std::size_t visual_row = 0;
  std::size_t end_column = 0;
  bool valid = false;
};
CompareRightLineAnchor CompareRightLineBlameAnchor(const CompareTabState& compare_tab,
                                                   std::size_t right_line,
                                                   std::size_t line_visual_columns);

struct CompareCollapsedContextRowHit {
  const compare::ComparePresentationRow* row = nullptr;
  std::size_t presentation_row = 0;
  int visible_row = 0;
  SDL_FRect block_rect{};
};
std::optional<CompareCollapsedContextRowHit> CompareCollapsedContextRowAt(
    const CompareTabState& compare_tab,
    const SDL_FRect& editor_surface,
    float rows_y,
    float line_height,
    bool show_vertical_scrollbar,
    float y);

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

}
