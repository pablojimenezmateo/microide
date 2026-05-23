## 1. Atlas Build + Counter Plumbing

- [ ] 1.1 Add `render.glyph_atlas_hits`, `render.glyph_atlas_fallbacks`, `render.glyph_atlas_evictions` to `util::PerfCounterId` and `kCounterNames` in `src/util/PerformanceCounters.{h,cpp}`. Verify the existing `microide_perf --report-json` smoke run prints them under `perf_counters` once they get incremented.
- [ ] 1.2 Add a `GlyphAtlas` struct (private to `src/render/SdlTtfTextBackend.cpp` or a new sibling TU) holding `SDL_Texture* texture`, `std::array<SDL_FRect, 95> src_rects`, `int glyph_height`, `int max_advance`. Define `BuildAsciiGlyphAtlas(TTF_Font*, SDL_Renderer*)`.
- [ ] 1.3 Build the atlas in `TextRenderer::EnsureInitialized` (or equivalent font-load entrypoint) keyed on `(font face, font size)`. Increment `render.glyph_atlas_evictions` only when an existing atlas is destroyed and rebuilt. Done when: a one-shot test asserts `evictions == 0` after a single init and `== 1` after a forced font reload.
- [ ] 1.4 Add a startup-trace scope around atlas build so its cold-init cost is visible under `MICROIDE_STARTUP_TRACE=1`. Done when the scope name appears in a trace capture.

## 2. Fast Path Behind A Flag

- [ ] 2.1 Add `MICROIDE_RENDER_GLYPH_ATLAS` runtime flag (read once at init, cached on the backend). Default off for the first commit.
- [ ] 2.2 Add per-thread scratch buffers (`thread_local std::vector<SDL_FRect>` for `src_rects` and `dst_rects`) on `SdlTtfTextBackend`, mirroring the `terminal_foreground_runs_scratch_` pattern. Hot path SHALL `clear()` them, not `vector{}` them.
- [ ] 2.3 Add `IsRunAtlasEligible(std::string_view)` predicate that returns true iff every byte is `< 0x80` (i.e. every codepoint is `0x20..0x7E`; control chars and tabs are filtered upstream).
- [ ] 2.4 Add the atlas fast-path inside `SdlTtfTextBackend::DrawRun` (or the equivalent run-draw entrypoint): when `IsRunAtlasEligible(run)` is true and the flag is on, build `src_rects` + `dst_rects` from `atlas.src_rects[c - 0x20]` and the run's screen position, call `SDL_SetTextureColorMod(atlas.texture, r, g, b)`, then `SDL_RenderTextures(atlas.texture, src_rects.data(), dst_rects.data(), n)`. Increment `render.glyph_atlas_hits` by `n`.
- [ ] 2.5 Route every other run (non-ASCII or flag-off) through the existing composite path and increment `render.glyph_atlas_fallbacks` exactly once when a run was eligible-looking-from-outside but contains non-ASCII bytes.
- [ ] 2.6 Re-run the full `microide_perf --smoke` smoke set with the flag off and confirm no regressions versus the pre-change numbers in `dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md`.

## 3. Parity And Sanitizers

- [ ] 3.1 Add a parity fixture (a new `microide_tests` test, not a perf scenario) that renders a fixed ASCII string twice — once with the atlas flag on, once with it off — into in-memory SDL surfaces and asserts pixel parity within a documented tolerance. Use the same `SDL_PIXELFORMAT` for both captures.
- [ ] 3.2 Run `cmake --preset microide-asan && ctest --output-on-failure` with the atlas flag on. Done when 100 % pass.
- [ ] 3.3 Run `cmake --preset microide-ubsan && ctest --output-on-failure` with the atlas flag on. Done when 100 % pass.
- [ ] 3.4 Run `cmake --preset microide-tsan && ctest --output-on-failure` with the atlas flag on. Done when 100 % pass.

## 4. Perf Scenario + Counter Surfacing

- [ ] 4.1 Generate a 5 000-line syntax-colored C++ fixture under `tests/perf/fixtures/scroll_unique_code_lines/`. Lines must be distinct enough that a per-string color-keyed texture cache thrashes. Commit the SHA-256 alongside.
- [ ] 4.2 Register a `scroll_unique_code_lines` scenario in `tests/perf/PerfMain.cpp`: open the fixture, scroll once to warm caches, then measure a second pass via `PumpFrames` + `Scroll` per frame. Mark `.smoke = true`.
- [ ] 4.3 Add a scenario-level assertion that `render.glyph_atlas_evictions == 0` and `text_texture_cache_misses` / `cells_visited` < 1 % over the measurement window. The cells-visited metric needs a small new counter; wire it in `EditorViewRenderer::Render` row paint.
- [ ] 4.4 Commit an initial baseline for `scroll_unique_code_lines` from `perf-runner-v1` (or, locally, mark as advisory until the gate runs). The change record must include a `perf-baseline:` line.

## 5. Flip The Default + Tighten Budgets

- [ ] 5.1 Flip the compile-in default of `MICROIDE_RENDER_GLYPH_ATLAS` to on. Confirm the smoke suite still passes; `editor_render_whitespace_paint` p50 SHALL be ≥ 30 % below its pre-change baseline and `editor_sticky_scroll_scroll` p50 SHALL be ≥ 20 % below. Document the deltas in the PR.
- [ ] 5.2 Update the affected baselines in `tests/perf/baselines/` (`editor_render_whitespace_paint.json`, `editor_sticky_scroll_scroll.json`, plus any other scenarios whose numbers shift outside tolerance). Add the required `perf-baseline: glyph atlas landed` line to the change record.
- [ ] 5.3 Add an architectural-lint rule in `tests/ArchitectureInvariantsTests.cpp` that forbids direct atlas access outside `src/render/SdlTtfTextBackend.cpp` (regex on `GlyphAtlas` references).
- [ ] 5.4 Update `dev-docs/performance/investigations/performance-bottleneck-deep-dive-2.md` Finding 15 status to `done` and add a one-line carry-over note in `dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md`'s status table.
- [ ] 5.5 Capture the final before/after numbers and link the `perf-runner-v1` JSON report from the PR description.
