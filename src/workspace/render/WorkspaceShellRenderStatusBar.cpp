#include "workspace/shell/WorkspaceShell.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "render/ScopedRenderClip.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/services/StatusBarService.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace microide::workspace {

using namespace detail;

namespace {

SDL_FRect StatusBarHoverRect(const SDL_FRect& row) {
  return MakeRect(row.x - 4.0f, row.y, row.w + 8.0f, row.h);
}

}

void WorkspaceShell::PrepareStatusBarFrame(const WorkspaceLayout& layout) {
  PreparedFrameState& frame = frame_state_;
  frame.status_bar = RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  frame.status_bar_segments.clear();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::StatusBar);
  if (frame.status_bar.visible) {
    units.push_back(DerivedDamage::Unit{
        .rect = frame.status_bar.rect,
        .fingerprint = PaintFingerprint()
                           .Add(frame.status_bar.rect)
                           .Add(theme_.chrome_background)
                           .Add(theme_.border)
                           .value()});
    const auto segment_color = [&](const StatusBarSegmentViewModel& seg) -> SDL_Color {
      switch (seg.id) {
        case StatusBarSegmentId::Problems:
          return StatusToneColor(theme_, seg.tone, theme_.diagnostic_warning);
        case StatusBarSegmentId::Lsp:
          return StatusToneColor(theme_, seg.tone, theme_.chrome_text_secondary);
        default:
          break;
      }
      if (seg.clickable) {
        return theme_.accent;
      }
      switch (seg.id) {
        case StatusBarSegmentId::Project:
        case StatusBarSegmentId::Branch:
        case StatusBarSegmentId::LineColumn:
          return theme_.chrome_text;
        default:
          return theme_.chrome_text_secondary;
      }
    };
    ForEachStatusBarSegmentRect(frame.status_bar, text_renderer_,
                                [&](const StatusBarSegmentViewModel& seg, const SDL_FRect& row) {
      const bool hovered = seg.clickable && PointerOver(row);
      const SDL_Color color = hovered ? theme_.text_primary : segment_color(seg);
      const PreparedFrameState::StatusBarSegmentPaint& segment =
          frame.status_bar_segments.emplace_back(PreparedFrameState::StatusBarSegmentPaint{
              .row = row, .text = seg.text, .color = color, .hovered = hovered});
      units.push_back(DerivedDamage::Unit{
          .rect = StatusBarHoverRect(row),
          .fingerprint =
              FingerprintOf(segment, hovered ? theme_.row_highlight : theme_.chrome_background)});
    });
  }
  frame.derived_damage.Commit(DerivedSurface::StatusBar,
                              [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
}

void WorkspaceShell::RenderStatusBar(SDL_Renderer* renderer,
                                     const WorkspaceLayout& ) const {
  const PreparedFrameState& frame = frame_state_;
  if (!frame.status_bar.visible) {
    return;
  }
  const SDL_FRect& bar = frame.status_bar.rect;
  DrawFilledRect(renderer, bar, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(bar.x, bar.y, bar.w, 1.0f), theme_.border);
  for (const PreparedFrameState::StatusBarSegmentPaint& segment : frame.status_bar_segments) {
    SDL_Color text_bg = theme_.chrome_background;
    if (segment.hovered) {
      DrawSelectableRowBackground(renderer, theme_, StatusBarHoverRect(segment.row), theme_.chrome_background,
                                  true);
      text_bg = theme_.row_highlight;
    }
    DrawVCenteredTextOn(text_renderer_, renderer, segment.row, 0.0f, segment.color, text_bg, segment.text);
  }
}

}
