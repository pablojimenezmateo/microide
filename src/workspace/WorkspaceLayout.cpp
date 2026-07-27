#include "workspace/WorkspaceLayout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace microide::workspace {

namespace {

constexpr float kMenuBarHeight = 25.0f;
constexpr float kProjectTabStripHeight = 32.0f;
constexpr float kTabStripHeight = 34.0f;
constexpr float kHeaderHeight = kWorkspaceHeaderHeight;
constexpr float kDivider = kWorkspaceDividerThickness;
constexpr float kResizeHandleThickness = kWorkspaceResizeHandleThickness;
constexpr float kScrollbarThickness = kWorkspaceScrollbarThickness;
constexpr float kScrollbarInset = kWorkspaceScrollbarInset;
constexpr float kScrollbarMinThumbLength = kWorkspaceScrollbarMinThumbLength;
constexpr float kMaxSidebarWidth = kWorkspaceMaxSidebarWidth;
constexpr float kMinEditorAreaWidth = kWorkspaceMinEditorAreaWidth;
constexpr float kMinEditorAreaHeight = kWorkspaceMinEditorAreaHeight;
constexpr float kBottomPanelHeaderHeight = kWorkspaceBottomPanelHeaderHeight;
constexpr float kOverlayMinWidth = 520.0f;
constexpr float kOverlayMaxWidth = 840.0f;
constexpr float kOverlayMinHeight = 220.0f;
constexpr float kOverlayMaxHeight = kWorkspaceOverlayMaxHeight;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 216.0f;
constexpr float kPromptSurfaceInputHeight = 24.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
constexpr float kEditorSplitDividerThickness = kWorkspaceEditorSplitDividerThickness;

}  // namespace

SDL_FRect MakeRect(float x, float y, float w, float h) {
  return SDL_FRect{x, y, w, h};
}

WorkspaceLayout ComputeLayout(float window_width,
                              float window_height,
                              bool sidebar_visible,
                              bool bottom_panel_visible,
                              float sidebar_width,
                              float bottom_panel_height,
                              LayoutModeInputs layout_mode_inputs,
                              bool reserve_status_bar,
                              bool right_pane_visible,
                              float right_pane_width,
                              bool project_tab_strip_visible) {
  const LayoutMode layout_mode = ResolveLayoutMode(window_width, layout_mode_inputs);
  const float resolved_bottom_panel_height = bottom_panel_visible ? bottom_panel_height : 0.0f;
  const float resolved_sidebar_width = sidebar_visible ? sidebar_width : 0.0f;
  // The right debug pane is suppressed in compact layouts so the sidebar + pane
  // can't starve the editor below its minimum width.
  const bool right_pane_effective_visible =
      right_pane_visible && layout_mode != LayoutMode::Compact;
  const float resolved_right_pane_width =
      right_pane_effective_visible ? right_pane_width : 0.0f;
  const float status_bar_height =
      reserve_status_bar ? kWorkspaceStatusBarHeight : 0.0f;

  WorkspaceLayout layout;
  layout.layout_mode = layout_mode;
  layout.full = MakeRect(0.0f, 0.0f, window_width, window_height);
  layout.menu_bar = MakeRect(0.0f, 0.0f, window_width, kMenuBarHeight);
  // The strip collapses to zero height (but keeps its y) when hidden, so every render
  // and hit-test path that reads the rect suppresses naturally.
  const float project_tab_strip_height =
      project_tab_strip_visible ? kProjectTabStripHeight : 0.0f;
  layout.project_tab_strip =
      MakeRect(0.0f, kMenuBarHeight, window_width, project_tab_strip_height);
  layout.tab_strip =
      MakeRect(0.0f, kMenuBarHeight + project_tab_strip_height, window_width, kTabStripHeight);
  const float content_top = kMenuBarHeight + project_tab_strip_height + kTabStripHeight;
  const float content_bottom_reserved = resolved_bottom_panel_height + status_bar_height;
  layout.bottom_panel =
      MakeRect(0.0f, window_height - content_bottom_reserved, window_width,
               resolved_bottom_panel_height);
  layout.status_bar =
      reserve_status_bar
          ? MakeRect(0.0f, window_height - status_bar_height, window_width, status_bar_height)
          : MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  layout.content =
      MakeRect(0.0f, content_top, window_width,
               std::max(0.0f, window_height - content_top - content_bottom_reserved));
  layout.sidebar = MakeRect(0.0f, layout.content.y, resolved_sidebar_width, layout.content.h);
  const float editor_area_x = resolved_sidebar_width + (sidebar_visible ? kDivider : 0.0f);
  const float right_pane_reserve =
      resolved_right_pane_width + (right_pane_effective_visible ? kDivider : 0.0f);
  const float editor_area_width =
      std::max(0.0f, window_width - editor_area_x - right_pane_reserve);
  layout.editor_area = MakeRect(editor_area_x, layout.content.y, editor_area_width,
                                layout.content.h);
  layout.right_pane =
      right_pane_effective_visible
          ? MakeRect(layout.editor_area.x + layout.editor_area.w + kDivider, layout.content.y,
                     resolved_right_pane_width, layout.content.h)
          : MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  layout.breadcrumb =
      MakeRect(layout.editor_area.x, layout.editor_area.y, layout.editor_area.w, kHeaderHeight);
  layout.editor_surface =
      MakeRect(layout.editor_area.x, layout.editor_area.y + kHeaderHeight + kDivider,
               layout.editor_area.w, layout.editor_area.h - kHeaderHeight - kDivider);
  return layout;
}

