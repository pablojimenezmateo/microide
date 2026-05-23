> **STATUS: REJECTED (2026-05-15).** This change was prototyped end-to-end
> behind an opt-in flag and **regressed** every editor paint scenario on the
> software renderer by 48–83 % wall-time. The implementation was reverted in
> full. See `dev-docs/performance/performance-bottleneck-deep-dive-4.md` →
> "Rejected experiment: ASCII glyph atlas" for the measured outcome and the
> three preconditions any future revisit MUST meet. Do not apply.

## Why

Editor text paint dominates the perf budget on the software renderer.
`dev-docs/performance/performance-bottleneck-deep-dive-4.md` measures ~4.5 s of wall time across
the editor paint scenarios in the smoke suite — sticky_scroll 1.08 s,
whitespace_paint 0.80 s, smart_indent_typing 0.80 s, auto_close_typing 0.68 s,
indent_guides_paint 0.64 s, fold_recompute 0.54 s — all dominated by
`SdlTtfTextBackend`'s per-string draw path. The `RenderTextTextureCache` hit
rate is already > 99 %, so the remaining cost is in (a) the per-hit
`SDL_RenderTexture` draw-call overhead and (b) the per-miss
`TTF_RenderText_Blended` + `SDL_BlitSurface` + `SDL_CreateTextureFromSurface`
triple. Both go away with a single per-font ASCII glyph atlas combined with
batched glyph submission via `SDL_RenderTextures`. This is round-2 Finding 15 /
round-3 P5 carried forward and identified in round 4 as the single biggest
remaining gain.

## What Changes

- Add a per-font ASCII glyph atlas (codepoints 0x20..0x7E) built once at
  `TextRenderer` initialization. The atlas is an alpha-only `SDL_Texture`
  keyed by `(font, font-size)`; color is applied per-draw via
  `SDL_SetTextureColorMod`, so color leaves the texture-cache key entirely.
- Add a fast path in `SdlTtfTextBackend` that walks the cell array, groups
  cells by foreground color, and emits one batched `SDL_RenderTextures` call
  per same-color run using atlas-relative source rects.
- Keep the existing composite path as the fallback for shaped runs and any
  codepoint outside the ASCII range. The cutover happens at run boundaries.
- Atlas lifecycle: rebuilt only on font reload (size or face change); not on
  theme change, edit, or scroll.
- Add `render.glyph_atlas_hits`, `render.glyph_atlas_fallbacks`, and
  `render.glyph_atlas_evictions` perf counters. Steady-state
  `glyph_atlas_evictions` must be 0.
- Add a `scroll_unique_code_lines` perf scenario that walks ~5 000 distinct
  syntax-colored lines and asserts steady-state `text_texture_cache_misses`
  < 1 % of cells visited and `glyph_atlas_evictions == 0`.
- Land behind a flag (`MICROIDE_RENDER_GLYPH_ATLAS=1`) first; flip default to
  on once the new scenario is green on `perf-runner-v1`.

Out of scope: layout-revision tier split (tracked in
`dev-docs/project/known-tech-debt.md` #13); non-ASCII atlas (deferred — fallback path
covers it); GPU-renderer-specific tuning beyond what the software renderer
benefits from.

## Capabilities

### New Capabilities
- `text-renderer-glyph-atlas`: a per-font ASCII glyph atlas plus the batched
  draw path that consumes it. Owns the atlas/fallback boundary, the color
  application contract, the atlas lifecycle, and the assertable perf counters.

### Modified Capabilities
- `performance-budgets`: tighten `editor_render_whitespace_paint` and
  `editor_sticky_scroll_scroll` p95 wall budgets to reflect the new ceiling
  once the atlas is on by default. (Numbers pinned in the spec delta.)
- `performance-harness`: add the `scroll_unique_code_lines` scenario plus the
  glyph-atlas counter family to the reportable counter set.

## Impact

- Code: `src/render/TextRenderer.{h,cpp}`, `src/render/SdlTtfTextBackend.{h,cpp}`,
  `src/util/PerformanceCounters.{h,cpp}`, `tests/perf/PerfMain.cpp` (new
  scenario), `tests/perf/fixtures/` (scrolling fixture).
- APIs: no external API change; the `TextRenderer::DrawString` /
  `TextBackend::DrawRun` contract gains a clarified "atlas-eligible run"
  internal concept but the public signatures stay the same.
- Dependencies: no new third-party libraries; relies on existing SDL3
  (`SDL_RenderTextures`, `SDL_SetTextureColorMod`) and SDL3_ttf
  (`TTF_RenderGlyph_Blended`) functionality.
- Sanitizers: atlas is initialization-time + render-thread-read-only;
  surface is small for ASAN/TSAN. The change still runs through the standard
  sanitizer presets per `AGENTS.md`.
- Performance harness: existing baselines in `tests/perf/baselines/` will need
  refreshing for the affected scenarios; the change record includes the
  required `perf-baseline:` line.
