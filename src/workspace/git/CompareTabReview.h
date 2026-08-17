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

// ---- soft-wrap row table (TD-2026-08-13-200) --------------------------------
//
// With `editor.wrap` off every one of these is the identity and costs a branch:
// a presentation row IS an on-screen row. With it on, a presentation row occupies
// max(left segments, right segments) on-screen rows, the shorter pane padded with
// blank rows so the two sides stay aligned.
//
// `compare_tab.scroll_row` indexes on-screen (visual) rows in BOTH modes;
// `compare_tab.selected_row` stays a presentation-row index in both, so selecting
// a wrapped line highlights all of its rows.

// Rebuild the table if the panes, the tab size, the wrap flag or the presentation
// moved. Called from the layout pass, which is the one place that knows the pane
// widths.
void EnsureCompareWrapLayout(const CompareTabState& compare_tab,
                             bool soft_wrap,
                             std::size_t left_columns,
                             std::size_t right_columns);
// Re-wrap against the geometry the table was last built with, after the content or
// the presentation changed away from a layout pass (collapsed-context expand).
// No-op while wrap is off.
void RefreshCompareWrapLayoutForContent(const CompareTabState& compare_tab);

std::size_t CompareTabVisualRowCount(const CompareTabState& compare_tab);
std::size_t CompareVisualRowToPresentationRow(const CompareTabState& compare_tab,
                                              std::size_t visual_row);
std::size_t ComparePresentationRowToVisualRow(const CompareTabState& compare_tab,
                                              std::size_t presentation_row);

// Right-pane document line for a model row (the row's own line, else the next row
// below that has one). The canonical implementation; WorkspaceShell forwards.
std::size_t CompareTabRightLineForModelRow(const CompareTabState& compare_tab,
                                           std::size_t model_row);

// Presentation row of the diff's FIRST change, or 0 when the diff has no hunks.
//
// A comparison opens to be read, and what the reader opened it for is the change;
// landing on row 0 puts them in whatever unchanged context happens to precede it,
// which for a large file is the whole screen. VS Code reveals the first change
// region, and so does this (TD-2026-08-17-258's product half).
std::size_t CompareFirstChangePresentationRow(const CompareTabState& compare_tab);

// Carry the editable pane's caret to the line the selection now names.
//
// `selected_row` is a PRESENTATION row and the right pane edits MODEL lines, so the
// two agree only if something maps one onto the other — and until this existed,
// nothing did. Every hunk jump moved the selection and revealed it while the caret
// stayed wherever it was, so a reader could jump to a change 300 lines down, type,
// and edit line 0. No-op on a read-only right pane, where there is no caret to
// disagree with.
void SyncCompareCaretToSelectedRow(CompareTabState& compare_tab);

// Where the right pane's caret sits on screen. `column` is measured in the pane's
// own on-screen cells — with wrap on that is the hanging indent plus the offset
// into the segment, with wrap off it is the line's visual column — so both feed
// TextGridCursorX unchanged.
struct CompareRightCaretPlacement {
  std::size_t visual_row = 0;
  std::size_t column = 0;
};
CompareRightCaretPlacement CompareRightCaretPlacementFor(const CompareTabState& compare_tab,
                                                         std::size_t right_line,
                                                         std::size_t visual_column);

// Inverse: the right-pane document position under an on-screen (row, column).
struct CompareRightPaneHit {
  std::size_t presentation_row = 0;
  std::size_t model_row = 0;
  std::size_t line = 0;
  std::size_t visual_column = 0;
};
CompareRightPaneHit CompareRightPaneHitAt(const CompareTabState& compare_tab,
                                          std::size_t visual_row,
                                          std::size_t screen_column);

// Anchor for the right pane's inline blame annotation, given the line's full
// visual width: the on-screen row of its LAST wrapped segment and the on-screen
// column just past its text. `valid` is false when the diff does not represent
// that document line (a deleted-only row, a stale line index).
struct CompareRightLineAnchor {
  std::size_t visual_row = 0;
  std::size_t end_column = 0;
  bool valid = false;
};
CompareRightLineAnchor CompareRightLineBlameAnchor(const CompareTabState& compare_tab,
                                                   std::size_t right_line,
                                                   std::size_t line_visual_columns);

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
