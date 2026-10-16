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
#include <cstddef>
#include <cstdint>
#include <optional>

#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::workspace {

struct PreparedFrameState {
  // ---- decided once per prepared frame ---------------------------------
  // The caret blink phase. Paint sites read this, never the clock.
  bool caret_visible = false;
  // Whether the focused terminal owns the blinking caret (not its find bar, not a
  // modal): only then does its caret blink; otherwise it shows a steady outline.
  bool terminal_owns_caret = false;
  bool painted_terminal_owns_caret = false;  // outline vs blinking block, as painted
  // The floating or modal surface the pointer is over, if any: PointerOver lights
  // up only rects inside it (an empty rect: the pointer is outside a modal, and
  // nothing hovers).
  std::optional<SDL_FRect> pointer_occluder;

  // ---- what the last prepared frame painted ----------------------------
  std::optional<SDL_FRect> painted_caret_rect;
  // The element painted hovered and the tooltip card painted, under the pointer.
  std::optional<SDL_FRect> painted_hover_rect;
  std::optional<SDL_FRect> painted_tooltip_rect;
  std::size_t painted_tooltip_text_hash = 0;  // the same card can name another tab
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
  // Which translucent modal backdrops were painted (a bit per surface). Each dims
  // everything under it, so a modal opening over another changes the whole
  // window, not just the new dialog.
  std::uint8_t painted_backdrops = 0;
  // Whether the focused pane painted the find widget's matches: they show only
  // while the widget is the overlay, and another overlay replacing it (a
  // completion request) took them off the editor without repainting it.
  bool painted_find_matches = false;
  // The active compare tab's scroll position as painted. Typing past a pane's
  // edge scrolls BOTH panes sideways to keep the caret in view while the edit
  // damages only its row, so a moved position repaints the surface.
  struct CompareScroll {
    const void* tab = nullptr;
    int row = 0;
    std::size_t column = 0;
    bool operator==(const CompareScroll&) const = default;
  };
  CompareScroll painted_compare_scroll{};
  // Where the editor split's dividers were painted (a hash of their rects). A
  // divider that moved -- the split's shares changed -- moves every pane beside
  // it; the change is repainted whatever damage the next frame carries.
  std::uint64_t painted_split_dividers = 0;
  // The project strip's source version as painted (WorkspaceTabStripChrome).
  std::uint64_t painted_project_strip_version = 0;
  // Which editor tabs painted a dirty marker (a hash of (group, tab) per dirty
  // tab). Many paths change a buffer's dirty state -- typing, but also Replace
  // All, a banner Reload, an undo back to the saved text, a save -- and each used
  // to have to remember to repaint the tab strip; the ones that forgot left the
  // tab's close button where its dirty dot belonged.
  std::uint64_t painted_editor_tab_dirty = 0;
  // Where the overlay card was painted. An overlay that shrinks or moves while it
  // is up -- the completion list narrowing as the user types, a picker whose
  // result count changed -- repainted only its new rect, leaving the strip it no
  // longer covers on screen. A moved rect repaints old and new.
  std::optional<SDL_FRect> painted_overlay_rect;
  // The sidebar list selection painted (view and row): the tree follows the
  // active editor's file, and that selection change damaged nothing.
  std::uint64_t painted_sidebar_selection = 0;
};

}  // namespace microide::workspace
