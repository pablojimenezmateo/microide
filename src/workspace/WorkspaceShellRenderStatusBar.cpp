#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/StatusBarService.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderStatusBar(SDL_Renderer* renderer,
                                     const WorkspaceLayout& layout) const {
  const StatusBarViewModel vm = RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  if (!vm.visible) {
    return;
  }

  DrawFilledRect(renderer, vm.rect, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(vm.rect.x, vm.rect.y, vm.rect.w, 1.0f), theme_.border);

  const float padding = 12.0f;
  const float gap = 14.0f;
  const auto segment_color = [&](const StatusBarSegmentViewModel& seg) {
    if (seg.clickable) {
      return theme_.accent;
    }
    switch (seg.id) {
      case StatusBarSegmentId::Project:
      case StatusBarSegmentId::Branch:
      case StatusBarSegmentId::LineColumn:
        return theme_.chrome_text;
      case StatusBarSegmentId::Problems:
        return seg.text.find("0") != std::string_view::npos ? theme_.chrome_text_secondary
                                                             : theme_.diagnostic_warning;
      case StatusBarSegmentId::Lsp:
        return seg.text.find("Ready") != std::string_view::npos ? theme_.chrome_text_secondary
                                                                 : theme_.diagnostic_info;
      case StatusBarSegmentId::Language:
      case StatusBarSegmentId::Indent:
      case StatusBarSegmentId::Encoding:
      case StatusBarSegmentId::LayoutMode:
      default:
        return theme_.chrome_text_secondary;
    }
  };
  // Clickable segments get a hover background so the pointer cursor (already returned by
  // CursorKindForPosition for these segments) is matched by a visible affordance.
  const auto draw_segment = [&](const StatusBarSegmentViewModel& seg, const SDL_FRect& row) {
    const bool hovered = seg.clickable && last_mouse_position_valid_ &&
                         Contains(row, last_mouse_x_, last_mouse_y_);
    SDL_Color text_bg = theme_.chrome_background;
    if (hovered) {
      const SDL_FRect hover_rect = MakeRect(row.x - 4.0f, row.y, row.w + 8.0f, row.h);
      DrawSelectableRowBackground(renderer, theme_, hover_rect, theme_.chrome_background, true);
      text_bg = theme_.row_highlight;
    }
    DrawVCenteredTextOn(text_renderer_, renderer, row, 0.0f,
                        hovered ? theme_.text_primary : segment_color(seg), text_bg, seg.text);
  };

  float left_x = vm.rect.x + padding;
  for (const StatusBarSegmentViewModel& seg : vm.left_segments) {
    const float width = text_renderer_.MeasureWidth(seg.text);
    draw_segment(seg, MakeRect(left_x, vm.rect.y, width, vm.rect.h));
    left_x += width + gap;
  }

  float right_x = vm.rect.x + vm.rect.w - padding;
  for (auto it = vm.right_segments.rbegin(); it != vm.right_segments.rend(); ++it) {
    const float width = text_renderer_.MeasureWidth(it->text);
    right_x -= width;
    draw_segment(*it, MakeRect(right_x, vm.rect.y, width, vm.rect.h));
    right_x -= gap;
  }
}

}  // namespace microide::workspace
