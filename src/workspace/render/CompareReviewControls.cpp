#include "workspace/render/CompareReviewControls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/TextRenderer.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareMergeRender.h"

namespace microide::workspace {

namespace {

constexpr float kEdgeInset = 6.0f;

// The band a text line whose top is `line_y` paints (rows fill from a pixel above).
SDL_FRect LineBand(float x, float line_y, float width, float line_height) {
  return MakeRect(x, line_y - 1.0f, width, line_height);
}

// The on-screen row under y, or nullopt off the rows.
std::optional<int> VisibleRowAt(const CompareControlGeometry& geometry, float y) {
  const float offset = y - geometry.rows_y;
  if (offset < 0.0f || geometry.line_height <= 0.0f) {
    return std::nullopt;
  }
  return static_cast<int>(offset / geometry.line_height);
}

float RightTextEnd(const CompareControlGeometry& geometry) {
  return geometry.right_x + geometry.gutter_width + geometry.right_width;
}

}  // namespace

std::string_view CompareHunkToggleLabel(compare::BranchReviewMarkerStatus status) {
  return status == compare::BranchReviewMarkerStatus::Reviewed ? std::string_view("Unmark")
                                                               : std::string_view("Mark Reviewed");
}

std::string_view CompareHunkBadgeLabel(compare::BranchReviewMarkerStatus status) {
  switch (status) {
    case compare::BranchReviewMarkerStatus::Reviewed:
      return "Reviewed";
    case compare::BranchReviewMarkerStatus::ChangedSinceReviewed:
      return "Changed since review";
    case compare::BranchReviewMarkerStatus::Unreviewed:
      break;
  }
  return {};
}

std::string_view CompareFileReviewedLabel(bool file_reviewed) {
  return file_reviewed ? std::string_view("Unmark File") : std::string_view("Mark File Reviewed");
}

bool CompareFileFullyReviewed(const CompareTabState& compare_tab) {
  return !compare_tab.model.hunks.empty() &&
         compare_tab.review_reviewed_hunk_count >= compare_tab.model.hunks.size();
}

compare::BranchReviewMarkerStatus CompareHunkReviewStatus(const CompareTabState& compare_tab, int hunk) {
  if (!CompareTabTakesReviewMarks(compare_tab) || hunk < 0 ||
      static_cast<std::size_t>(hunk) >= compare_tab.review_hunk_markers.size()) {
    return compare::BranchReviewMarkerStatus::Unreviewed;
  }
  return compare_tab.review_hunk_markers[static_cast<std::size_t>(hunk)].status;
}

std::string_view CompareNoteCaption(const CompareTabState& compare_tab, int note_hunk) {
  if (compare::IsCompareDetachedNoteHunk(note_hunk)) {
    return "Comment (change gone), ";
  }
  if (note_hunk == compare::kCompareFileNoteHunk) {
    return "File comment: ";
  }
  return static_cast<std::size_t>(note_hunk) < compare_tab.review_hunk_markers.size() &&
                 compare_tab.review_hunk_markers[static_cast<std::size_t>(note_hunk)].note_outdated
             ? std::string_view("Comment (change edited since): ")
             : std::string_view("Comment: ");
}

bool CompareHunkHasNote(const CompareTabState& compare_tab, int hunk) {
  return hunk >= 0 && static_cast<std::size_t>(hunk) < compare_tab.review_hunk_markers.size() &&
         compare_tab.review_hunk_markers[static_cast<std::size_t>(hunk)].has_note;
}

bool CompareHunkShowsReviewButtons(const CompareTabState& compare_tab, int hunk) {
  return hunk >= 0 && CompareTabTakesReviewMarks(compare_tab) &&
         (hunk == CompareHoveredHunk(compare_tab) || hunk == CompareTabSelectedHunkIndex(compare_tab));
}

int CompareHunkStartingAt(const CompareTabState& compare_tab, std::size_t presentation_row) {
  const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, presentation_row);
  if (row == nullptr || row->kind != compare::ComparePresentationRowKind::Model ||
      row->model_row_index >= compare_tab.model.rows.size()) {
    return -1;
  }
  const std::size_t model_index = row->model_row_index;
  const int hunk = compare_tab.model.rows[model_index].hunk;
  if (hunk < 0 || (model_index > 0 && compare_tab.model.rows[model_index - 1].hunk == hunk)) {
    return -1;
  }
  return hunk;
}

