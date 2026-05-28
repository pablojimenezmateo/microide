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
  float left_x = vm.rect.x + padding;
  for (const StatusBarSegmentViewModel& seg : vm.left_segments) {
    const float width = text_renderer_.MeasureWidth(seg.text);
    const SDL_FRect row = MakeRect(left_x, vm.rect.y, width, vm.rect.h);
    DrawVCenteredTextOn(text_renderer_, renderer, row, 0.0f, segment_color(seg),
                        theme_.chrome_background, seg.text);
    left_x += width + gap;
  }

  float right_x = vm.rect.x + vm.rect.w - padding;
  for (auto it = vm.right_segments.rbegin(); it != vm.right_segments.rend(); ++it) {
    const float width = text_renderer_.MeasureWidth(it->text);
    right_x -= width;
    const SDL_FRect row = MakeRect(right_x, vm.rect.y, width, vm.rect.h);
    DrawVCenteredTextOn(text_renderer_, renderer, row, 0.0f, segment_color(*it),
                        theme_.chrome_background, it->text);
    right_x -= gap;
  }
}

}  // namespace microide::workspace
