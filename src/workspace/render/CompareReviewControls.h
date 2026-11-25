#pragma once

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

inline constexpr std::string_view kComparePreviousChangeLabel = "Previous Change";
inline constexpr std::string_view kCompareNextChangeLabel = "Next Change";
inline constexpr std::string_view kCompareHunkCommentLabel = "Comment";
inline constexpr std::string_view kCompareNoteEditLabel = "Edit";
inline constexpr std::string_view kCompareNoteDeleteLabel = "Delete";
std::string_view CompareNoteCaption(const CompareTabState& compare_tab, int note_hunk);

std::string_view CompareHunkToggleLabel(compare::BranchReviewMarkerStatus status);
std::string_view CompareHunkBadgeLabel(compare::BranchReviewMarkerStatus status);
std::string_view CompareFileReviewedLabel(bool file_reviewed);

bool CompareFileFullyReviewed(const CompareTabState& compare_tab);
compare::BranchReviewMarkerStatus CompareHunkReviewStatus(const CompareTabState& compare_tab, int hunk);
bool CompareHunkHasNote(const CompareTabState& compare_tab, int hunk);
bool CompareHunkShowsReviewButtons(const CompareTabState& compare_tab, int hunk);
int CompareHunkStartingAt(const CompareTabState& compare_tab, std::size_t presentation_row);
int CompareHunkControlsOnRow(const CompareTabState& compare_tab, const DiffWrapRow& wrap, int visible_row);
constexpr bool IsCompareHeaderControl(CompareHoverKind kind) {
  return kind == CompareHoverKind::HeaderPreviousChange || kind == CompareHoverKind::HeaderNextChange ||
         kind == CompareHoverKind::HeaderToggleFileReviewed;
}
int CompareHunkOfPresentationRow(const CompareTabState& compare_tab, std::size_t presentation_row);

struct CompareHeaderButtonRects {
  std::optional<SDL_FRect> file_reviewed;
  std::optional<SDL_FRect> previous;
  std::optional<SDL_FRect> next;
  float text_right_edge = 0.0f;
};
CompareHeaderButtonRects BuildCompareHeaderButtonRects(const render::TextRenderer& text_renderer,
                                                       const CompareControlGeometry& geometry,
                                                       const CompareTabState& compare_tab);

struct CompareHunkReviewRects {
  std::optional<SDL_FRect> badge;
  std::optional<SDL_FRect> toggle;
  std::optional<SDL_FRect> comment;
};
CompareHunkReviewRects BuildCompareHunkReviewRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   const CompareTabState& compare_tab,
                                                   int hunk,
                                                   int visible_row);

struct CompareNoteActionRects {
  SDL_FRect block{};
  SDL_FRect edit{};
  SDL_FRect remove{};
  float text_x = 0.0f;
  float text_right_edge = 0.0f;
};
CompareNoteActionRects BuildCompareNoteActionRects(const render::TextRenderer& text_renderer,
                                                   const CompareControlGeometry& geometry,
                                                   int visible_row);

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
  int hunk = -1;
};
struct CompareControlRow {
  std::size_t presentation_row = 0;
  const compare::ComparePresentationRow* row = nullptr;
  bool note = false;
  int controls_hunk = -1;
};
std::optional<CompareControlRow> CompareControlRowAt(const CompareTabState& compare_tab, int visible_row);

std::optional<CompareControlHit> CompareControlAt(const render::TextRenderer& text_renderer,
                                                  const CompareControlGeometry& geometry,
                                                  const CompareTabState& compare_tab,
                                                  float x,
                                                  float y);
int CompareHoverRowAtY(const CompareControlGeometry& geometry, const CompareTabState& compare_tab, float y);
int CompareHoveredHunk(const CompareTabState& compare_tab);

}
