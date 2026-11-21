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

// A clickable segment's hover background reaches 4 px past its row on each side.
SDL_FRect StatusBarHoverRect(const SDL_FRect& row) {
  return MakeRect(row.x - 4.0f, row.y, row.w + 8.0f, row.h);
}

}  // namespace

// The status bar is a derived-damage surface (openspec derived-damage): prep
// settles everything each segment paints -- row, text, colour, hover -- and
// fingerprints it, so a segment repaints whenever what it shows changes, whoever
// changed it (the strip's content is other surfaces' state: the caret, the
// language, the server, the branch). Paint draws the settled segments only.
void WorkspaceShell::PrepareStatusBarFrame(const WorkspaceLayout& layout) {
  PreparedFrameState& frame = frame_state_;
  frame.status_bar = RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  frame.status_bar_segments.clear();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::StatusBar);
  if (frame.status_bar.visible) {
    // The strip itself: its background and top border.
    units.push_back(DerivedDamage::Unit{
        .rect = frame.status_bar.rect,
        .fingerprint = PaintFingerprint()
                           .Add(frame.status_bar.rect)
                           .Add(theme_.chrome_background)
                           .Add(theme_.border)
                           .value()});
    const auto segment_color = [&](const StatusBarSegmentViewModel& seg) -> SDL_Color {
      // Diagnostic segments are colored by semantic tone (set in
      // StatusBarModelService) so the count/state -- not the display text -- picks
      // the color. Clickable segments then take the accent color on top of that.
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
    // Clickable segments get a hover background so the pointer cursor (already
    // returned by CursorKindForPosition for these segments) is matched by a
    // visible affordance.
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
                                     const WorkspaceLayout& /*layout*/) const {
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

}  // namespace microide::workspace
