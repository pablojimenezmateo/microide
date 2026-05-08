# Performance Hotspot Ledger

Generated from `tests/perf/hotspot-scan-report.json` (`--iterations=1`) and follow-up validation runs.

## Ranked Opportunities

1. **Idle wake churn (`idle_soak_30s`)**
   - Evidence: `~30.1s` wall in soak window, `~11.2M` allocations, `~4.3k` unexpected wake events.
   - Expected impact: high (idle CPU + wake regression risk).
   - Complexity: medium-high.
   - Ownership: workspace event loop + scheduled wake producers.
   - Verification: `idle_soak_30s`, `long_soak_8h`, adjacent `switch_and_idle`.

2. **Long soak throughput (`long_soak_8h`)**
   - Evidence: `~3.1s` default soak wall, `~2.8M` allocations in one iteration.
   - Expected impact: high for long-session stability.
   - Complexity: medium.
   - Ownership: watcher scheduling + background wake surfaces.
   - Verification: `long_soak_8h` plus `idle_soak_30s`.

3. **Tab cycling and tab-open churn (`multi_tab_cycle`)**
   - Evidence: `~1285.9ms` wall, `~2.25M` allocations.
   - Expected impact: high for common editor navigation.
   - Complexity: medium-high.
   - Ownership: editor tab service, render prep path, tab activation plumbing.
   - Verification: `multi_tab_cycle`, adjacent `typing_large_file`.

4. **Diagnostics publish latency (`linter_on_save`)**
   - Evidence: `~1011.9ms` wall.
   - Expected impact: medium-high for save feedback loops.
   - Complexity: medium.
   - Ownership: plugin diagnostics + async callback pump.
   - Verification: `linter_on_save`, adjacent `search_first_result`.

5. **Terminal scroll burst path (`terminal_scroll_long_output`)**
   - Evidence: `~97.4ms` wall, `~516k` allocations; phase timing added (`terminal.open`, `terminal.initial_wait`, `terminal.scroll_burst`).
   - Expected impact: medium.
   - Complexity: medium.
   - Ownership: terminal panel and bottom-panel render integration.
   - Verification: `terminal_scroll_long_output`, adjacent `scroll_large_file`.

## Implemented Slice In This Pass

- Added lightweight phase-timing seams in `ScenarioContext` (`Measure`, per-iteration `phase_durations_ms`) so hot scenario segments can be isolated without ad hoc tracing.
- Added terminal-phase timing instrumentation to `terminal_scroll_long_output`.
- Added `window_resize_stress` scenario + baseline to close resize-path coverage gap.
- Reduced harness idle polling overhead in `ScenarioContext::Wait` (sleep on no handled wake instead of immediate 1 ms churn loop).

## Baseline Note

`perf-baseline: add window_resize_stress baseline to gate resize-path regressions discovered by the hotspot audit.`
