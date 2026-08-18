# UI Invariants

Rules the shell's surfaces must keep holding. Each one is here because it was
*broken once*, usually invisibly, and the fix is not self-evident from the code.
They were previously buried inside `active-work.md`'s "shipped baseline" list,
where they read as history rather than as rules.

Reviewer-enforced unless a named test is given.

## Painting

### A painted frame is fully opaque
The scene renders into a retained RGBA texture blitted whole to a window that is
never cleared, so a translucent pixel re-composites against its own previous
output on every present until the region goes flat. Translucent fills must
therefore **composite, never overwrite**: `render::SetDrawColor` picks the blend
mode from the colour's alpha, every shell draw primitive routes through it, and
the scene texture presents with `SDL_BLENDMODE_NONE`.

Do not call `SDL_SetRenderDrawColor` directly from a render TU.

Guarded by `WorkspaceShell/RenderedFrameIsFullyOpaque`.

### Derived chrome asks for its own repaint
The status bar is built from state owned by *other* surfaces, so no `Request*Redraw`
helper covered it and it simply never repainted on a partial frame. `StatusBarService`
tracks whether a **painted** value changed and frame prep requests the strip when one
did.

Redraws requested from the render path must not strand: the event loop does not
block with one pending, and `HandleScheduledWake` merges them in.

### The mouse cursor rides redraw invalidation, not an input allowlist
A single `cursor_hit_generation_` is bumped by every cursor-relevant redraw, and the
per-frame `UpdateMouseCursor` fast path recomputes whenever the pointer moved or that
generation changed. On Wayland the shape only becomes visible once the compositor
recomposites, so an event-time cursor change must be followed by a present.

Do not reintroduce a per-field cursor fingerprint, do not poll the live OS pointer
(`SDL_GetMouseState`) from the render path, and do not make an event-time cursor
change that skips the present. Full reasoning: `dev-docs/platform/wayland-stale-cursor.md`.

## Surfaces and layout

### One quick-open surface
The file finder, project search, command palette, and the git commit/launch pickers
share `ComputeQuickOpenOverlaySurfaceRect`, one header block (title, query field,
result summary, `↑↓ select · Enter choose · Esc cancel` hint), and one set of row
offsets — `OverlayQueryRowOffset` / `OverlaySummaryRowOffset` / `OverlayListStartOffset`,
read by both the view-model builder and the click hit-test so a field cannot be
painted off its own hit target.

Do not reintroduce a per-mode overlay rect.

### A group's active tab is hydrated, or its pane paints the Welcome screen
Only a group's ACTIVE tab is eagerly loaded on session restore; every other tab
holds a deferred handle. `GroupActiveViewport` falls back to that group's welcome
surface whenever its active tab has no editor state, so an unhydrated active tab
paints Welcome over a strip that is highlighting a file. Every operation that
PROMOTES a tab to active in a group — closing the active one, dragging it out
into the other half of a split — therefore has to run the promoted neighbour
through `TabCoordinator::HydrateGroupActiveTab`. Closing had it spelled inline
and the cross-group move had nothing, so dragging a tab across a split blanked
the half it came from until 2026-08-18.

Guarded by `WorkspaceShell/MoveTabToGroupHydratesSourceGroupNeighbor`.

### Text that does not fit is truncated or wrapped, never sheared
Toasts scale their width with the window and ellipsize through
`TruncateToWidthEphemeralView`; narrow-rail empty states word-wrap through
`DrawWrappedPlaceholder` / `ForEachWrappedLabelLine`.

## Lists and empty states

### An empty list says so once, in the list
The count row sits directly above the list area, so a count that spells out the
empty state prints the same sentence twice. `BuildFilteredCountSummary` returns
empty at zero matches and every surface pairs it with its own list-area label.
There is no "0 of N" and no synthetic count.

This has regressed twice, which is why `RenderViewModelBuilder/EmptyPickerStatesAreNotStatedTwice`
walks every picker mode rather than trusting per-surface tests.

### Empty groups are hidden — unless the header owns a control
A clean checkout used to be ten rows of prose saying nothing. `ShouldShowSection`
drops an empty group and one headerless line covers the whole-panel state, as in
VSCode.

The exception is load-bearing and general: `Outgoing` is always shown because its
*header row* carries the base-branch button, so hiding the group would remove the
only control that could make it non-empty. **Before hiding any empty group, check
whether its header is also a control surface.**

### A view with no tab still needs a way in
The sidebar rail draws three primary tabs and pushes everything else — Problems,
Tests, Outline, plugin views — into the overflow menu, lighting the overflow button
whenever the active view lives behind it. Filtering a view out of *both* the row and
the menu leaves it reachable only by command, with nothing on screen saying which
view is showing.

### Every filtered surface reports its own filter
Settings and Help/About carry the quick-open footer — `N of M` left, key hint right —
and a zero-match query puts a line in the list area instead of leaving a blank card.
Hints are per-surface, not copied: Help/About is read-only, so it says
`↑↓ scroll · Esc close`, not the picker's `Enter choose`.