std::optional<EditorSplitAxisLayout> ComputeEditorSplitAxisLayout(
    const SDL_FRect& rect,
    bool vertical,
    std::span<const float> size_fractions) {
  if (size_fractions.empty()) {
    return std::nullopt;
  }

  EditorSplitAxisLayout layout;
  layout.vertical = vertical;
  layout.divider_thickness = kEditorSplitDividerThickness;
  layout.extents.resize(size_fractions.size(), 0.0f);
  layout.child_rects.reserve(size_fractions.size());
  if (size_fractions.size() > 1) {
    layout.divider_rects.reserve(size_fractions.size() - 1);
  }

  layout.total_extent = std::max(
      0.0f, (vertical ? rect.w : rect.h) -
                kEditorSplitDividerThickness * static_cast<float>(size_fractions.size() - 1));
  layout.min_pane_extent = std::max(1.0f, std::floor(layout.total_extent * 0.12f));
  std::vector<float> weights(size_fractions.size(), 0.0f);
  float total_weight = 0.0f;
  for (std::size_t i = 0; i < size_fractions.size(); ++i) {
    weights[i] = std::max(0.0f, size_fractions[i]);
    total_weight += weights[i];
  }
  if (total_weight <= 0.0f) {
    std::fill(weights.begin(), weights.end(), 1.0f);
    total_weight = static_cast<float>(weights.size());
  }

  float cursor = vertical ? rect.x : rect.y;
  float remaining_extent = layout.total_extent;
  float remaining_weight = total_weight;
  for (std::size_t i = 0; i < weights.size(); ++i) {
    const std::size_t remaining_children = weights.size() - i;
    float child_extent = remaining_children == 1
                             ? remaining_extent
                             : std::floor(remaining_weight > 0.0f
                                              ? remaining_extent * (weights[i] / remaining_weight)
                                              : remaining_extent /
                                                    static_cast<float>(remaining_children));
    if (remaining_extent > layout.min_pane_extent * static_cast<float>(remaining_children)) {
      child_extent = std::clamp(
          child_extent, layout.min_pane_extent,
          remaining_extent -
              layout.min_pane_extent * static_cast<float>(remaining_children - 1));
    }

    layout.extents[i] = std::max(0.0f, child_extent);
    layout.child_rects.push_back(vertical ? MakeRect(cursor, rect.y, layout.extents[i], rect.h)
                                          : MakeRect(rect.x, cursor, rect.w, layout.extents[i]));

    cursor += layout.extents[i];
    remaining_extent = std::max(0.0f, remaining_extent - layout.extents[i]);
    remaining_weight = std::max(0.0f, remaining_weight - weights[i]);

    if (i + 1 < weights.size()) {
      layout.divider_rects.push_back(
          vertical ? MakeRect(cursor, rect.y, kEditorSplitDividerThickness, rect.h)
                   : MakeRect(rect.x, cursor, rect.w, kEditorSplitDividerThickness));
      cursor += kEditorSplitDividerThickness;
    }
  }

  return layout;
}

EditorGroupRectsLayout ComputeEditorGroupRects(const WorkspaceLayout& layout,
                                               std::size_t group_count,
                                               bool vertical_divider,
                                               float first_fraction) {
  EditorGroupRectsLayout result;
  result.vertical_divider = vertical_divider;
  const SDL_FRect ts = layout.tab_strip;
  const SDL_FRect bc = layout.breadcrumb;
  const SDL_FRect es = layout.editor_surface;
  const SDL_FRect ea = layout.editor_area;

  if (group_count < 2) {
    result.groups.push_back(EditorGroupRects{ts, bc, es});
    return result;
  }

  const float fraction = std::clamp(first_fraction, 0.1f, 0.9f);
  const float divider = kWorkspaceEditorSplitDividerThickness;

  if (vertical_divider) {
    // Side-by-side. Confine both groups' tab strips to the editor-area x-range so
    // they sit above their own surfaces; the strip background fill (full width)
    // is unchanged by the renderer.
    const float avail = std::max(0.0f, ea.w - divider);
    const float left_w = std::floor(avail * fraction);
    const float right_w = std::max(0.0f, avail - left_w);
    const float split_x = ea.x + left_w;
    const float right_x = split_x + divider;
    const auto col = [](const SDL_FRect& r, float x, float w) {
      return MakeRect(x, r.y, w, r.h);
    };
    result.groups.push_back(EditorGroupRects{
        col(ts, ea.x, left_w), col(bc, ea.x, left_w), col(es, ea.x, left_w)});
    result.groups.push_back(EditorGroupRects{
        col(ts, right_x, right_w), col(bc, right_x, right_w), col(es, right_x, right_w)});
    result.divider = MakeRect(split_x, ea.y, divider, ea.h);
    return result;
  }

  // Stacked. Group 0 keeps the top tab strip + breadcrumb and takes the top slice
  // of the editor surface; group 1 synthesizes a tab strip at the top of its
  // region with the remaining surface below.
  const float avail = std::max(0.0f, es.h - divider);
  const float top_h = std::floor(avail * fraction);
  const float bottom_h = std::max(0.0f, avail - top_h);
  result.groups.push_back(EditorGroupRects{ts, bc, MakeRect(es.x, es.y, es.w, top_h)});
  const float region_y = es.y + top_h + divider;
  const float strip_h = std::min(kTabStripHeight, bottom_h);
  result.groups.push_back(EditorGroupRects{
      MakeRect(es.x, region_y, es.w, strip_h),
      MakeRect(0.0f, 0.0f, 0.0f, 0.0f),
      MakeRect(es.x, region_y + strip_h, es.w, std::max(0.0f, bottom_h - strip_h))});
  result.divider = MakeRect(es.x, es.y + top_h, es.w, divider);
  return result;
}

bool Contains(const SDL_FRect& rect, float x, float y) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

std::optional<SDL_FRect> UnionOptionalRects(std::optional<SDL_FRect> lhs,
                                            const SDL_FRect& rhs) {
  if (!lhs.has_value()) {
    return rhs;
  }
  const float x = std::min(lhs->x, rhs.x);
  const float y = std::min(lhs->y, rhs.y);
  const float right = std::max(lhs->x + lhs->w, rhs.x + rhs.w);
  const float bottom = std::max(lhs->y + lhs->h, rhs.y + rhs.h);
  return MakeRect(x, y, right - x, bottom - y);
}

