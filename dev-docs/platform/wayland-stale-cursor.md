# The Wayland stale-cursor bug (dropped present)

> Status: fixed on `main` (2026-06). Regression test:
> `WorkspaceShell/CursorChangeRequestsPresent`.

A bug that plagued microide "since the beginning": you move the mouse onto certain
items and **the cursor shape doesn't update** — the previous shape lingers even
though the app is focused. Intermittent, looked broken, resisted several fixes.

## TL;DR

When the pointer moves onto an item that changes only the cursor (e.g. text →
button) and nothing else on screen changes, `HandleMouseMotion` returns `false`
("nothing interesting to repaint"). The app loop **ignores a handler's requested
redraw when `handled` is false**, so the present that would have shown the new
cursor was silently dropped. `SDL_SetCursor` had run, but on Wayland the shape only
becomes visible once the compositor recomposites (the cursor rides a hardware
overlay plane), so the old shape stayed on screen.

Two-part fix:

1. **Queue a present when the cursor changes.** `UpdateMouseCursor` calls
   `RequestCursorPresent()` (a present bit, not damage — see below) on a real apply
   at event time.
2. **Deliver any queued redraw.** `WorkspaceEventDispatcher::finish()` now treats a
   pending redraw as "handled", so the app loop renders it instead of dropping it.

## The two clues that cracked it

- **"It doesn't happen while screen-recording."** A recorder forces the compositor
  to draw a *software* cursor composited into every frame and to recomposite
  continuously, so any cursor change shows on the next frame. That told us the
  shape *was* being set — it just wasn't being shown. (Same class as the macOS
  `CGCursorIsDrawnInFramebuffer()` issue:
  <https://gist.github.com/retroplasma/ec21767d0a8380c7ea9c2fbee1c7d6bf>.)
- **"It happens when the caret stops blinking, and it's worst on the welcome
  screen."** The blinking caret repaints ~2×/second; each of those frames presents,
  which recomposites and re-latches the cursor plane — accidentally masking the
  dropped present. Stop the blink (idle) or remove the caret entirely (welcome
  screen) and there is nothing left to mask it.

## How it was pinned down

Reasoning in circles ended once the real app was instrumented (env-gated logging of
every cursor decision and every present). The log was unambiguous:

```
cursor evt APPLY x=256 y=235 0->2 force=0 ... present=1   <- cursor changed (Default->Pointer), present requested
cursor evt skip-samekind x=255 ... kind=2                 <- subsequent motions agree it's Pointer
...                                                        <- and NOT ONE "render PRESENT" line anywhere
```

The cursor was applied and a present was requested, but no present ever happened.
The request was being dropped. The cause was `HandleMouseMotion`'s return value:

```cpp
// WorkspaceShellMouseMotion.cpp — depends only on hover visuals, never the cursor:
return hover_visual_changed || chrome_tooltip_visual_changed ||
       sidebar_hover_button_changed || status_segment_hover_changed;
```

A motion that only changes the cursor returns `false`, and the app loop gates on it:

```cpp
// Application.cpp
const auto result = HandleEvent(event);
if (result.handled) {            // <- false => result.redraw is discarded
  ... accumulate result.redraw into dirty_rects ...
}
```

## The fix

```cpp
// WorkspaceEventOrchestrator.cpp — a queued redraw IS a reason to render.
const auto finish = [this](bool handled) {
  RenderInvalidation redraw = operations_.consume_pending_render_invalidation();
  return EventResult{
      .handled = handled || redraw.HasAnyRedraw(),
      .redraw = std::move(redraw),
  };
};
```

```cpp
// WorkspaceShellCursor.cpp — a cursor change at event time queues the present that
// re-latches the hardware cursor plane. The render-path call already presents this
// frame, so it passes during_frame_prepare=true and skips the request.
if (!during_frame_prepare) {
  RequestCursorPresent();
}
```

`RequestCursorPresent` sets `RenderInvalidation::present` — a bit, not a damage
rect. The app loop answers a present with no damage by re-blitting the retained
scene texture and calling `SDL_RenderPresent` (`Application::PresentRetainedScene`),
which commits the surface and makes the compositor recomposite and re-latch the
cursor plane without a render pass.

### Why it is not a 1×1 damage rect (2026-10-13)

It used to be: a 1×1 rect at the pointer, chosen because the retained scene is
re-blit whole anyway. But every input handler ends in
`EnsureRedraw(Request<Surface>Redraw)`, which repaints the surface the handler
changed *unless damage is already queued* — and mouse-down and wheel resolve the
cursor before they dispatch. Whenever the click or scroll also changed the cursor
(or a reassert was forced, which Wayland's hit-test path does), that pixel
satisfied the guard and the surface fallback never ran: expanding a directory in
the project tree, or scrolling it, repainted one pixel and left the old rows on
screen. A present is now a bit, not damage. (Since TD-2026-10-13-337 the guard
no longer asks "is anything queued" at all: `EnsureRedraw(mark, ...)` compares the
shell's damage generation against a mark taken before the handler ran, so only
the handler's own requests can stand in for its fallback.)
Regression tests: `WorkspaceShell/TreeExpandRepaintsSidebarWhenCursorChanges`,
`WorkspaceShell/TreeWheelRepaintsSidebarWhenCursorChanges`.

## Why it stays fast

- The present is requested **only on a real cursor change** (an element-boundary
  crossing), never per motion or per frame. Gliding within one element, or holding
  still, requests nothing.
- The `finish()` change costs one `HasAnyRedraw()` (a bool + empty-vector check) per
  event. Events that requested no redraw still report `handled` exactly as before.

## Don't regress this

- Don't gate the app loop's use of `result.redraw` on `result.handled` again without
  keeping `finish()`'s "pending redraw ⇒ handled" rule — a handler that requests a
  redraw while returning `false` (cursor-only changes do exactly this) would have
  its frame dropped.
- Don't drop `RequestCursorPresent` on the event-time cursor path: without a present
  the new shape sits queued and goes stale on an idle compositor — invisible while
  the caret blinks, obvious the moment it stops or on a caret-less screen.
- Don't "fix" a recurrence by reviving a per-frame live-pointer poll or a continuous
  repaint; present only on a real apply.
- Don't turn the present back into a damage rect, and don't let a "something is
  already queued" guard count `present` — that is the stale-tree bug above.

## References

- `src/workspace/WorkspaceEventOrchestrator.cpp` — `finish()`
- `src/workspace/shell/WorkspaceShellCursor.cpp` — `UpdateMouseCursor`
- `src/workspace/shell/WorkspaceShellRedraw.cpp` — `RequestCursorPresent`
- `src/workspace/WorkspaceEventResult.h` — `RenderInvalidation::present` / `HasDamage`
- `src/app/Application.cpp` — the present-only branch of the loop
- `src/workspace/shell/WorkspaceShellMouseMotion.cpp` — `HandleMouseMotion` return value
- `tests/WorkspaceShellCursorTests.cpp` — `CursorChangeRequestsPresent`
- Inspiration: <https://gist.github.com/retroplasma/ec21767d0a8380c7ea9c2fbee1c7d6bf>