### Key hints join through the shared separator
`AppendHintSegment` / `JoinHintSegments` in `workspace/WorkspaceUiText.h`, separator
`kHintSeparator` (`" · "`). Do not re-declare `AppendHintSegment` in a TU — two files
independently grew byte-identical private copies joining on `"  |  "`, which is how
the git sidebar and the compare review header ended up spelled differently from every
other hint in the app.

Enforced by `CheckHintSegmentsUseTheSharedSeparator`, which targets the redefinition
rather than the `"  |  "` literal — that separator is legitimate between unrelated
fields (the breadcrumb's `path | left -> right`, the merge status line).

## Drags

### A live drag owns the pointer: no hover, no tooltip, no full-window repaint
Every pointer-owning gesture — the dividers, the scrollbars, a selection, a tab
reorder — is handled near the TOP of `HandleMouseMotion`, before the hover
pipeline, and damages only the surface it moves. A drag handled after the hover
pipeline pays two tooltip resolutions, two interactive-rect hit tests and the
sidebar/status/floating probes per motion event, for hover state the drag cannot
change; the tab drag did exactly that, and then asked for a full-window repaint,
until 2026-08-14.

Three consequences follow from the drag not re-resolving hover, and all three are
the drag path's job to honour:

- `last_mouse_*` freezes at the press, so nothing may claim hover from it
  (`tab_hovered` and the bottom-panel `PointerOver` both refuse while dragging);
- `HoveredTooltip` refuses outright while a tab drag is live, and the frame that
  STARTS the drag damages where the card was, because the drag's own damage is
  scoped to the strip and the card hangs below it;
- the cursor shape stays whatever the press resolved — a drag is logically still
  on its own surface wherever the pointer physically is.

### A drop target is only shown for a drop that would change something
The drag-to-split overlay paints exactly the region the tab would take, and
paints nothing at all when the release would be a no-op — the middle of the pane
the tab already lives in, an edge of the pane the tab already lives in when it is
that pane's only tab (carving it out empties the pane, which collapses straight
back), or any edge zone while the editor area is already at `kMaxEditorGroups`.
Dropping another pane's last tab on an edge is NOT a no-op: that pane collapses
and the target gains a neighbour. A highlight promising a move that will not happen
is worse than no highlight: the user reads the release as broken rather than as
refused. Both halves are one field, `TabDragState::body_drop_zone`, so the
renderer and the commit cannot disagree about whether a drop is live.

Guarded by `WorkspaceShell/EditorTabDragOverOwnPaneCenterOffersNoDrop`,
`WorkspaceShell/EditorTabDragToPaneEdgeRefusesLoneTab`, and
`WorkspaceShell/EditorTabDragToPaneEdgeMovesAcrossPanes`.

### A drag does not survive losing the window, and Escape abandons it
The focus-lost handler ends every in-flight gesture: the button-up goes to
whoever took the focus, so a gesture left "in progress" is still in progress on
the next click. Escape does the same thing deliberately, ahead of every surface
that also answers Escape. Abandoning is not committing: an abandoned tab drag
glides the lifted tab home and reorders nothing.

### The drop lands where the dragged thing is drawn
A reorder resolves its slot from the DRAGGED item's own box — its centre — not
from the cursor. Keying it off the cursor makes the landing spot depend on where
inside the item it was grabbed, so grabbing a wide tab near an edge reads as the
strip lagging half a tab behind the drag. The probe is unclamped even though the
ghost's paint position is pinned inside the strip, so pulling further than the
ghost can render still reads as "put it first / last".

On an overflowing strip both ends of the drop pin to what is VISIBLE — answering
"index 0" / "the end" would teleport the item to a slot nobody can see — and the
strip auto-scrolls under a pointer parked at its edge, pumped from the animation
tick so a perfectly still pointer keeps stepping. Those two go together: the pin
is what makes the drop honest, and the auto-scroll is what keeps the far end
reachable.

## Workflow

### Commit pre-check Warning is advisory; Blocking is not
`RequestCommit` handles three cases separately:

- a `Blocking` check refuses outright, every time, and nothing acknowledges it;
- nothing staged refuses with `Nothing staged`;
- unacknowledged `Warning` checks raise a `ConfirmCommitWarnings` prompt listing them.
  Confirming records the acknowledgements and dispatches the operation the user
  originally asked for; cancelling acknowledges nothing, so the next attempt asks
  again. `Open()` and a successful commit both clear the set.

Before this, `acknowledged_warning_ids` had no reachable writer, so every Warning
behaved exactly like Blocking — a repository with any untracked file could not be
committed at all.

Do not re-merge the Blocking and Warning arms of `RequestCommit`, and do not add a
Warning-severity pre-check expecting it to hard-block: use `Blocking` for that.
