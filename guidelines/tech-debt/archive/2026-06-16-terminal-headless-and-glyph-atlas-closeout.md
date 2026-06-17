# 2026-06-16 terminal closeout, headless app lifecycle, and glyph-atlas guardrail (§26, §27, §13)

- Date: 2026-06-16
- Area: terminal, app, rendering
- Source: §26, §27, §13; commits `26246233` (coverage atlas), terminal/app closeout commits
- Related guardrail: **the editor glyph-atlas *draw-path* (GPU/`SDL_RenderGeometry`) stays rejected
  and is gated by the §13 preconditions below — do not retry without all three.** Linked from
  `dev-docs/project/known-tech-debt.md` and
  `dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md`.

## Summary

The three deferred terminal items (alt-screen copy, escape-file split, full output fuzzer) and the
deferred headless `Application` lifecycle test were closed, and the glyph-atlas history was resolved:
the rejected GPU/per-quad draw-path variant stays rejected under fixed preconditions, while the
endorsed colour-independent coverage atlas on the composite-on-miss path shipped.

## Resolution — §26 terminal deferred-debt closeout (T3, T5a, output fuzzer)

- **alt-screen scrollback copy (T5a)**: `SaveActiveScreenLocked` used to deep-copy the primary
  scrollback (`std::deque<TerminalLine>`, up to ~2000 lines) on every alternate-screen enter, with the
  symmetric copy back on exit — firing whenever a TUI app starts/stops. The two screen buffers are
  never live simultaneously, so the transition is now a buffer move/swap:
  `SaveActiveScreenMetadataLocked` persists only cursor/scroll-region scalars; `SetAlternateScreenLocked`
  hands the active grid to its backing store via `std::move`; `RestoreSavedScreenLocked` moves the
  target buffer into `lines_`. O(scrollback) → O(1) moves + scalar copies. The "alternate screen never
  initialized" check (keyed on `alternate_screen_.lines.empty()`) is captured before the move into a
  `should_clear` flag (else mode 47/1047 re-entry would wrongly clear restored content). New tests
  `TerminalSession/AlternateScreenPreservesPrimaryScrollback`,
  `AlternateScreenReentryPreservesAltContent`; perf scenario `terminal_alt_screen_toggle`.
- **escape-file decomposition (T3)**: the 732-line `TerminalSessionEscape.cpp` split by sequence
  family into `TerminalSessionCsi.cpp`, `TerminalSessionSgr.cpp`, `TerminalSessionOsc.cpp`,
  `TerminalSessionModes.cpp`. Handlers stay `TerminalSession` members; the only cross-TU seam is
  `detail::ApplySgrParameters` in `src/terminal/TerminalSessionEscapeInternal.h`. Pure relayout. The
  arch-lint allowlist (`CheckTerminalSessionSplitTranslationUnits`) names the four new TUs.
- **full terminal session-output fuzzer**: new `tests/fuzz/TerminalSessionOutputFuzz.cpp` drives
  `AppendOutputLocked` end-to-end via `TerminalSessionTestAccess::AppendOutput` (broader than the
  CSI-only fuzzer). Wired under `MICROIDE_FUZZ` + `MICROIDE_TESTING=1` (the test-access seam and class
  layout must agree across every TU or it is an ODR mismatch). Seed corpus covers plain text, SGR,
  cursor/erase, OSC, mode toggles, DCS/APC string payloads, mixed/invalid UTF-8, overflow params.

Validated: full `ctest` green; ASAN preset green; fuzz body compiled GCC+ASAN/UBSAN and run over the
seed corpus + 80 randomized inputs.

## Resolution — §27 `app` headless lifecycle test (A2 closeout)

Both recorded blockers dissolved on inspection:
- "the software renderer cannot exercise the render-target texture path" was an untested assumption —
  under the dummy video driver `SDL_CreateRenderer(window, nullptr)` returns the **window-backed**
  software renderer, which accepts `SDL_TEXTUREACCESS_TARGET`/`SDL_SetRenderTarget`, so the real
  `SceneTexturePresenter`/retained-scene path runs headless.
