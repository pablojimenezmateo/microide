## Context

MicroIDE's text path on the software SDL renderer is the dominant cost across
every editor paint scenario: ~4.5 s of smoke wall time concentrates in
`SdlTtfTextBackend` according to `docs/performance-bottleneck-deep-dive-4.md`.

The current path:

- `TextRenderer::MeasureWidth` is cached by string; hit rate > 99 % in steady
  state (good).
- `TextRenderer::DrawString` (and the lower-level `TextBackend::DrawRun`) goes
  through a per-`(text, color)` texture cache. Hits issue one
  `SDL_RenderTexture` call per run. Misses pay
  `TTF_RenderText_Blended` + `SDL_BlitSurface` + `SDL_CreateTextureFromSurface`,
  and the result is keyed by color (so scrolling syntax-colored unique lines
  thrashes the LRU).
- The architectural-lint rules in `tests/ArchitectureInvariantsTests.cpp`
  forbid string materialization in render TUs but say nothing about
  per-string textures.

Constraints we have to respect:

- The host stays SDL3 + SDL3_ttf only — no new third-party text-stack
  (HarfBuzz, FreeType direct, etc.).
- The software renderer is the perf-gated path; the GPU renderer benefits
  incidentally but is not the design target.
- Editor cells already have a UTF-8-byte representation per cell
  (`TerminalCell` for terminal, `LayoutLine::text_offsets` for editor); both
  expose codepoint boundaries cheaply.
- The render thread is single-threaded and holds no atlas mutex during draw;
  the atlas is built on initialization and read-only afterwards.
- Existing perf scenarios MUST stay green; no scenario can regress beyond
  tolerance.

Stakeholders: anyone touching `src/render/*`, `src/editor/EditorViewRenderer.cpp`,
`src/workspace/WorkspaceShellRenderBottomPanel.cpp`,
`tests/perf/PerfMain.cpp`.

## Goals / Non-Goals

**Goals:**

- Make ASCII text rendering on the software renderer batched and
  alloc-free on the hot path.
- Remove color from the texture-cache key set so theme variation and
  syntax-highlighted unique tokens stop forcing texture-cache evictions.
- Land behind a flag, ship perf evidence, then flip the default.
- Add three perf counters (`render.glyph_atlas_hits`,
  `render.glyph_atlas_fallbacks`, `render.glyph_atlas_evictions`) plus the
  `scroll_unique_code_lines` regression scenario, so future regressions are
  caught by the existing perf gate rather than by visual inspection.

**Non-Goals:**

- Atlasing non-ASCII glyphs. The fallback composite path already handles
  CJK / emoji / shaped runs correctly; adding a second-tier atlas for them
  is a separate change and probably wants a different data structure
  (variable-size cells, possibly a hash-keyed glyph cache).