float ClampSidebarWidth(float width, float window_width) {
  const float viable_min_width =
      kWorkspaceSidebarHorizontalInset * 2.0f + kWorkspaceSidebarRowHeight + 1.0f;
  const float max_width = std::min(
      kMaxSidebarWidth,
      std::max(viable_min_width, window_width - kMinEditorAreaWidth - kDivider));
  return std::clamp(width, viable_min_width, max_width);
}

float ClampRightPaneWidth(float width, float window_width, float sidebar_width) {
  const float viable_min_width =
      kWorkspaceSidebarHorizontalInset * 2.0f + kWorkspaceSidebarRowHeight + 1.0f;
  const float sidebar_reserve = sidebar_width > 0.0f ? sidebar_width + kDivider : 0.0f;
  const float available = window_width - sidebar_reserve - kMinEditorAreaWidth - kDivider;
  const float max_width =
      std::min(kWorkspaceMaxRightPaneWidth, std::max(viable_min_width, available));
  return std::clamp(width, viable_min_width, max_width);
}

float ClampBottomPanelHeight(float height, float window_height) {
  const float content_height =
      std::max(0.0f, window_height - kMenuBarHeight - kProjectTabStripHeight - kTabStripHeight);
  const float viable_min_height = kBottomPanelHeaderHeight + kScrollbarThickness + 14.0f;
  const float min_height = std::min(viable_min_height, content_height);
  const float max_height = std::max(min_height, content_height - kMinEditorAreaHeight);
  return std::clamp(height, min_height, max_height);
}

int BottomPanelVisibleRowsForHeight(float panel_height, float line_height) {
  const float available_height = panel_height - kBottomPanelHeaderHeight - 18.0f;
  if (line_height <= 0.0f) {
    return 1;
  }
  return std::max(1, static_cast<int>(available_height / line_height));
}

int TailScrollRowForContent(std::size_t line_count, int visible_rows) {
  return std::max(0, static_cast<int>(line_count) - visible_rows);
}

int ClampScrollRowToContent(int scroll_row, std::size_t line_count, int visible_rows) {
  return std::clamp(scroll_row, 0, TailScrollRowForContent(line_count, visible_rows));
}

