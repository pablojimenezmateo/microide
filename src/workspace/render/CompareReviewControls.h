#pragma once

// The review controls a compare surface draws and answers clicks on: the header's
// Previous / Next Change buttons and, on a review, its Mark File Reviewed button;
// each hunk's Mark Reviewed / Comment buttons on its first row (shown while the
// hunk is hovered or holds the selection) with its review badge; and the Edit /
// Delete buttons of a review note row.
//
// Paint, the click, the hover highlight, the cursor shape and the control
// channel's element list all lay these out through the functions below, so the
// hand cursor cannot sit over a button the click would ignore.

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string_view>

#include "compare/BranchReviewStateTypes.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::render {
class TextRenderer;
}

namespace microide::workspace {

// What of a compare surface's layout the controls are placed against (the
// fields of the shell's CompareSurfaceLayout they read, plus the surface).
struct CompareControlGeometry {
  SDL_FRect surface{};
  float review_summary_y = 0.0f;
  float rows_y = 0.0f;
  float line_height = 14.0f;
  float right_x = 0.0f;
  float gutter_width = 0.0f;
  float right_width = 0.0f;
  bool show_vertical = false;
};

// From the shell's CompareSurfaceLayout (a template only so this header need not
// include the shell).
template <typename SurfaceLayout>
CompareControlGeometry MakeCompareControlGeometry(const SDL_FRect& surface, const SurfaceLayout& layout) {
  return CompareControlGeometry{
      .surface = surface,
      .review_summary_y = layout.review_summary_y,
      .rows_y = layout.rows_y,
      .line_height = layout.line_height,
      .right_x = layout.right_x,
      .gutter_width = layout.gutter_width,
      .right_width = layout.right_width,
      .show_vertical = layout.show_vertical,
  };
}

// ---- labels -----------------------------------------------------------------

inline constexpr std::string_view kComparePreviousChangeLabel = "Previous Change";
inline constexpr std::string_view kCompareNextChangeLabel = "Next Change";
inline constexpr std::string_view kCompareHunkCommentLabel = "Comment";
inline constexpr std::string_view kCompareNoteEditLabel = "Edit";
inline constexpr std::string_view kCompareNoteDeleteLabel = "Delete";
// The caption before the text on note row `note_hunk` (a hunk, the file's
// kCompareFileNoteHunk, or a detached note's CompareDetachedNoteHunk): "Comment: ",
// "File comment: ", and for a note whose change was edited since or is gone, so.
std::string_view CompareNoteCaption(const CompareTabState& compare_tab, int note_hunk);

// "Mark Reviewed", or "Unmark" for a hunk already marked.
std::string_view CompareHunkToggleLabel(compare::BranchReviewMarkerStatus status);
// The badge a marked hunk keeps on its first row: "Reviewed", "Changed since
// review", or nothing.
std::string_view CompareHunkBadgeLabel(compare::BranchReviewMarkerStatus status);
// "Mark File Reviewed", or "Unmark File" once every hunk is reviewed.
std::string_view CompareFileReviewedLabel(bool file_reviewed);

// ---- what a tab shows -------------------------------------------------------

// Every hunk of the file is marked reviewed.
bool CompareFileFullyReviewed(const CompareTabState& compare_tab);
// Hunk `hunk`'s review status as the last marker pass resolved it.
compare::BranchReviewMarkerStatus CompareHunkReviewStatus(const CompareTabState& compare_tab, int hunk);
// Whether hunk `hunk` has a note under it (the last marker pass's answer).
bool CompareHunkHasNote(const CompareTabState& compare_tab, int hunk);
// Whether hunk `hunk` shows its review buttons: a review, and the hunk is
// hovered or holds the selection.
bool CompareHunkShowsReviewButtons(const CompareTabState& compare_tab, int hunk);
// The hunk whose FIRST row presentation row `presentation_row` is, or -1.
int CompareHunkStartingAt(const CompareTabState& compare_tab, std::size_t presentation_row);
// The hunk whose review controls on-screen row `visible_row` (showing wrapped row
// `wrap`) carries, or -1: the hunk's first row -- or, once that has scrolled off
// above, the view's top row for as long as it is inside the hunk, so a change
// longer than the view keeps its buttons. Paint, the row's key, the click and
// the element list all ask this.
int CompareHunkControlsOnRow(const CompareTabState& compare_tab, const DiffWrapRow& wrap, int visible_row);
// The header's own buttons, as against a row's.
constexpr bool IsCompareHeaderControl(CompareHoverKind kind) {
  return kind == CompareHoverKind::HeaderPreviousChange || kind == CompareHoverKind::HeaderNextChange ||
         kind == CompareHoverKind::HeaderToggleFileReviewed;
}
// The hunk presentation row `presentation_row` belongs to (a changed row's, or
// a hunk note's), or -1.
int CompareHunkOfPresentationRow(const CompareTabState& compare_tab, std::size_t presentation_row);

// ---- geometry ---------------------------------------------------------------

struct CompareHeaderButtonRects {
  std::optional<SDL_FRect> file_reviewed;
  std::optional<SDL_FRect> previous;
  std::optional<SDL_FRect> next;
  // Where the header's text has to stop.
  float text_right_edge = 0.0f;
};
// The header line's buttons, right-aligned on it: Previous / Next Change when
// the diff has changes, Mark File Reviewed on a review. The summary keeps its
// width up to half the line (it carries the review's progress); a button that
// does not fit beside that is left out (the file button first).
CompareHeaderButtonRects BuildCompareHeaderButtonRects(const render::TextRenderer& text_renderer,
                                                       const CompareControlGeometry& geometry,
                                                       const CompareTabState& compare_tab);

struct CompareHunkReviewRects {
  std::optional<SDL_FRect> badge;
  std::optional<SDL_FRect> toggle;
  std::optional<SDL_FRect> comment;
};
// A hunk's review badge and buttons on its first row (`visible_row` on screen),
// right-aligned on the working side's text. The buttons are present only while
// the hunk shows them; Comment only while it has no note (its note row edits it)
// and only beside Mark Reviewed -- a narrow pane keeps the mark, not the comment.
CompareHunkReviewRects BuildCompareHunkReviewRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   const CompareTabState& compare_tab,
                                                   int hunk,
                                                   int visible_row);