int CompareHunkControlsOnRow(const CompareTabState& compare_tab, const DiffWrapRow& wrap, int visible_row) {
  if (!CompareTabTakesReviewMarks(compare_tab)) {
    return -1;
  }
  if (wrap.first) {
    if (const int hunk = CompareHunkStartingAt(compare_tab, wrap.unit); hunk >= 0) {
      return hunk;
    }
  }
  if (visible_row != 0) {
    return -1;
  }
  // The view's top row, inside a hunk whose first row is above the view.
  const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, wrap.unit);
  return row != nullptr && row->kind == compare::ComparePresentationRowKind::Model &&
                 row->model_row_index < compare_tab.model.rows.size()
             ? compare_tab.model.rows[row->model_row_index].hunk
             : -1;
}

std::optional<CompareControlRow> CompareControlRowAt(const CompareTabState& compare_tab, int visible_row) {
  const int visual_index = compare_tab.scroll_row + visible_row;
  if (visible_row < 0 || visual_index < 0 ||
      static_cast<std::size_t>(visual_index) >= CompareTabVisualRowCount(compare_tab)) {
    return std::nullopt;
  }
  const DiffWrapRow wrap = compare_tab.wrap_layout.RowAt(static_cast<std::size_t>(visual_index));
  const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, wrap.unit);
  if (row == nullptr) {
    return std::nullopt;
  }
  const bool note = row->kind == compare::ComparePresentationRowKind::ReviewNote;
  return CompareControlRow{
      .presentation_row = wrap.unit,
      .row = row,
      .note = note,
      .controls_hunk = note ? -1 : CompareHunkControlsOnRow(compare_tab, wrap, visible_row),
  };
}

int CompareHunkOfPresentationRow(const CompareTabState& compare_tab, std::size_t presentation_row) {
  const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, presentation_row);
  if (row == nullptr) {
    return -1;
  }
  if (row->kind == compare::ComparePresentationRowKind::ReviewNote) {
    return row->hunk_index;
  }
  if (row->kind != compare::ComparePresentationRowKind::Model ||
      row->model_row_index >= compare_tab.model.rows.size()) {
    return -1;
  }
  return compare_tab.model.rows[row->model_row_index].hunk;
}

CompareHeaderButtonRects BuildCompareHeaderButtonRects(const render::TextRenderer& text_renderer,
                                                       const CompareControlGeometry& geometry,
                                                       const CompareTabState& compare_tab) {
  const float reserve = geometry.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right = geometry.surface.x + std::max(0.0f, geometry.surface.w - reserve) - kEdgeInset;
  CompareButtonRowPlacer placer{
      .text_renderer = text_renderer,
      .row_rect = LineBand(geometry.surface.x, geometry.review_summary_y, geometry.surface.w, geometry.line_height),
      .right = right,
      // The summary's share: all of it, up to half the line.
      .left_limit = geometry.surface.x + 8.0f +
                    std::min(text_renderer.MeasureWidth(compare_tab.review_header.summary_line) + 14.0f,
                             std::max(0.0f, right - geometry.surface.x) * 0.5f),
  };
  CompareHeaderButtonRects rects;
  // Right to left: Next, Previous (the pair or neither), then the file's button.
  if (!compare_tab.model.hunks.empty()) {
    rects.next = placer.Place(kCompareNextChangeLabel);
    rects.previous = rects.next.has_value() ? placer.Place(kComparePreviousChangeLabel) : std::nullopt;
    if (!rects.previous.has_value()) {
      rects.next.reset();
      placer.right = right;
    }
  }
  if (CompareTabTakesReviewMarks(compare_tab) && rects.previous.has_value()) {
    rects.file_reviewed = placer.Place(CompareFileReviewedLabel(CompareFileFullyReviewed(compare_tab)));
  }
  rects.text_right_edge = std::max(geometry.surface.x, placer.right + kCompareButtonGap - 10.0f);
  return rects;
}

