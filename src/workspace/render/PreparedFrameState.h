#pragma once

// What one prepared frame decided once for all of its renders, and what the last
// prepared frame painted.
//
// Paint must not read the clock, the live pointer or another thread's state: a
// partial frame renders several clips, and the QA redraw verifier renders the same
// frame again in full, so every render of one frame has to paint the same thing.
// The decisions go here, made in PrepareFrameOnce. And whatever moves a caret, a
// hover or a document away from where the last frame painted it needs that region
// repainted; rather than every transition carrying its own damage request (several
// carried none), frame prep compares with what was painted and repaints both.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>

#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::workspace {

struct PreparedFrameState {
  // ---- decided once per prepared frame ---------------------------------
  // The caret blink phase. Paint sites read this, never the clock.
  bool caret_visible = false;
  // The floating or modal surface the pointer is over, if any: PointerOver lights
  // up only rects inside it (an empty rect: the pointer is outside a modal, and
  // nothing hovers).
  std::optional<SDL_FRect> pointer_occluder;

  // ---- what the last prepared frame painted ----------------------------
  std::optional<SDL_FRect> painted_caret_rect;
  // The element painted hovered and the tooltip card painted, under the pointer.
  std::optional<SDL_FRect> painted_hover_rect;
  std::optional<SDL_FRect> painted_tooltip_rect;
  // The editor hover card painted (hover docs, diagnostics, the blame card).
  std::optional<SDL_FRect> painted_hover_popup_rect;
  // Per editor pane, the (viewport, content revision) painted.
  std::array<std::uint64_t, kMaxEditorGroups> painted_pane_content{};
  // The tab strips painted mid-slide (kind None: none). A new drag can take a
  // strip's animation slot while it still glides, and that strip's tabs then
  // snap back with no frame of their own.
  struct SlideStrip {
    TabDragKind kind = TabDragKind::None;
    std::size_t group_index = 0;
  };
  std::array<SlideStrip, kMaxAnimatingTabStrips> painted_slide_strips{};
};

}  // namespace microide::workspace
