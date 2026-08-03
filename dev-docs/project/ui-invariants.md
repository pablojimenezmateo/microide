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