CompareHunkReviewRects BuildCompareHunkReviewRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   const CompareTabState& compare_tab,
                                                   int hunk,
                                                   int visible_row) {
  CompareHunkReviewRects rects;
  if (hunk < 0 || !CompareTabTakesReviewMarks(compare_tab)) {
    return rects;
  }
  const compare::BranchReviewMarkerStatus status = CompareHunkReviewStatus(compare_tab, hunk);
  const bool buttons = CompareHunkShowsReviewButtons(compare_tab, hunk);
  const bool has_note = CompareHunkHasNote(compare_tab, hunk);
  const float line_y = geometry.rows_y + static_cast<float>(visible_row) * geometry.line_height;
  CompareButtonRowPlacer placer{
      .text_renderer = text_renderer,
      .row_rect = LineBand(geometry.right_x, line_y, geometry.gutter_width + geometry.right_width, geometry.line_height),
      .right = RightTextEnd(geometry) - kEdgeInset,
      .left_limit = geometry.right_x + geometry.gutter_width,
  };
  // Right to left, so the badge sits left of the buttons it explains. The mark
  // is the control a narrow pane keeps: Comment only when both fit.
  if (buttons) {
    const std::string_view toggle_label = CompareHunkToggleLabel(status);
    const bool both_fit = placer.right - CompareButtonWidth(text_renderer, kCompareHunkCommentLabel) -
                              kCompareButtonGap - CompareButtonWidth(text_renderer, toggle_label) >=
                          placer.left_limit;
    if (!has_note && both_fit) {
      rects.comment = placer.Place(kCompareHunkCommentLabel);
    }
    rects.toggle = placer.Place(toggle_label);
  }
  if (const std::string_view badge = CompareHunkBadgeLabel(status); !badge.empty()) {
    rects.badge = placer.Place(badge);
  }
  return rects;
}

CompareNoteActionRects BuildCompareNoteActionRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   int visible_row) {
  CompareNoteActionRects rects;
  rects.block = CompareCollapsedContextBlockRect(geometry.surface, geometry.rows_y, geometry.line_height,
                                                 geometry.show_vertical, visible_row);
  CompareButtonRowPlacer placer{.text_renderer = text_renderer,
                                .row_rect = rects.block,
                                .right = rects.block.x + rects.block.w - 4.0f,
                                .left_limit = -std::numeric_limits<float>::infinity()};
  rects.remove = *placer.Place(kCompareNoteDeleteLabel);
  rects.edit = *placer.Place(kCompareNoteEditLabel);
  rects.text_x = rects.block.x + 10.0f;
  rects.text_right_edge = std::max(rects.block.x, rects.edit.x - 10.0f);
  return rects;
}

std::optional<CompareClippedNote> CompareClippedNoteAt(const render::TextRenderer& text_renderer,
                                                       const CompareControlGeometry& geometry,
                                                       const CompareTabState& compare_tab,
                                                       float x,
                                                       float y) {
  const std::optional<int> visible_row = Contains(geometry.surface, x, y) ? VisibleRowAt(geometry, y) : std::nullopt;
  const std::optional<CompareControlRow> row =
      visible_row.has_value() ? CompareControlRowAt(compare_tab, *visible_row) : std::nullopt;
  if (!row.has_value() || !row->note) {
    return std::nullopt;
  }
  const CompareNoteActionRects rects = BuildCompareNoteActionRects(text_renderer, geometry, *visible_row);
  if (x < rects.block.x || x >= rects.text_right_edge) {
    return std::nullopt;  // on its buttons, or off the block
  }
  const float room = rects.text_right_edge - rects.text_x -
                     text_renderer.MeasureWidth(CompareNoteCaption(compare_tab, row->row->hunk_index));
  if (text_renderer.MeasureWidth(row->row->summary_text) <= room) {
    return std::nullopt;
  }
  return CompareClippedNote{.text = row->row->summary_text, .block = rects.block};
}

