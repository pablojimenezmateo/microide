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
#include "workspace/state/WorkspaceProjectState.h"

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
  // Per editor pane, the (viewport, content, scroll position) painted.
  std::array<std::uint64_t, kMaxEditorGroups> painted_pane_scroll{};
  // Per editor pane, the fold markers painted (FoldMarkerFingerprint) and the
  // (content, fold model revision) they were hashed at.
  std::array<std::uint64_t, kMaxEditorGroups> painted_fold_revision{};
  std::array<std::uint64_t, kMaxEditorGroups> painted_fold_markers{};
  // Per editor pane, whether it painted its file's plugin decorations (they show
  // only while the buffer is clean).
  std::array<bool, kMaxEditorGroups> painted_decorations{};
  // The breakpoint stores' revisions as painted (gutter dots, Breakpoints pane).
  std::uint64_t painted_breakpoints = 0;
  // The window's regions as painted (sidebar, bottom panel, editor surface, right
  // pane, status bar, tab strip), so one that moves repaints old and new.
  static constexpr std::size_t kTrackedRegions = 6;
  std::array<SDL_FRect, kTrackedRegions> painted_regions{};
  bool painted_regions_valid = false;
  // The surface that painted the focus ring (nullopt: no frame yet). Focus moves
  // from many places -- a key, a click, a control-channel command, a plugin --
  // and only HandleEvent repainted the two surfaces, so moving a tab from the
  // command line drew the editor's ring in a full frame and not in a partial one
  // (QA monkey, editor profile, seeds 3 and 6).
  std::optional<FocusTarget> painted_focus;
  // The editor group that was focused when the strips were painted: each strip
  // draws its group's active tab as focused or not, and a click in a group's
  // empty strip moved the focus with no strip repainted (QA monkey, seed 9).
  std::optional<std::size_t> painted_focused_group;
  // The editor viewport that had the keyboard last frame (null: none did), for
  // the on_focus_change auto-save.
  const void* focused_editor = nullptr;
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
    // Whether the working side is the active one: its caret, selection and
    // inline blame are drawn only then, and a click below the last row cleared
    // it with no repaint (QA monkey, seed 18).
    bool right_active = false;
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
  // The floating debug toolbar as painted: whether it shows, its rect, and the
  // run state its Continue/Pause button and step buttons draw. A session that
  // stopped or resumed repainted the debug pane and the menu bar but not the
  // toolbar, so its first button kept reading Pause over a stopped program.
  struct DebugToolbar {
    bool visible = false;
    bool stopped = false;
    SDL_FRect rect{};
    bool operator==(const DebugToolbar& o) const {
      return visible == o.visible && stopped == o.stopped && rect.x == o.rect.x &&
             rect.y == o.rect.y && rect.w == o.rect.w && rect.h == o.rect.h;
    }
  };
  DebugToolbar painted_debug_toolbar{};
  // The output channels' content revision the output panel was painted at
  // (WorkspaceOutputChannels::ContentRevision); 0 while it shows something else.
  std::uint64_t painted_output_revision = 0;
  // The bottom panel's tab models as painted (TabStripService::
  // BottomPanelTabsFingerprint); 0 while the panel is hidden. A strip label
  // changes with nothing else on screen: a results channel's first line, a
  // shell's OSC title -- whose output repaints only the terminal's body.
  std::uint64_t painted_bottom_panel_tabs = 0;
  // The active merge tab's status as painted (result state, conflicts left, the
  // selected one): a git refresh or the file watcher marks a merge result stale
  // or saved asynchronously, and the header kept reading the old state.
  std::uint64_t painted_merge_status = 0;
  // The active compare tab's review header as painted (tab and summary line). An
  // edit in the working side that adds or removes a hunk changes the line's hunk
  // count while damaging only the edited row.
  std::uint64_t painted_compare_header = 0;
  // The active diff's shape as painted -- each presentation row's kind, model row,
  // hunk and collapsed count -- recomputed only when its presentation revision
  // moves. An edit on the working side can regroup the whole diff (a hunk grows,
  // a collapsed block moves) while its damage names the edited line (QA monkey,
  // seed 2).
  const void* painted_compare_tab = nullptr;
  std::uint64_t painted_compare_presentation_revision = 0;
  std::uint64_t painted_compare_shape = 0;
  // The sidebar list selection painted (view and row): the tree follows the
  // active editor's file, and that selection change damaged nothing.
  std::uint64_t painted_sidebar_selection = 0;
};

}  // namespace microide::workspace
