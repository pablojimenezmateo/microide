## Context

The editor grid (`EditorSplitTree`, TD-2026-08-18-265/266, TD-2026-08-30-280) is
an n-ary tree in canonical form whose in-order leaves are the editor groups. The
terminal panel kept a separate, one-axis model. This change makes the tree the
pane model of both surfaces.

## Decisions

### One tree type, one cap
`SplitTree` keeps the editor tree's contract unchanged (canonical form, heap-free
inline storage, process-unique revision, structural `operator==`, pre-order
record). Its leaf cap `kMaxSplitLeaves = 8` is the capacity of every per-pane
inline container on both surfaces. The terminal cap rises from 6 to 8: with two
axes a tab can hold eight panes at a usable size, and a separate smaller cap would
need a second instantiation of every inline container for no benefit.

### Region layout is separate from chrome
`ComputeSplitRegions(tree, area, divider)` is the tree walk that was inside
`ComputeEditorGroupRects`. The editor maps each region to strip / breadcrumb /
surface (`ChromeForRegion`); the terminal's regions are its pane bodies. Dividers
carry (node, boundary, vertical, pair_start, pair_extent), so a live divider drag
on either surface re-resolves its pair by address and converts the pointer along
the divider's own axis.

### Directional focus is geometric
`AdjacentSplitRegion(regions, from, direction)` is the editor's former
`AdjacentEditorGroup` over plain rects. Terminal Left/Right used to step by index,
which in a grid would jump to a pane above or below.

### Dropping a terminal tab: what is the target?
The strip shows tabs; the body shows only the active tab, and pressing a tab
activates it — so at the moment a drag starts, the body shows the dragged tab
itself. The drag records the tab that was on screen before the press. While the
pointer is over the panel body (off the strip), that tab is shown again (a real
activation, so every per-frame consumer stays on the one active-tab path), and the
body offers its panes' edges; back on the strip, the dragged tab is shown again
and the gesture is an ordinary reorder. A cancelled gesture leaves the dragged tab
active, as a click would have. The body offers no centre zone: a terminal pane
holds one session, so "into this pane" has no meaning.

### Graft, not move-one-pane
A dragged tab may itself be split. Moving only its active pane would leave the
tab behind with the rest, which is not what dragging a tab means. `GraftTree`
inserts the dragged tab's whole tree beside the target leaf (flattened into the
parent when the axes agree, so the result stays canonical); the dragged tab's
panes are spliced into the target's pane vector at the returned ordinal, in order.
The drop is offered only when the combined pane count fits the cap.

## Risks

- Session compatibility: older sessions' terminal weights are dropped (tabs
  restore as an even row). Accepted: correctness and one format over a lossy
  compatibility path for a cosmetic share.
