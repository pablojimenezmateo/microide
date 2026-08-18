#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class TabMouseCoordinator {
 public:
  struct Operations {
    // The three strip lists are memoized by their owning service and handed back
    // BY REFERENCE. Returning them by value copied three std::strings per visible
    // tab on every mouse-motion event of a drag, every press on a strip, and
    // twice per wheel tick (TD-2026-08-14-211). Callers must not hold the
    // reference across an operation that can rebuild the strip (activate, close,
    // switch, scroll) — copy the POD fields they need first.
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_project_tabs;
    std::function<void(std::size_t)> request_close_project;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_tabs;
    // The same list for an explicit group, so a cross-group drag can resolve the
    // strip under the pointer without focusing it first (TD-2026-08-14-213).
    // Memoized per group by the same cache, and handed back by reference for the
    // reason above it.
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(std::size_t,
                                                                      const SDL_FRect&)>
        compute_visible_tabs_for_group;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::size_t)> activate_tab;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<bool()> bottom_panel_visible;
    std::function<SDL_FRect(const SDL_FRect&)> bottom_panel_terminal_new_tab_rect;
    std::function<void(std::string)> open_terminal;
    std::function<const std::vector<WorkspaceShell::VisibleStripTab>&(const SDL_FRect&)>
        compute_visible_bottom_panel_tabs;
    std::function<bool(std::size_t)> activate_bottom_panel_tab;
    std::function<bool(std::size_t)> close_bottom_panel_tab;
    std::function<bool(std::size_t)> bottom_panel_tab_is_terminal;
    std::function<void()> clear_tab_drag;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<bool(std::size_t)> move_active_project_to;
    std::function<bool(std::size_t)> move_active_tab_to;
    // Moves one tab out of `from_group` at `from_index` and into `to_group` at
    // `to_slot`, focusing the destination and activating the moved tab. This is
    // NOT `move_active_tab_to` with a group argument: nothing is reordered within
    // a list, the source group collapses if the move emptied it, and the slot is
    // a raw insertion point rather than a post-removal target index.
    std::function<bool(std::size_t, std::size_t, std::size_t, std::size_t)> move_tab_to_group;
    // Carves a NEW editor group out of `from_group` holding just its tab
    // `from_index`, on the side the bool selects (true = ahead of the source
    // group). Backs the drag-a-tab-onto-a-pane-edge split; a body drop in the
    // CENTER of a pane goes through `move_tab_to_group` instead, because that is
    // a move into an existing group and nothing about the split changes.
    std::function<bool(std::size_t, std::size_t, EditorSplitOrientation, bool)>
        move_tab_to_new_group;
    std::function<bool(std::size_t)> move_active_terminal_tab_to;
    std::function<bool(std::size_t)> move_active_output_tab_to;
    std::function<void()> save_workspace_session;
    std::function<void()> save_session_state;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_project_tab_overflow_controls;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls;
    // Per-group, for the same reason as `compute_visible_tabs_for_group`: the
    // hidden-left/right counts are derived from that group's own scroll index.
    std::function<WorkspaceShell::TabStripOverflowControls(
        std::size_t, const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_tab_overflow_controls_for_group;
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        compute_bottom_panel_tab_overflow_controls;
    std::function<bool(int)> scroll_project_tab_strip;
    std::function<bool(int)> scroll_editor_tab_strip;
    // Auto-scroll during a cross-group drag has to walk the strip the pointer is
    // over, which is not the focused group's — `scroll_editor_tab_strip` would
    // scroll the wrong half of the split.
    std::function<bool(std::size_t, int)> scroll_editor_tab_strip_for_group;
    std::function<bool(int)> scroll_bottom_panel_tab_strip;
    // Per-group editor rects (one entry for a single group, two in a split), so
    // the tab mouse path can resolve which group's strip the pointer hit and
    // focus it. Takes the caller's layout and hands back the heap-free
    // InlineVector-backed struct: flattening it into a vector of pairs was a heap
    // allocation AND a second full layout computation, on every motion event of a
    // drag and every wheel tick over a strip.
    std::function<EditorGroupRectsLayout(const WorkspaceLayout&)> compute_editor_group_rects;
    std::function<void(std::size_t)> focus_editor_group;
  };

  TabMouseCoordinator(ProjectCatalogState& project_catalog,
                      ProjectWorkspaceState& current_project_state,
                      TabDragState& tab_drag_state,
                      TabSlideState& tab_slide_state,
                      Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);
  // Re-runs the drag update against the stored pointer position, with no new
  // event. The shell's animation tick calls this so a pointer parked at the edge
  // of an overflowing strip keeps auto-scrolling. Returns true if anything moved.
  bool TickDragAutoScroll();
  // Abandons a live drag: the lifted tab glides home and nothing is reordered.
  // Escape does this, and so does the window losing focus — the button-up would
  // be delivered to whoever took the focus, so the gesture cannot be finished.
  // Returns true if there was a drag to abandon.
  bool CancelDrag();

 private:
  // The on-screen tab strip of each editor group, heap-free. Falls back to a
  // single entry for the focused group over `layout.tab_strip` when the per-group
  // operation is unavailable.
  struct EditorGroupTabStrips {
    struct Entry {
      std::size_t group_index = 0;
      SDL_FRect strip{};
    };
    std::array<Entry, kMaxEditorGroups> entries{};
    std::size_t count = 0;
  };
  EditorGroupTabStrips ResolveEditorGroupTabStrips(const WorkspaceLayout& layout) const;

  // Resolved geometry/state for the strip that owns the in-flight drag. Shared
  // by motion (compute target slot) and commit (single reorder on release).
  struct DragStrip {
    SDL_FRect strip{};
    // Borrowed: points into the owning service's memoized visible-tab list. Valid
    // only until something rebuilds that strip, which is why every mutation
    // re-resolves rather than reusing an older DragStrip.
    const std::vector<WorkspaceShell::VisibleStripTab>* tabs = nullptr;
    std::size_t count = 0;          // reorderable items in the dragged tab's kind
    std::size_t active = 0;         // active index within that kind list
    std::size_t model_offset = 0;   // model index where this kind's range begins
    std::size_t group_index = 0;    // editor group this strip belongs to
    std::function<bool(std::size_t)> move;  // commit a reorder within the kind list
    std::function<bool(int)> scroll;        // step the strip's overflow window
    std::function<WorkspaceShell::TabStripOverflowControls(
        const SDL_FRect&, const std::vector<WorkspaceShell::VisibleStripTab>&)>
        overflow;
    bool valid = false;
  };
  DragStrip ResolveDragStrip(const WorkspaceLayout& layout, TabDragKind kind);
  // The editor strip of one specific group. `ResolveDragStrip(Editor)` is this
  // for the drag's SOURCE group; a cross-group drag resolves the destination
  // group's strip through the same helper so both sides share one shape.
  DragStrip ResolveEditorDragStrip(const WorkspaceLayout& layout, std::size_t group_index);
  DragStrip ResolveBottomPanelDragStrip(const WorkspaceLayout& layout);
  // Which editor group's strip the live pointer is over, or the source group when
  // it is over neither (leaving the strips entirely must not retarget the drop).
  // Takes the already-computed per-group rects: this and the body-drop probe below
  // both run on every motion event of a drag, and recomputing the editor layout
  // once each would triple the per-event layout cost.
  std::size_t ResolvePointerEditorGroup(const WorkspaceLayout& layout,
                                        const EditorGroupRectsLayout& rects) const;
  // Resolves where a drop would land if the pointer were released over an editor
  // pane BODY right now, writing `body_drop_zone` / `body_drop_group_index` /
  // `body_drop_rect` on the drag state. Leaves the zone `None` when the pointer
  // is over a strip (that is a strip drop) or when the drop would be a no-op, so
  // "is a body drop live" is a single field read for the renderer and the commit.
  void ResolveEditorBodyDrop(const EditorGroupRectsLayout& rects);
  // Commits a drop onto a pane body: a move into that pane's group for the center
  // zone, a new group carved out on that side for the four edge zones.
  void FinishEditorBodyDrop(const WorkspaceLayout& layout);
  // Recomputes slot + slide targets for the current `pointer_x`, auto-scrolling
  // first if the pointer is parked at an edge with something hidden behind it.
  bool UpdateDragForPointer();
  bool AutoScrollDragStrip(const DragStrip& d);
  // Commits the pending reorder (if any) and hands off to the post-release
  // settle glide in one pass, so the strip is resolved once before the model
  // moves and once after instead of twice on each side.
  void FinishDrag();
  // Commits a drop into the OTHER editor group. Split out of FinishDrag because
  // none of its four steps (guard against a moved model, resolve the raw slot,
  // move between groups, glide the dropped tab home in the destination strip) is
  // the same operation as the within-strip reorder.
  void FinishCrossGroupDrag(const WorkspaceLayout& layout);
  void PersistReorderedTabs(TabDragKind kind);
  // Recomputes the Chrome-like slide targets for the live pointer: neighbor tabs
  // ease toward `d`'s displaced layout with a gap opened at `insertion_slot`.
  // `slot` is which of the two animating strips this seeds — 0 for the strip the
  // gesture started on, 1 for a cross-group destination — and `lifted_index` is
  // the model index rendered as the floating ghost, or `kNoLiftedTab` when the
  // dragged tab is not in this strip at all.
  static constexpr std::size_t kNoLiftedTab = static_cast<std::size_t>(-1);
  // An insertion slot past every model index, i.e. "no gap opens in this strip".
  // The source strip of a cross-group drag uses it: its tabs close ranks over the
  // lifted tab and nothing separates for a drop that is landing elsewhere.
  static constexpr std::size_t kNoInsertionSlot = static_cast<std::size_t>(-1);
  void SeedSlideTargets(const DragStrip& d,
                        std::size_t insertion_slot,
                        std::size_t slide_slot,
                        std::size_t lifted_index);
  // Drops the second animating strip back to idle, freeing its offsets. Called
  // when a cross-group drag comes back over its own group.
  void ClearDestinationSlide();
  // Steps the ease on the input thread. Motion events arrive faster than the
  // ~16ms scheduled wake, so without this the neighbors would only move once the
  // pointer paused.
  void AdvanceSlideOnInputThread();

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  TabDragState& tab_drag_state_;
  TabSlideState& tab_slide_state_;
  Operations operations_;
};

}  // namespace microide::workspace
