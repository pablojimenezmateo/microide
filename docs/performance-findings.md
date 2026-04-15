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

## Still Worth Doing

### Partial redraw

The app still redraws whole frames after small invalidations. The highest-value remaining UI
performance project is still:

- dirty-rect invalidation
- caret-only invalidation
- hover-only or blink-only lightweight redraw paths

Relevant code:

- `src/app/Application.cpp`
- `src/workspace/WorkspaceShellRender.cpp`

### Lower-cost text rendering backend

`SDL_ttf` width caching helps, but text rendering still uses the current raster path. A glyph-atlas
or similarly cached draw backend is still a good next step if text rendering remains a measurable
cost.

Relevant code:

- `src/render/TextRenderer.cpp`
- `src/render/SdlTtfTextBackend.cpp`

### Profiling discipline

The startup tracer exists, but broader redraw and idle profiling still needs to be done regularly
before and after rendering work.

Relevant docs:

- `docs/startup-tracing.md`

## Notes

- The blame overlay remains performance-sensitive, but the width-cache work should reduce its layout
  cost without changing behavior.
- The terminal still needs broader real-world validation; these fixes reduce load but do not expand
  emulator coverage by themselves.