- Splitting `document_->layout_revision` into tiers
  (tracked separately at `docs/known-tech-debt.md` #13). That change improves
  *invalidation cascade* costs; this change improves *paint* costs. They are
  complementary but independent.
- Replacing the SDL renderer or moving the editor text path off SDL3_ttf.
- Optimizing the editor *layout* path (`TextLayout::BuildVisibleLine`,
  `LayoutLine` shape) — out of scope.

## Decisions

### 1. One atlas per `(font face, font size)`, alpha-only, built at init

We rasterize ASCII glyphs `0x20..0x7E` once at `TextRenderer::EnsureInitialized`
into a single `SDL_Texture` with `SDL_PIXELFORMAT_INDEX8` (or `SDL_PIXELFORMAT_RGBA32`
with only the alpha channel populated, depending on what SDL3_ttf accepts
without a software-blend hit). Per-codepoint atlas-relative `SDL_FRect`s
live in a 95-entry `std::array<SDL_FRect, 95>` on the renderer.

**Alternatives considered:**

- Dynamic atlas (grow on demand for non-ASCII). Rejected because the win is
  in ASCII; supporting non-ASCII in the same atlas means eviction policy and
  rehashing — too much surface for this change. The fallback covers it.
- Per-glyph individual textures (one `SDL_Texture` per glyph). Rejected:
  trades one form of texture-creation cost for another, and loses the
  batched-submission benefit.
- HarfBuzz + glyph atlas. Rejected: scope creep; shaped runs already work
  via SDL3_ttf composite, and the ASCII fast path doesn't need shaping.

### 2. Color via `SDL_SetTextureColorMod`, set once per run

Atlas glyphs are alpha masks. Foreground color is applied by calling
`SDL_SetTextureColorMod(atlas, r, g, b)` (and `SDL_SetTextureAlphaMod` for
opacity) immediately before each batched submission, then submitting the
run. This is what removes color from the cache key set — the same atlas
texture serves every color.

**Alternatives considered:**

- Pre-rendering colored glyphs into the atlas. Rejected: explodes atlas
  size by 8 K+ for any reasonable color count.
- Per-glyph color-mod set + draw. Rejected: defeats batching; submission
  rate would stay at one call per glyph.

### 3. Batched submission via `SDL_RenderTextures`

SDL3 provides `SDL_RenderTextures(texture, src_rects, dst_rects, count)`
which submits N glyph quads against a single source texture in one call.
Per same-color run we build two arrays (`src` from atlas rects, `dst` from
cell screen positions) and emit one call.

The arrays live in `thread_local` scratch buffers on `SdlTtfTextBackend`,
matching the existing `terminal_foreground_runs_scratch_` pattern in
`src/workspace/WorkspaceShellRenderBottomPanel.cpp`. No allocations on the
hot path.

**Alternatives considered:**

- One `SDL_RenderTexture` call per glyph. Rejected as the baseline we're
  replacing.
- Manual `SDL_RenderGeometry` with per-glyph vertices. Rejected: same
  benefit, more code, harder to keep within the SDL3_ttf shape.

### 4. Run boundary decision: codepoint-based, all-or-nothing per run

We decide atlas-vs-fallback at the **run** level, not the glyph level. A
run goes to the atlas if and only if every codepoint in it is in
`0x20..0x7E`. The decision is a single pass over the run's UTF-8 bytes
testing whether each byte is `< 0x80`.

**Why per-run, not per-glyph:**

- Run boundaries already exist in the renderer (same color, contiguous
  cells). Splitting a same-color run into "atlas part + composite part" needs
  position tracking and complicates batch composition for no measurable win
  on real code.
- A single non-ASCII char in a mostly-ASCII run is rare; when it happens,
  the whole run falls back. The counter `render.glyph_atlas_fallbacks` makes
  this visible if it ever becomes a real cost.

**Alternatives considered:**

- Per-glyph decision with a "mixed" path. Rejected: doubles the hot-path
  control flow for marginal benefit.

### 5. Atlas lifecycle: build at init, rebuild only on font change

The atlas is built in `TextRenderer::EnsureInitialized` (or the existing
font-load entrypoint) and survives every other state change (theme, edit,
scroll, color-mod, project switch). It is torn down and rebuilt only when
the font face or font size changes.

This is enforced by the `render.glyph_atlas_evictions` counter, which
increments only in the rebuild path. Steady-state runs MUST report 0.

### 6. Fall back behind an opt-in flag for the first ship

The fast path lives behind `MICROIDE_RENDER_GLYPH_ATLAS=1` for the initial
landing. Default flips to on in the same change once
`scroll_unique_code_lines` passes on `perf-runner-v1` and the
`editor_render_whitespace_paint` / `editor_sticky_scroll_scroll` targets
in the spec hold.

This is the same pattern recent perf passes have used (e.g. the round-1
ASAN scene-realloc coalescing).

### 7. Architectural lint: new counter family + atlas-only-from-`SdlTtfTextBackend`

We extend `tests/ArchitectureInvariantsTests.cpp` with a rule that limits
direct atlas access to `src/render/SdlTtfTextBackend.cpp` only. No
workspace or editor TU should know about the atlas — they call
`TextBackend::DrawRun` and the atlas decision is internal.

## Risks / Trade-offs

- **Visual diff vs. composite path** → ASCII rendering may differ at the
  pixel level due to glyph hinting / sub-pixel positioning between
  `TTF_RenderText_Blended` (full-string layout) and per-glyph atlas
  composition. Mitigation: a pre-flip parity check fixture that renders
  the same ASCII strings both ways and diffs the resulting pixels with a
  bounded tolerance. If parity fails, hold the flag flip and either tune
  hinting flags or fall back further.

- **Atlas size memory cost** → 95 glyphs × glyph cell size at the
  configured font height. For a 16-pixel font this is roughly 30-60 KB of
  alpha texture. Acceptable; explicitly compared to the 80 KB the width
  cache used before round-2 cleanup. Mitigation: log atlas size on build,
  add to startup trace.

- **Variable-width fonts** → ASCII glyphs at the editor's monospace font
  are uniform-width but the atlas slot needs to allow the metric. We use
  per-glyph advance from SDL3_ttf, not a fixed cell width. Mitigation:
  source rects are stored per-glyph, not derived from a grid.

- **Non-monospace runs** → some chrome (status bar, sidebar) may use
  proportional fonts. The atlas keys on `(face, size)`, so each font gets
  its own atlas; non-monospace works correctly via the same per-glyph
  rect array. The hot path stays in the editor though, where the font is
  monospace.

- **First-paint cold cost** → atlas build adds ~95 `TTF_RenderGlyph_Blended`
  calls on the first init. Mitigation: this is one-time, amortizes across
  every subsequent frame, and is bounded; capture it under
  `MICROIDE_STARTUP_TRACE` per `docs/startup-tracing.md`.

- **SDL3_ttf version skew** → `TTF_RenderGlyph_Blended` and
  `SDL_RenderTextures` are both in SDL3 / SDL3_ttf 3.x; the existing build
  already depends on those versions. Mitigation: a static_assert on the
  SDL3 version in the atlas TU.

## Migration Plan

1. Land the atlas + counters + flag (default off). Sanitizers green; no
   perf scenario flips.
2. Land the `scroll_unique_code_lines` scenario with the atlas enabled
   under flag. Capture before/after JSON on `perf-runner-v1`.
3. Flip the flag default to on in a follow-up commit on the same change
   branch. Refresh affected baselines with the `perf-baseline:` annotation.
4. Once shipped, mark item 15 in
   `docs/performance-bottleneck-deep-dive-2.md` and the carry-over in
   `docs/performance-bottleneck-deep-dive-4.md` as done.

**Rollback:** the env var `MICROIDE_RENDER_GLYPH_ATLAS=0` disables the
atlas at runtime and re-routes everything through the composite path. If
needed, a revert of the flag-flip commit alone restores the prior default
behavior without touching the atlas code itself.

## Open Questions

- **Sub-pixel positioning:** if the font's glyph advance is non-integer,
  do we accumulate sub-pixel x-offsets across the run (like the composite
  path does) or snap each glyph to integer x? Investigate during step 1
  of the migration plan and document the choice in the atlas TU.
- **Should the atlas also serve the chrome paths** (status bar, sidebar
  tree labels)? They use `TextBackend::DrawRun` too. The cleanest answer is
  yes (one atlas per face/size, used everywhere), and the spec is written
  to permit that. Confirm during step 1 review.
- **Threshold for "ASCII run":** spec says all-or-nothing, but should we
  allow trailing `\t` / `\n`? In practice cells store one codepoint each
  and tab expansion happens upstream of `DrawRun`, so this likely doesn't
  arise. Confirm by inspection during implementation.