struct CompareNoteActionRects {
  SDL_FRect block{};
  SDL_FRect edit{};
  SDL_FRect remove{};
  // Where the row's text starts (after the accent edge) and has to stop.
  float text_x = 0.0f;
  float text_right_edge = 0.0f;
};
// A review note row's block (as a collapsed-context block is laid out) and its
// Edit / Delete buttons.
CompareNoteActionRects BuildCompareNoteActionRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   int visible_row);

// ---- hit testing ------------------------------------------------------------

// The review note row under (x, y) whose comment does not fit its row, for the
// tooltip that shows it whole: the comment (the row's text) and the row's block.
struct CompareClippedNote {
  std::string_view text;
  SDL_FRect block{};
};
std::optional<CompareClippedNote> CompareClippedNoteAt(const render::TextRenderer& text_renderer,
                                                       const CompareControlGeometry& geometry,
                                                       const CompareTabState& compare_tab,
                                                       float x,
                                                       float y);

struct CompareControlHit {
  CompareHoverKind kind = CompareHoverKind::HeaderNextChange;
  std::size_t presentation_row = 0;
  // The hunk a hunk or note control acts on; -1 for the file's note or a
  // header button.
  int hunk = -1;
};
// What on-screen row `visible_row` of the rows carries for the review controls:
// its presentation row, and either a note row's hunk (`note`) or the hunk whose
// buttons it shows (`controls_hunk`, -1: none). Nullopt past the last row.
struct CompareControlRow {
  std::size_t presentation_row = 0;
  const compare::ComparePresentationRow* row = nullptr;
  bool note = false;
  int controls_hunk = -1;
};
std::optional<CompareControlRow> CompareControlRowAt(const CompareTabState& compare_tab, int visible_row);

// The review or header control under (x, y), if any. Collapsed-context buttons
// resolve through CompareCollapsedContextActionAt.
std::optional<CompareControlHit> CompareControlAt(const render::TextRenderer& text_renderer,
                                                  const CompareControlGeometry& geometry,
                                                  const CompareTabState& compare_tab,
                                                  float x,
                                                  float y);
// The on-screen row at y (what CompareTabState::review_hover_visible_row holds),
// or -1 above the rows and on a diff that takes no review marks.
int CompareHoverRowAtY(const CompareControlGeometry& geometry, const CompareTabState& compare_tab, float y);
// The hunk under the pointer (a changed row's or a hunk note's), or -1: the hunk
// that shows its buttons on hover.
int CompareHoveredHunk(const CompareTabState& compare_tab);

}  // namespace microide::workspace