std::optional<CompareControlHit> CompareControlAt(const render::TextRenderer& text_renderer,
                                                  const CompareControlGeometry& geometry,
                                                  const CompareTabState& compare_tab,
                                                  float x,
                                                  float y) {
  if (!Contains(geometry.surface, x, y)) {
    return std::nullopt;
  }
  if (y < geometry.rows_y) {
    const CompareHeaderButtonRects header = BuildCompareHeaderButtonRects(text_renderer, geometry, compare_tab);
    for (const auto& [rect, kind] :
         {std::pair{header.previous, CompareHoverKind::HeaderPreviousChange},
          std::pair{header.next, CompareHoverKind::HeaderNextChange},
          std::pair{header.file_reviewed, CompareHoverKind::HeaderToggleFileReviewed}}) {
      if (rect.has_value() && Contains(*rect, x, y)) {
        return CompareControlHit{.kind = kind, .presentation_row = 0, .hunk = -1};
      }
    }
    return std::nullopt;
  }
  const std::optional<int> visible_row = VisibleRowAt(geometry, y);
  const std::optional<CompareControlRow> row =
      visible_row.has_value() ? CompareControlRowAt(compare_tab, *visible_row) : std::nullopt;
  if (!row.has_value()) {
    return std::nullopt;
  }
  const auto hit = [&](CompareHoverKind kind, int hunk) {
    return CompareControlHit{.kind = kind, .presentation_row = row->presentation_row, .hunk = hunk};
  };
  if (row->note) {
    const CompareNoteActionRects rects = BuildCompareNoteActionRects(text_renderer, geometry, *visible_row);
    if (Contains(rects.edit, x, y)) {
      return hit(CompareHoverKind::NoteEdit, row->row->hunk_index);
    }
    if (Contains(rects.remove, x, y)) {
      return hit(CompareHoverKind::NoteDelete, row->row->hunk_index);
    }
    return std::nullopt;
  }
  if (row->controls_hunk < 0) {
    return std::nullopt;
  }
  const CompareHunkReviewRects rects =
      BuildCompareHunkReviewRects(text_renderer, geometry, compare_tab, row->controls_hunk, *visible_row);
  if (rects.toggle.has_value() && Contains(*rects.toggle, x, y)) {
    return hit(CompareHoverKind::HunkToggleReviewed, row->controls_hunk);
  }
  if (rects.comment.has_value() && Contains(*rects.comment, x, y)) {
    return hit(CompareHoverKind::HunkComment, row->controls_hunk);
  }
  return std::nullopt;
}

int CompareHoverRowAtY(const CompareControlGeometry& geometry, const CompareTabState& compare_tab, float y) {
  return CompareTabTakesReviewMarks(compare_tab) ? VisibleRowAt(geometry, y).value_or(-1) : -1;
}

int CompareHoveredHunk(const CompareTabState& compare_tab) {
  const int visual_index = compare_tab.scroll_row + compare_tab.review_hover_visible_row;
  if (compare_tab.review_hover_visible_row < 0 || visual_index < 0 ||
      static_cast<std::size_t>(visual_index) >= CompareTabVisualRowCount(compare_tab)) {
    return -1;
  }
  return std::max(-1, CompareHunkOfPresentationRow(
                          compare_tab, CompareVisualRowToPresentationRow(compare_tab, static_cast<std::size_t>(visual_index))));
}

}  // namespace microide::workspace
