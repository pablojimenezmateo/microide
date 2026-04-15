# MicroIDE Performance Findings

Last reviewed on 2026-04-15 after the terminal-focus freeze investigation and the follow-up
performance pass.

This note captures concrete bottlenecks that were found in the current codebase, what was already
fixed, and what still remains worth doing.

## Fixed In This Pass

### Text measurement hot path

Problem:

- `TextRenderer::MeasureWidth()` forwarded every request to the backend
- `TextRenderer::TruncateToWidth()` remeasured growing prefixes linearly
- chrome layout, truncation, blame overlays, and other repeated labels paid the same width cost
  over and over

Implemented:

- width caching inside `TextRenderer`, keyed by string and invalidated when backend or presentation
  scale changes
- logarithmic UTF-8-aware truncation instead of linear prefix probing
- dedicated renderer tests that fail if repeated labels stop hitting the cache or truncation falls
  back to many width probes

Impact:

- repeated UI labels, menu items, tab titles, blame text, and truncation paths now avoid redundant
  backend sizing work

### Terminal event flooding and transcript snapshots

Problem:

- terminal reader threads pushed a wake event for every read chunk
- several shell paths still cloned the full terminal transcript even when they only needed the
  selected rows or the current invocation rows

Implemented:

- terminal wake events are now coalesced until the shell consumes one update
- terminal selection copy, primary-selection sync, last-command transcript capture, and pending
  command capture now snapshot only the needed row ranges
- terminal-session coverage now includes wake-event coalescing

Impact:

- noisy commands generate less SDL event pressure
- large terminal scrollback no longer causes avoidable allocations in the remaining command-copy and
  selection paths

### Retained scene redraws and explicit invalidation

Problem:

- the app previously repainted the whole shell directly to the window backbuffer on every redraw
- caret blink ticks paid for a full shell render even though only one small visual region changed
- most handled UI events still implicitly fell back to full-scene redraws because redraw ownership
  lived in the app loop instead of the shell

Implemented:

- `Application` now keeps a retained scene texture for the shell
- redraws can target only a clipped dirty rect on that texture
- caret-blink updates now repaint only the active editor or terminal caret rect instead of the full
  shell
- the app-shell event contract now carries handled state plus redraw invalidation
- `WorkspaceShell` now owns redraw requests for chrome, overlay, prompt, sidebar, editor, and
  bottom-panel surfaces instead of forcing the app to guess

Impact:

- the common idle animation path now does materially less work
- menu hover, prompt interactions, editor typing, terminal input, terminal wake updates, and
  similar high-frequency paths now stay on the retained-scene partial redraw path
- redraw ownership is explicit instead of heuristic
- active editor typing now repaints only the focused editor pane, and terminal typing or wake
  updates now repaint only the panel content area instead of the full editor or bottom-panel surface

### Faster ASCII text draws

Problem:

- many hot text paths still rendered small ASCII runs through full string shaping and texture
  creation in `SDL_ttf`

Implemented:

- `SdlTtfTextBackend` now has a cached ASCII glyph path for common monospaced runs
- ASCII width measurement now uses fixed-cell width directly instead of calling into `TTF_GetStringSize`

Impact:

- editor, compare, merge, and terminal code paths that mostly draw ASCII text now avoid some of the
  heavier per-string backend work

## Still Worth Doing

### Finer-grained surface invalidation

Dirty-rect ownership is now explicit and broad enough to keep the common interaction paths off the
full-frame fallback, but the shell still invalidates at major-surface granularity. The highest-value
remaining UI work is now:

- editor edits and selections that repaint less than the full editor surface
- compare and merge redraws that invalidate only the affected pane or rows
- bottom-panel updates that repaint less than the full panel when only one row or caret changed

Relevant code:

- `src/workspace/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellInput.cpp`
- `src/workspace/WorkspaceCompareInteractionCoordinator.cpp`

### Lower-cost text rendering backend

The new ASCII glyph cache is a good middle step, but it is still not a full atlas-backed text
renderer. A more complete glyph-atlas or batched text path is still a good next step if text
rendering remains measurable after this pass.

Relevant code:

- `src/render/TextRenderer.cpp`
- `src/render/SdlTtfTextBackend.cpp`

### Profiling discipline

The startup tracer exists, and redraw tracing can now be enabled with `MICROIDE_TRACE_REDRAW=1`, but
broader redraw and idle profiling still needs to be done regularly before and after rendering work.

Relevant docs:

- `docs/startup-tracing.md`

## Notes

- The blame overlay remains performance-sensitive, but the width-cache work should reduce its layout
  cost without changing behavior.
- The terminal still needs broader real-world validation; these fixes reduce load but do not expand
  emulator coverage by themselves.
