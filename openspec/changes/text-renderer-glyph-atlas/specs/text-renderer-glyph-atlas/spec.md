## ADDED Requirements

### Requirement: Per-Font ASCII Glyph Atlas

`TextRenderer` SHALL build a per-font ASCII glyph atlas at initialization, covering codepoints `0x20`..`0x7E` inclusive. The atlas SHALL be a single alpha-only `SDL_Texture` keyed by `(font face, font size)`, with each glyph's atlas-relative `SDL_FRect` stored on the renderer. The atlas SHALL NOT be invalidated by theme changes, edits, scrolls, or color-mod changes. The atlas SHALL be torn down and rebuilt only when the font face or font size changes.

#### Scenario: Atlas is built once per font
- **WHEN** `TextRenderer::EnsureInitialized` returns successfully on a given font face / size
- **THEN** the renderer SHALL hold exactly one atlas texture for that `(face, size)` key, and subsequent draw calls SHALL NOT rebuild it

#### Scenario: Theme change preserves the atlas
- **WHEN** the editor color theme changes without changing the font face or size
- **THEN** the atlas texture SHALL remain unchanged and zero atlas rebuilds SHALL be observed via `render.glyph_atlas_evictions`

#### Scenario: Font reload rebuilds the atlas
- **WHEN** the editor font face or size changes
- **THEN** the previous atlas texture SHALL be released and a new atlas SHALL be built for the new `(face, size)` key before the next draw call returns

### Requirement: Color Application Via Color-Mod, Not Per-String Keying

Text color SHALL be applied to atlas-rendered runs via `SDL_SetTextureColorMod` (and `SDL_SetTextureAlphaMod` for opacity) on the atlas texture immediately before each batched glyph submission. The atlas's texture-cache key SHALL NOT include color. The existing `RenderTextTextureCache` color-keyed path SHALL remain only for the fallback composite path.

#### Scenario: Same string, different colors hits the atlas once each
- **WHEN** the renderer draws the same ASCII run with two different foreground colors in the same frame
- **THEN** both draws SHALL hit the atlas, neither SHALL create a new `SDL_Texture`, and `render.glyph_atlas_hits` SHALL increment by the number of glyphs drawn across both runs

#### Scenario: Color-mod is restored after each run
- **WHEN** the renderer draws a sequence of differently-colored atlas runs followed by a fallback composite draw
- **THEN** the fallback draw SHALL observe the texture color-mod as set by its own path, not as a stale value left over from the preceding atlas run

### Requirement: Atlas-Eligible Run Selection

A draw run SHALL use the atlas fast path when every codepoint in the run lies within `0x20`..`0x7E` inclusive. Any non-ASCII codepoint in a run SHALL force the entire run onto the fallback composite path. Run boundaries SHALL be respected — the renderer SHALL NOT split a single run across atlas and fallback paths. `render.glyph_atlas_fallbacks` SHALL increment exactly once per run routed to the composite path on an atlas-eligible-from-the-outside-but-non-ASCII-inside boundary.

#### Scenario: ASCII run goes through the atlas
- **WHEN** a same-color run containing only ASCII codepoints is drawn
- **THEN** the atlas fast path SHALL emit one batched `SDL_RenderTextures` call (or platform equivalent) covering every glyph in the run, and `render.glyph_atlas_hits` SHALL increment by the glyph count

#### Scenario: Mixed-codepoint run falls back
- **WHEN** a same-color run contains at least one codepoint outside `0x20`..`0x7E`
- **THEN** the entire run SHALL render through the composite fallback path, the atlas SHALL NOT be consulted for that run, and `render.glyph_atlas_fallbacks` SHALL increment by 1

#### Scenario: Adjacent runs route independently
- **WHEN** an ASCII run is immediately followed by a non-ASCII run with the same color
- **THEN** the ASCII run SHALL go through the atlas and the non-ASCII run SHALL go through the fallback, and the renderer SHALL NOT merge the two into a single composite draw

### Requirement: Batched Glyph Submission

The atlas fast path SHALL submit all glyphs of a same-color run via a single batched API call (e.g. `SDL_RenderTextures` with parallel `src_rects` and `dst_rects` arrays), not one `SDL_RenderTexture` call per glyph. The renderer SHALL reuse a per-thread scratch buffer for the source-rect / destination-rect arrays so the hot path performs no heap allocation per draw.

#### Scenario: Single SDL call per run
- **WHEN** an atlas-eligible run of N glyphs is drawn
- **THEN** the backend SHALL issue exactly one batched glyph-submission call covering all N glyphs

#### Scenario: Hot path performs no heap allocation
- **WHEN** the harness scenario `scroll_unique_code_lines` runs with the atlas enabled
- **THEN** `AssertNoAllocationsDuringDraw()` SHALL hold across the steady-state portion of the scenario

### Requirement: Atlas Steady-State Counters

The renderer SHALL expose three perf counters in `util::PerfCounterId` and the `microide_perf` JSON report: `render.glyph_atlas_hits` (incremented per glyph routed through the atlas), `render.glyph_atlas_fallbacks` (incremented per run routed to the composite fallback), and `render.glyph_atlas_evictions` (incremented when the atlas texture is destroyed and rebuilt, e.g. on font reload). In a steady-state warm scenario `render.glyph_atlas_evictions` SHALL be 0.

#### Scenario: Counters are reported in JSON
- **WHEN** `microide_perf --report-json=…` runs any scenario
- **THEN** the resulting JSON SHALL include the three counter names under `perf_counters` whenever they are non-zero

#### Scenario: Steady-state atlas has no evictions
- **WHEN** `scroll_unique_code_lines` runs to completion with the atlas enabled
- **THEN** `render.glyph_atlas_evictions` SHALL be 0 across all iterations

### Requirement: Fallback Parity And Optional Flag

The atlas fast path SHALL produce visually identical output to the existing composite path for ASCII-only runs. The change SHALL land behind an opt-in environment flag (`MICROIDE_RENDER_GLYPH_ATLAS=1`) until perf and parity are validated on `perf-runner-v1`; the flag default SHALL flip to enabled once the new scenario passes on the reference runner.

#### Scenario: Atlas output matches composite for ASCII
- **WHEN** a fixture renders ASCII-only content with and without `MICROIDE_RENDER_GLYPH_ATLAS=1`
- **THEN** the resulting pixel buffers SHALL be byte-identical (or within the documented anti-aliasing tolerance for the SDL3_ttf version in use)

#### Scenario: Flag default is enabled after perf gate passes
- **WHEN** the change record includes a `perf-runner-v1` JSON report showing the success criteria in the proposal are met
- **THEN** the flag's compiled-in default SHALL be on, and the env var SHALL only be needed to opt **out** (`MICROIDE_RENDER_GLYPH_ATLAS=0`) for triage