- "`Initialize`/`Shutdown` are private" resolved with a narrow friend seam
  `microide::tests::ApplicationTestAccess` (friends are only banned in `src/workspace/*`).

Real bug fixed: `Application::Shutdown()` destroyed the window/renderer but never reset `initialized_`,
so it was neither idempotent nor re-initializable in-process. It now clears
`initialized_`/`first_render_complete_` and re-arms `presentation_state_dirty_` before the
`quick_exit` gate. Tests in `tests/ApplicationTests.cpp`:
`HeadlessInitializeAndShutdownTearsDownCleanly`, `HeadlessRendersRetainedSceneFrame`,
`HeadlessReinitializeAfterShutdown`, `HeadlessDestructorShutsDownInitializedApp` (all redirect
HOME/XDG/APPDATA into a `TemporaryDirectory`, run `safe_mode`/`disable_plugins`). The dummy driver's
benign `SDL_SetWindowHitTest failed` log per `Initialize()` is ignored noise.

## Resolution — §13 editor glyph atlas

### Rejected variant (do NOT revisit without all three preconditions)

The 2026-05-15 experiment built one alpha-only `SDL_Texture` ASCII atlas with an `SDL_RenderGeometry`
fast path in `SdlTtfTextBackend`. End-to-end working, but wall-time regressed on every editor paint
scenario on the software renderer: `editor_render_whitespace_paint` +81%, `editor_sticky_scroll_scroll`
+83%, `editor_indent_guides_paint` +48%. Reverted in full; `MICROIDE_RENDER_GLYPH_ATLAS` no longer
exists.

Why it was wrong: the composite texture cache is at >99% hit rate on every paint scenario, so
cache-miss thrash is not the bottleneck; `DrawString` is called at the run level, not the cell level;
`SDL_RenderGeometry` in the software renderer forces per-pixel attribute interpolation, submitting
strictly more per-pixel work than the composite blit it replaced.

**Do not propose another draw-path glyph-atlas variant unless all three hold:**
1. MicroIDE is rendering through a **GPU backend, not software** (the software path remains the
   perf-gated path).
2. A **measured fixture exists where `render.text_texture_cache_misses / cells_visited` exceeds ~10%**
   in steady state (today < 1%).
3. A trace shows `BuildAsciiCompositeSurface` / `SDL_CreateTextureFromSurface` as a **top-3 hotspot**
   on `perf-runner-v1` (today far below the per-pixel blit cost).

If those ever hold, the right shape is "reuse atlas data to build a composite string texture on cache
miss" — preserving the one-`SDL_RenderTexture`-per-cached-string draw shape — not per-quad geometry.
The OpenSpec record is kept at `openspec/changes/text-renderer-glyph-atlas/` for archaeology only;
**do not apply.**

### What shipped instead (2026-06-16): coverage atlas on the composite-on-miss path

`src/render/AsciiGlyphAtlas.{h,cpp}` holds one **colour-independent** white coverage atlas (printable
ASCII `0x20..0x7E`), built lazily and rebuilt only on font-size change.
`SdlTtfTextBackend::BuildAsciiCompositeSurface` blits glyphs from atlas sub-rects, tinting via
`SDL_SetSurfaceColorMod`, to assemble the same composite string surface as before. The **hit path is
byte-identical** (one `SDL_RenderTexture` per cached `(text, colour)` string), so the gated >99%-hit
paint scenarios never enter the miss path and stay green. The win is on the miss path and in
footprint: it removes the previous per-`(char, colour)` glyph-surface cache (512-entry LRU) that
re-rasterized the same glyph shape once per syntax colour. Pixel-identity guarded by
`tests/TextRendererTests.cpp::TestAsciiGlyphAtlasMatchesPerColorRendering` (0-pixel-diff vs
`TTF_RenderText_Blended`) and `...CoversPrintableRange`. Opaque colours only; translucent text keeps
the whole-string `TTF_RenderText_Blended` fallback. The §13 preconditions still gate any return to a
draw-path atlas; they do not apply to this miss-path coverage atlas.