SDL_FRect SidebarResizeHandleRect(const WorkspaceLayout& layout) {
  if (layout.sidebar.w <= 0.0f || layout.sidebar.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(layout.sidebar.x + layout.sidebar.w - kResizeHandleThickness * 0.5f,
                  layout.sidebar.y, kResizeHandleThickness + kDivider, layout.sidebar.h);
}

SDL_FRect BottomPanelResizeHandleRect(const WorkspaceLayout& layout) {
  if (layout.bottom_panel.w <= 0.0f || layout.bottom_panel.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(layout.bottom_panel.x,
                  layout.bottom_panel.y - kResizeHandleThickness * 0.5f, layout.bottom_panel.w,
                  kResizeHandleThickness + kDivider);
}

SDL_FRect RightPaneResizeHandleRect(const WorkspaceLayout& layout) {
  if (layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(layout.right_pane.x - kResizeHandleThickness * 0.5f - kDivider,
                  layout.right_pane.y, kResizeHandleThickness + kDivider, layout.right_pane.h);
}

SDL_FRect SidebarResizeCursorRect(const WorkspaceLayout& layout) {
  const SDL_FRect visual = SidebarResizeHandleRect(layout);
  if (visual.w <= 0.0f || visual.h <= 0.0f) {
    return visual;
  }
  return MakeRect(visual.x - kWorkspaceResizeHandleCursorInflate, visual.y,
                  visual.w + kWorkspaceResizeHandleCursorInflate * 2.0f, visual.h);
}

SDL_FRect BottomPanelResizeCursorRect(const WorkspaceLayout& layout) {
  const SDL_FRect visual = BottomPanelResizeHandleRect(layout);
  if (visual.w <= 0.0f || visual.h <= 0.0f) {
    return visual;
  }
  return MakeRect(visual.x, visual.y - kWorkspaceResizeHandleCursorInflate, visual.w,
                  visual.h + kWorkspaceResizeHandleCursorInflate * 2.0f);
}

SDL_FRect RightPaneResizeCursorRect(const WorkspaceLayout& layout) {
  const SDL_FRect visual = RightPaneResizeHandleRect(layout);
  if (visual.w <= 0.0f || visual.h <= 0.0f) {
    return visual;
  }
  return MakeRect(visual.x - kWorkspaceResizeHandleCursorInflate, visual.y,
                  visual.w + kWorkspaceResizeHandleCursorInflate * 2.0f, visual.h);
}

// The grab (hit) region is intentionally identical to the cursor-change region:
// the user only ever sees a resize cursor inside *ResizeCursorRect, so the drag
// must start in exactly that span and never extend past it. (Earlier these
// inflated wider than the cursor zone to compensate for the cursor failing to
// refresh on idle Wayland compositors; that bug is fixed, so the padding is gone
// and the two can no longer drift.)
SDL_FRect SidebarResizeHitRect(const WorkspaceLayout& layout) {
  return SidebarResizeCursorRect(layout);
}

SDL_FRect RightPaneResizeHitRect(const WorkspaceLayout& layout) {
  return RightPaneResizeCursorRect(layout);
}

SDL_FRect BottomPanelResizeHitRect(const WorkspaceLayout& layout) {
  return BottomPanelResizeCursorRect(layout);
}

SDL_FRect WindowControlButtonHitRect(const SDL_FRect& button_rect) {
  if (button_rect.w <= 0.0f || button_rect.h <= 0.0f) {
    return button_rect;
  }
  return MakeRect(button_rect.x - kWorkspaceWindowControlButtonHitInflate,
                  button_rect.y - kWorkspaceWindowControlButtonHitInflate,
                  button_rect.w + kWorkspaceWindowControlButtonHitInflate * 2.0f,
                  button_rect.h + kWorkspaceWindowControlButtonHitInflate * 2.0f);
}

SDL_FRect VerticalScrollbarHitRect(const ScrollbarGeometry& geometry) {
  if (geometry.track.w <= 0.0f || geometry.track.h <= 0.0f) {
    return geometry.track;
  }
  return MakeRect(geometry.track.x - kWorkspaceScrollbarHitInflate, geometry.track.y,
                  geometry.track.w + kWorkspaceScrollbarHitInflate * 2.0f, geometry.track.h);
}

SDL_FRect HorizontalScrollbarHitRect(const ScrollbarGeometry& geometry) {
  if (geometry.track.w <= 0.0f || geometry.track.h <= 0.0f) {
    return geometry.track;
  }
  return MakeRect(geometry.track.x, geometry.track.y - kWorkspaceScrollbarHitInflate,
                  geometry.track.w, geometry.track.h + kWorkspaceScrollbarHitInflate * 2.0f);
}

SDL_FRect TabCloseHitRect(const SDL_FRect& close_visual_rect, const SDL_FRect& tab_rect) {
  if (close_visual_rect.w <= 0.0f || close_visual_rect.h <= 0.0f) {
    return close_visual_rect;
  }
  const float inflate = kWorkspaceTabCloseHitInflate;
  const float left = std::max(tab_rect.x, close_visual_rect.x - inflate);
  const float top = std::max(tab_rect.y, close_visual_rect.y - inflate);
  const float right = std::min(tab_rect.x + tab_rect.w, close_visual_rect.x + close_visual_rect.w + inflate);
  const float bottom = std::min(tab_rect.y + tab_rect.h, close_visual_rect.y + close_visual_rect.h + inflate);
  return MakeRect(left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top));
}

SDL_FRect EmptyTabStripPlaceholderRect(const SDL_FRect& tab_strip) {
  return MakeRect(tab_strip.x, tab_strip.y + 2.0f, 220.0f, std::max(22.0f, tab_strip.h - 2.0f));
}

std::optional<std::size_t> BottomPanelLineIndexAtY(float text_y,
                                                   float line_height,
                                                   int visible_rows,
                                                   int vertical_scroll,
                                                   float y,
                                                   std::size_t line_count) {
  if (y < text_y || line_height <= 0.0f) {
    return std::nullopt;
  }
  const int row = static_cast<int>(std::floor((y - text_y) / line_height));
  if (row < 0 || row >= visible_rows) {
    return std::nullopt;
  }
  const int absolute_index = vertical_scroll + row;
  if (absolute_index < 0 || absolute_index >= static_cast<int>(line_count)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(absolute_index);
}

SDL_FRect ComputeMenuOverflowPopupRect(const SDL_FRect& chevron_rect, std::size_t item_count) {
  const float resolved_count = std::max<std::size_t>(1, item_count);
  const float height = 8.0f + kWorkspaceMenuPopupItemHeight * static_cast<float>(resolved_count);
  const float width = std::max(160.0f, chevron_rect.w + 132.0f);
  return MakeRect(chevron_rect.x + chevron_rect.w - width, chevron_rect.y + chevron_rect.h, width,
                  height);
}

LayoutMode ResolveLayoutMode(float window_width, const LayoutModeInputs& inputs) {
  if (inputs.user_override == LayoutModeInputs::Override::Regular) {
    return LayoutMode::Regular;
  }
  if (inputs.user_override == LayoutModeInputs::Override::Compact) {
    return LayoutMode::Compact;
  }
  const float upper = inputs.compact_breakpoint_px + kWorkspaceLayoutCompactHysteresis;
  const float lower = inputs.compact_breakpoint_px - kWorkspaceLayoutCompactHysteresis;
  if (inputs.previous_mode == LayoutMode::Compact) {
    return window_width >= upper ? LayoutMode::Regular : LayoutMode::Compact;
  }
  return window_width <= lower ? LayoutMode::Compact : LayoutMode::Regular;
}

SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout) {
  return MakeRect(layout.bottom_panel.x, layout.bottom_panel.y + kBottomPanelHeaderHeight,
                  layout.bottom_panel.w,
                  std::max(0.0f, layout.bottom_panel.h - kBottomPanelHeaderHeight));
}

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width = kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputeEditorBannerStripRect(const SDL_FRect& editor_surface) {
  const float height = std::min(kWorkspaceEditorBannerHeight, editor_surface.h);
  return MakeRect(editor_surface.x, editor_surface.y, editor_surface.w, height);
}

EditorBannerButtonLayout ComputeEditorBannerButtonRects(const SDL_FRect& strip, bool has_actions) {
  constexpr float kButtonHeight = 20.0f;
  constexpr float kButtonGap = 6.0f;
  constexpr float kEdgePad = 8.0f;
  constexpr float kDismissSize = 18.0f;
  const float y = strip.y + (strip.h - kButtonHeight) * 0.5f;

  EditorBannerButtonLayout layout;
  layout.dismiss = MakeRect(strip.x + strip.w - kEdgePad - kDismissSize,
                            strip.y + (strip.h - kDismissSize) * 0.5f, kDismissSize, kDismissSize);
  if (!has_actions) {
    return layout;
  }

  constexpr float kReloadWidth = 64.0f;
  constexpr float kOverwriteWidth = 78.0f;
  constexpr float kKeepWidth = 54.0f;
  float x = layout.dismiss.x - kButtonGap - kKeepWidth;
  layout.keep = MakeRect(x, y, kKeepWidth, kButtonHeight);
  x -= kButtonGap + kOverwriteWidth;
  layout.overwrite = MakeRect(x, y, kOverwriteWidth, kButtonHeight);
  x -= kButtonGap + kReloadWidth;
  layout.reload = MakeRect(x, y, kReloadWidth, kButtonHeight);
  return layout;
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::vector<SDL_FRect> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog, int button_count) {
  const int resolved_count = std::max(1, button_count);
  const float total_width = kPromptSurfaceButtonWidth * static_cast<float>(resolved_count) +
                            kPromptSurfaceButtonGap * static_cast<float>(resolved_count - 1);
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  std::vector<SDL_FRect> rects;
  rects.reserve(static_cast<std::size_t>(resolved_count));
  for (int i = 0; i < resolved_count; ++i) {
    rects.push_back(MakeRect(start_x + static_cast<float>(i) *
                                           (kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap),
                             y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight));
  }
  return rects;
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f,
                  kPromptSurfaceInputHeight);
}

TextGridInteractionLayout ComputeTextGridInteractionLayout(const SDL_FRect& rect,
                                                           float text_x,
                                                           float first_line_y,
                                                           float line_height,
                                                           float char_width,
                                                           std::size_t scroll_line,
                                                           std::size_t line_count,
                                                           std::size_t horizontal_scroll,
                                                           std::size_t visible_rows,
                                                           std::size_t visible_columns) {
  TextGridInteractionLayout layout;
  layout.rect = rect;
  layout.text_x = text_x;
  layout.first_line_y = first_line_y;
  layout.line_height = std::max(1.0f, line_height);
  layout.char_width = std::max(1.0f, char_width);
  layout.line_count = line_count;
  layout.visible_rows = std::max<std::size_t>(1, visible_rows);
  layout.visible_columns = std::max<std::size_t>(1, visible_columns);
  const std::size_t max_scroll_line =
      line_count > layout.visible_rows ? line_count - layout.visible_rows : 0;
  layout.scroll_line = std::min(scroll_line, max_scroll_line);
  layout.horizontal_scroll = horizontal_scroll;
  return layout;
}

std::optional<std::size_t> VisibleTextGridLineAtY(const TextGridInteractionLayout& layout,
                                                  float y) {
  if (layout.line_count == 0 || layout.line_height <= 0.0f || y < layout.first_line_y) {
    return std::nullopt;
  }

  const std::size_t row = static_cast<std::size_t>(
      std::floor((y - layout.first_line_y) / layout.line_height));
  if (row >= layout.visible_rows) {
    return std::nullopt;
  }

  const std::size_t line = layout.scroll_line + row;
  if (line >= layout.line_count) {
    return std::nullopt;
  }
  return line;
}

std::size_t ClampTextGridLineAtY(const TextGridInteractionLayout& layout, float y) {
  if (layout.line_count == 0) {
    return 0;
  }

  const float local_y = std::max(0.0f, y - layout.first_line_y);
  const std::size_t row =
      static_cast<std::size_t>(std::floor(local_y / std::max(1.0f, layout.line_height)));
  return std::min(layout.scroll_line + row, layout.line_count - 1);
}

std::size_t TextGridVisualColumnAtX(const TextGridInteractionLayout& layout, float x) {
  const float local_x = std::max(0.0f, x - layout.text_x);
  return layout.horizontal_scroll +
         static_cast<std::size_t>(
             std::max(0L, std::lround(local_x / std::max(1.0f, layout.char_width))));
}

float TextGridCursorX(const TextGridInteractionLayout& layout, std::size_t visual_column) {
  const std::size_t visible_column =
      visual_column > layout.horizontal_scroll ? visual_column - layout.horizontal_scroll : 0;
  return layout.text_x + static_cast<float>(visible_column) * layout.char_width;
}

float TextGridLineY(const TextGridInteractionLayout& layout, std::size_t line_index) {
  const long long relative_line = static_cast<long long>(line_index) -
                                  static_cast<long long>(layout.scroll_line);
  return layout.first_line_y + static_cast<float>(relative_line) * layout.line_height;
}

ScrollSurfaceLayout ComputeScrollSurfaceLayout(const SDL_FRect& area,
                                               std::size_t total_rows,
                                               int visible_rows,
                                               int requested_vertical_scroll,
                                               std::size_t total_columns,
                                               std::size_t visible_columns,
                                               std::size_t requested_horizontal_scroll) {
  ScrollSurfaceLayout layout;
  layout.visible_rows = std::max(1, visible_rows);
  layout.max_vertical_scroll = TailScrollRowForContent(total_rows, layout.visible_rows);
  layout.vertical_scroll =
      ClampScrollRowToContent(requested_vertical_scroll, total_rows, layout.visible_rows);
  layout.visible_columns = std::max<std::size_t>(1, visible_columns);
  layout.max_horizontal_scroll =
      total_columns > layout.visible_columns ? total_columns - layout.visible_columns : 0;
  layout.horizontal_scroll = std::min(requested_horizontal_scroll, layout.max_horizontal_scroll);
  layout.show_vertical = layout.max_vertical_scroll > 0;
  layout.show_horizontal = layout.max_horizontal_scroll > 0;

  const float reserved_width =
      layout.show_vertical ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  const float reserved_height =
      layout.show_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  layout.content_rect =
      MakeRect(area.x, area.y, std::max(0.0f, area.w - reserved_width),
               std::max(0.0f, area.h - reserved_height));
  layout.vertical_scrollbar = MakeVerticalScrollbarGeometry(
      area, static_cast<float>(total_rows), static_cast<float>(layout.visible_rows),
      static_cast<float>(layout.vertical_scroll), layout.show_horizontal);
  layout.horizontal_scrollbar = MakeHorizontalScrollbarGeometry(
      area, static_cast<float>(total_columns), static_cast<float>(layout.visible_columns),
      static_cast<float>(layout.horizontal_scroll), layout.show_vertical);
  return layout;
}

ScrollableListLayout ComputeScrollableListLayout(const SDL_FRect& container,
                                                 float list_y,
                                                 std::size_t item_count,
                                                 int requested_scroll_row,
                                                 float horizontal_inset,
                                                 float row_step,
                                                 float row_height,
                                                 float list_bottom_padding,
                                                 float scrollbar_bottom_padding,
                                                 bool fractional_visible_units) {
  ScrollableListLayout layout;
  layout.row_x = container.x + horizontal_inset;
  layout.row_y = list_y;
  layout.row_step = row_step;
  layout.row_height = row_height;

  const float available_height =
      std::max(0.0f, container.y + container.h - list_y - list_bottom_padding);
  const float raw_visible_units = row_step > 0.0f ? available_height / row_step : 0.0f;
  layout.visible_units =
      fractional_visible_units
          ? std::max(1.0f, raw_visible_units)
          : static_cast<float>(
                std::max(1, static_cast<int>(std::floor(std::max(0.0f, raw_visible_units)))));
  layout.visible_rows =
      std::max(1, static_cast<int>(std::floor(std::max(1.0f, layout.visible_units))));
  layout.max_scroll = std::max(
      0, static_cast<int>(std::ceil(static_cast<float>(item_count) - layout.visible_units)));
  layout.scroll_row = std::clamp(requested_scroll_row, 0, layout.max_scroll);
  layout.row_width =
      std::max(0.0f, container.w - horizontal_inset * 2.0f -
                         (layout.max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
  layout.list_rect =
      MakeRect(container.x, list_y, container.w,
               std::max(0.0f, container.y + container.h - list_y - scrollbar_bottom_padding));
  layout.scrollbar =
      MakeVerticalScrollbarGeometry(layout.list_rect, static_cast<float>(item_count),
                                    layout.visible_units, static_cast<float>(layout.scroll_row));
  return layout;
}

int RevealScrollableListIndex(const ScrollableListLayout& layout, int selected_index) {
  if (selected_index < 0) {
    return layout.scroll_row;
  }

  int scroll_row = layout.scroll_row;
  if (selected_index < scroll_row) {
    scroll_row = selected_index;
  } else if (selected_index >= scroll_row + layout.visible_rows) {
    scroll_row = selected_index - layout.visible_rows + 1;
  }
  return std::clamp(scroll_row, 0, layout.max_scroll);
}

std::optional<int> ScrollableListIndexAtY(const ScrollableListLayout& layout, float y) {
  if (layout.row_step <= 0.0f || y < layout.row_y ||
      y >= layout.row_y + static_cast<float>(layout.visible_rows) * layout.row_step) {
    return std::nullopt;
  }

  const int row = static_cast<int>(std::floor((y - layout.row_y) / layout.row_step));
  return layout.scroll_row + row;
}

SDL_FRect ScrollableListRowRect(const ScrollableListLayout& layout, int visible_row) {
  return MakeRect(layout.row_x, layout.row_y + static_cast<float>(visible_row) * layout.row_step,
                  layout.row_width, layout.row_height);
}

std::optional<SDL_FRect> ComputeScrollbarThumb(const SDL_FRect& track,
                                               float total_units,
                                               float visible_units,
                                               float scroll_units,
                                               bool vertical) {
  if (track.w <= 0.0f || track.h <= 0.0f || total_units <= visible_units ||
      visible_units <= 0.0f) {
    return std::nullopt;
  }

  const float track_length = vertical ? track.h : track.w;
  if (track_length <= 0.0f) {
    return std::nullopt;
  }

  const float max_scroll = std::max(0.0f, total_units - visible_units);
  const float clamped_scroll = std::clamp(scroll_units, 0.0f, max_scroll);
  const float thumb_length = std::clamp(track_length * (visible_units / total_units),
                                        kScrollbarMinThumbLength, track_length);
  const float travel = std::max(0.0f, track_length - thumb_length);
  const float offset =
      (travel <= 0.0f || max_scroll <= 0.0f) ? 0.0f : (clamped_scroll / max_scroll) * travel;

  if (vertical) {
    return MakeRect(track.x, track.y + offset, track.w, thumb_length);
  }
  return MakeRect(track.x + offset, track.y, thumb_length, track.h);
}

std::optional<ScrollbarGeometry> MakeVerticalScrollbarGeometry(const SDL_FRect& area,
                                                              float total_units,
                                                              float visible_units,
                                                              float scroll_units,
                                                              bool reserve_horizontal) {
  const SDL_FRect track = MakeRect(
      area.x + area.w - kScrollbarThickness - kScrollbarInset, area.y + kScrollbarInset,
      kScrollbarThickness,
      std::max(0.0f, area.h - kScrollbarInset * 2.0f -
                           (reserve_horizontal ? kScrollbarThickness + kScrollbarInset : 0.0f)));
  const auto thumb = ComputeScrollbarThumb(track, total_units, visible_units, scroll_units, true);
  if (!thumb.has_value()) {
    return std::nullopt;
  }
  return ScrollbarGeometry{
      .track = track,
      .thumb = *thumb,
      .total_units = total_units,
      .visible_units = visible_units,
      .scroll_units = scroll_units,
      .vertical = true,
  };
}

std::optional<ScrollbarGeometry> MakeHorizontalScrollbarGeometry(const SDL_FRect& area,
                                                                float total_units,
                                                                float visible_units,
                                                                float scroll_units,
                                                                bool reserve_vertical) {
  const SDL_FRect track = MakeRect(
      area.x + kScrollbarInset, area.y + area.h - kScrollbarThickness - kScrollbarInset,
      std::max(0.0f, area.w - kScrollbarInset * 2.0f -
                           (reserve_vertical ? kScrollbarThickness + kScrollbarInset : 0.0f)),
      kScrollbarThickness);
  const auto thumb = ComputeScrollbarThumb(track, total_units, visible_units, scroll_units, false);
  if (!thumb.has_value()) {
    return std::nullopt;
  }
  return ScrollbarGeometry{
      .track = track,
      .thumb = *thumb,
      .total_units = total_units,
      .visible_units = visible_units,
      .scroll_units = scroll_units,
      .vertical = false,
  };
}

float ScrollUnitsForPointer(const ScrollbarGeometry& geometry,
                            float pointer_coordinate,
                            float grab_offset) {
  const float track_start = geometry.vertical ? geometry.track.y : geometry.track.x;
  const float track_length = geometry.vertical ? geometry.track.h : geometry.track.w;
  const float thumb_length = geometry.vertical ? geometry.thumb.h : geometry.thumb.w;
  const float max_scroll = std::max(0.0f, geometry.total_units - geometry.visible_units);
  const float travel = std::max(0.0f, track_length - thumb_length);
  if (travel <= 0.0f || max_scroll <= 0.0f) {
    return 0.0f;
  }

  const float thumb_start = std::clamp(pointer_coordinate - grab_offset, track_start,
                                       track_start + travel);
  return ((thumb_start - track_start) / travel) * max_scroll;
}

std::vector<CompareScrollbarRun> BuildCompareScrollbarRuns(
    const compare::ComparePresentationModel& presentation,
    const compare::CompareModel& model) {
  std::vector<CompareScrollbarRun> runs;
  if (presentation.rows.empty() || model.rows.empty()) {
    return runs;
  }

  auto push_run = [&](int start_row, int end_row, compare::CompareRowKind kind) {
    if (kind == compare::CompareRowKind::Unchanged || start_row < 0 || end_row <= start_row) {
      return;
    }
    runs.push_back(CompareScrollbarRun{.kind = kind, .start_row = start_row, .end_row = end_row});
  };

  int run_start = -1;
  compare::CompareRowKind run_kind = compare::CompareRowKind::Unchanged;
  for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
    const compare::ComparePresentationRow& presentation_row = presentation.rows[i];
    if (presentation_row.kind != compare::ComparePresentationRowKind::Model ||
        presentation_row.model_row_index >= model.rows.size()) {
      push_run(run_start, static_cast<int>(i), run_kind);
      run_start = -1;
      run_kind = compare::CompareRowKind::Unchanged;
      continue;
    }

    const compare::CompareRowKind kind = model.rows[presentation_row.model_row_index].kind;
    if (kind == compare::CompareRowKind::Unchanged) {
      push_run(run_start, static_cast<int>(i), run_kind);
      run_start = -1;
      run_kind = compare::CompareRowKind::Unchanged;
      continue;
    }

    if (run_start >= 0 && kind == run_kind) {
      continue;
    }

    push_run(run_start, static_cast<int>(i), run_kind);
    run_start = static_cast<int>(i);
    run_kind = kind;
  }
  push_run(run_start, static_cast<int>(presentation.rows.size()), run_kind);
  return runs;
}

namespace {
// Shared geometry for the centered modal overlays. The three public helpers differ
// only by their width/height fractions, vertical-placement fraction, and minimum
// padded width/height, so they forward into one body to keep them in lockstep.
struct OverlaySurfaceParams {
  float width_frac;
  float height_frac;
  float vertical_frac;
  float min_width;
  float min_height;
};

SDL_FRect ComputeOverlaySurfaceRectImpl(const SDL_FRect& editor_area,
                                        const OverlaySurfaceParams& params) {
  const float overlay_width =
      std::clamp(editor_area.w * params.width_frac, kOverlayMinWidth, kOverlayMaxWidth);
  const float overlay_height =
      std::clamp(editor_area.h * params.height_frac, kOverlayMinHeight, kOverlayMaxHeight);
  const float final_width =
      std::min(overlay_width, std::max(params.min_width, editor_area.w - 56.0f));
  const float final_height =
      std::min(overlay_height, std::max(params.min_height, editor_area.h - 48.0f));
  return MakeRect(editor_area.x + (editor_area.w - final_width) * 0.5f,
                  editor_area.y + (editor_area.h - final_height) * params.vertical_frac,
                  final_width, final_height);
}
}  // namespace

SDL_FRect ComputeOverlaySurfaceRect(const SDL_FRect& editor_area) {
  return ComputeOverlaySurfaceRectImpl(editor_area, {0.58f, 0.44f, 0.22f, 260.0f, 160.0f});
}

SDL_FRect ComputePickerOverlaySurfaceRect(const SDL_FRect& editor_area) {
  // The ref/commit picker is a denser, taller modal than the search overlays: it
  // carries a header block plus two-column rows, so it gets more width and height.
  return ComputeOverlaySurfaceRectImpl(editor_area, {0.66f, 0.60f, 0.18f, 260.0f, 160.0f});
}

SDL_FRect ComputeSettingsOverlaySurfaceRect(const SDL_FRect& editor_area) {
  // The settings surface is a two-pane editor (category list + value pane), so it
  // is the largest modal: wide enough for a left rail plus roomy controls, tall
  // enough to show many rows without constant scrolling.
  return ComputeOverlaySurfaceRectImpl(editor_area, {0.72f, 0.66f, 0.14f, 420.0f, 220.0f});
}

namespace {
constexpr float kFindWidgetMargin = 12.0f;
constexpr float kFindWidgetPad = 8.0f;
constexpr float kFindWidgetRowHeight = 24.0f;
constexpr float kFindWidgetRowGap = 6.0f;
constexpr float kFindWidgetButton = 22.0f;
constexpr float kFindWidgetButtonGap = 4.0f;
constexpr float kFindWidgetCountWidth = 56.0f;
constexpr float kFindWidgetReplaceButtonWidth = 70.0f;
constexpr float kFindWidgetReplaceAllWidth = 40.0f;
constexpr float kFindWidgetMinWidth = 280.0f;
constexpr float kFindWidgetMaxWidth = 460.0f;

constexpr float kDebugToolbarMargin = 12.0f;
constexpr float kDebugToolbarPad = 6.0f;
constexpr float kDebugToolbarButton = 26.0f;
constexpr float kDebugToolbarButtonGap = 4.0f;
constexpr float kDebugToolbarStackGap = 8.0f;
}  // namespace

SDL_FRect ComputeFindWidgetRect(const SDL_FRect& editor_area, bool replace_mode) {
  const float available =
      std::max(kFindWidgetMinWidth, editor_area.w - 2.0f * kFindWidgetMargin);
  const float width = std::min(kFindWidgetMaxWidth, available);
  const float rows = replace_mode ? 2.0f : 1.0f;
  const float height = 2.0f * kFindWidgetPad + rows * kFindWidgetRowHeight +
                       (rows - 1.0f) * kFindWidgetRowGap;
  const float x = editor_area.x + editor_area.w - width - kFindWidgetMargin;
  const float y = editor_area.y + kFindWidgetMargin;
  return MakeRect(std::floor(x), std::floor(y), std::floor(width), std::floor(height));
}

FindWidgetLayout ComputeFindWidgetLayout(const SDL_FRect& editor_area,
                                         bool replace_mode,
                                         std::size_t toggle_count) {
  FindWidgetLayout layout;
  layout.replace_mode = replace_mode;
  layout.toggle_count = std::min(toggle_count, kFindWidgetMaxToggles);
  const SDL_FRect r = ComputeFindWidgetRect(editor_area, replace_mode);
  layout.widget = r;

  const float field_x = r.x + kFindWidgetPad;
  const float row1_y = r.y + kFindWidgetPad;
  // Row 1, right-aligned: [count] [prev] [next] [close]; the search field fills
  // whatever width is left on the left.
  layout.close_button =
      MakeRect(r.x + r.w - kFindWidgetPad - kFindWidgetButton, row1_y, kFindWidgetButton,
               kFindWidgetRowHeight);
  layout.next_button = MakeRect(layout.close_button.x - kFindWidgetButtonGap - kFindWidgetButton,
                                row1_y, kFindWidgetButton, kFindWidgetRowHeight);
  layout.prev_button = MakeRect(layout.next_button.x - kFindWidgetButtonGap - kFindWidgetButton,
                                row1_y, kFindWidgetButton, kFindWidgetRowHeight);
  const float count_x = layout.prev_button.x - kFindWidgetButtonGap - kFindWidgetCountWidth;
  layout.count_rect = MakeRect(count_x, row1_y, kFindWidgetCountWidth, kFindWidgetRowHeight);
  // Mode toggles sit between the search field and the match count, filled from the
  // right so slot 0 stays adjacent to the field however many there are.
  float toggles_x = count_x;
  for (std::size_t index = layout.toggle_count; index-- > 0;) {
    toggles_x -= kFindWidgetButtonGap + kFindWidgetButton;
    layout.toggle_buttons[index] =
        MakeRect(toggles_x, row1_y, kFindWidgetButton, kFindWidgetRowHeight);
  }
  layout.search_field = MakeRect(field_x, row1_y,
                                 std::max(0.0f, toggles_x - kFindWidgetButtonGap - field_x),
                                 kFindWidgetRowHeight);

  if (replace_mode) {
    const float row2_y = row1_y + kFindWidgetRowHeight + kFindWidgetRowGap;
    layout.replace_all_button =
        MakeRect(r.x + r.w - kFindWidgetPad - kFindWidgetReplaceAllWidth, row2_y,
                 kFindWidgetReplaceAllWidth, kFindWidgetRowHeight);
    layout.replace_button =
        MakeRect(layout.replace_all_button.x - kFindWidgetButtonGap - kFindWidgetReplaceButtonWidth,
                 row2_y, kFindWidgetReplaceButtonWidth, kFindWidgetRowHeight);
    layout.replace_field =
        MakeRect(field_x, row2_y,
                 std::max(0.0f, layout.replace_button.x - kFindWidgetButtonGap - field_x),
                 kFindWidgetRowHeight);
  }
  return layout;
}

DebugToolbarLayout ComputeDebugToolbarLayout(const SDL_FRect& editor_area,
                                             std::optional<float> avoid_below_y,
                                             bool include_reverse) {
  DebugToolbarLayout layout;
  // The active button order: Continue/Pause, the forward steps, optionally the
  // reverse pair, then Restart and Stop. Skipping the reverse pair yields a layout
  // byte-for-byte identical to the pre-reverse bar.
  layout.kinds[layout.button_count++] = DebugToolbarButton::ContinuePause;
  layout.kinds[layout.button_count++] = DebugToolbarButton::StepOver;
  layout.kinds[layout.button_count++] = DebugToolbarButton::StepInto;
  layout.kinds[layout.button_count++] = DebugToolbarButton::StepOut;
  if (include_reverse) {
    layout.kinds[layout.button_count++] = DebugToolbarButton::ReverseContinue;
    layout.kinds[layout.button_count++] = DebugToolbarButton::StepBack;
  }
  layout.kinds[layout.button_count++] = DebugToolbarButton::Restart;
  layout.kinds[layout.button_count++] = DebugToolbarButton::Stop;

  const auto count = static_cast<float>(layout.button_count);
  const float width = 2.0f * kDebugToolbarPad + count * kDebugToolbarButton +
                      (count - 1.0f) * kDebugToolbarButtonGap;
  const float height = 2.0f * kDebugToolbarPad + kDebugToolbarButton;
  const float x = editor_area.x + editor_area.w - width - kDebugToolbarMargin;
  // Stack below the find widget when present, otherwise anchor at the editor top.
  const float y = avoid_below_y.has_value() ? *avoid_below_y + kDebugToolbarStackGap
                                            : editor_area.y + kDebugToolbarMargin;
  layout.widget = MakeRect(std::floor(x), std::floor(y), std::floor(width), std::floor(height));

  float bx = layout.widget.x + kDebugToolbarPad;
  const float by = layout.widget.y + kDebugToolbarPad;
  for (std::size_t i = 0; i < layout.button_count; ++i) {
    layout.buttons[i] = MakeRect(bx, by, kDebugToolbarButton, kDebugToolbarButton);
    bx += kDebugToolbarButton + kDebugToolbarButtonGap;
  }
  return layout;
}

bool operator==(const WorkspaceLayout& lhs, const WorkspaceLayout& rhs) noexcept {
  auto rect_eq = [](const SDL_FRect& a, const SDL_FRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  };
  return lhs.layout_mode == rhs.layout_mode && rect_eq(lhs.full, rhs.full) &&
         rect_eq(lhs.menu_bar, rhs.menu_bar) && rect_eq(lhs.project_tab_strip, rhs.project_tab_strip) &&
         rect_eq(lhs.tab_strip, rhs.tab_strip) && rect_eq(lhs.bottom_panel, rhs.bottom_panel) &&
         rect_eq(lhs.content, rhs.content) && rect_eq(lhs.sidebar, rhs.sidebar) &&
         rect_eq(lhs.editor_area, rhs.editor_area) && rect_eq(lhs.right_pane, rhs.right_pane) &&
         rect_eq(lhs.breadcrumb, rhs.breadcrumb) &&
         rect_eq(lhs.editor_surface, rhs.editor_surface) && rect_eq(lhs.status_bar, rhs.status_bar);
}

}  // namespace microide::workspace
