## ADDED Requirements

### Requirement: `scroll_unique_code_lines` Scenario

The harness SHALL register a `scroll_unique_code_lines` scenario that opens a fixture of at least 5 000 distinct syntax-colored lines, warms the renderer by scrolling once through the file, then scrolls through the file a second time and measures steady-state metrics on the second pass. The scenario SHALL run on the standard fixed window and software renderer.

#### Scenario: Scenario is registered with the harness
- **WHEN** `microide_perf --list` (or equivalent enumeration) runs
- **THEN** `scroll_unique_code_lines` SHALL appear in the registered scenario list and SHALL be runnable via `--scenarios=scroll_unique_code_lines`

#### Scenario: Steady-state texture-cache misses are bounded
- **WHEN** `scroll_unique_code_lines` reaches its post-warm-up measurement window with the atlas enabled
- **THEN** `render.text_texture_cache_misses` divided by the total cells visited in the measurement window SHALL be below 1 %

#### Scenario: Steady-state atlas evictions are zero
- **WHEN** `scroll_unique_code_lines` reaches its post-warm-up measurement window with the atlas enabled
- **THEN** `render.glyph_atlas_evictions` SHALL be 0 for every iteration of the scenario

### Requirement: Glyph-Atlas Counter Family Is Reportable

The harness JSON report SHALL include the three glyph-atlas counters (`render.glyph_atlas_hits`, `render.glyph_atlas_fallbacks`, `render.glyph_atlas_evictions`) under `perf_counters` for every scenario where the counter delta is non-zero. The counters SHALL participate in the `--report-json` / `--report-text` output identically to existing render counters.

#### Scenario: Counters surface in --report-json
- **WHEN** a scenario that exercises the editor text path runs with `--report-json=<path>` and the atlas enabled
- **THEN** the resulting JSON SHALL include `render.glyph_atlas_hits` (non-zero) and SHALL include `render.glyph_atlas_evictions` only when non-zero, both under each iteration's `perf_counters` block
