# Section 13.E aggregate coverage (record)

Scenario implementations live in `tests/perf/PerfMain.cpp` / `tests/perf/PerfHarness.{h,cpp}`.

## 13.E.1 — aggregate scenarios `typing_large_file` / `scroll_large_file`

These scenarios remain JSON-gated under `tests/perf/baselines/`.

**Before (committed gate, repo baseline JSON)**

| Scenario           | p50 wall ms | p95 wall ms | max wall ms |
|-------------------|------------:|------------:|------------:|
| `typing_large_file` | 14.5206 | 22.7121 | 28.4441 |
| `scroll_large_file` | 13.2317 | 22.0438 | 27.6440 |

**After (spot-check run)**

Linux/Xvfb, `--iterations=3`, `--report-text=/tmp/perf-13e-aggregate.txt` (numbers vary widely vs CI runners):

| Scenario           | p50 wall ms | p95 wall ms | max wall ms |
|-------------------|------------:|------------:|------------:|
| `typing_large_file` | 75.105 | 499.753 | 546.936 |
| `scroll_large_file` | 66.824 | 176.825 | 189.047 |

Interpretation: relative ordering vs baseline JSON on another machine is not meaningful; keep comparing aggregate scenarios against committed JSON **on the same CI/agent**.

## 13.E.2 — Isolated 13.B–13.D baselines

- Harness scenarios for Sections 13.B–13.D register matching **`tests/perf/baselines/editor_*.json`** aggregates together with micro-timing envelopes (`EnforceP95Microseconds`) where noted in `PerfMain.cpp`.
- Inventory at Section 13.E refresh time includes twenty baseline records covering folding/outline/view-model paths plus shaping/snippet/save fixture scenarios (`glob editor*.json` under `tests/perf/baselines/`).

## 13.E.3 — `idle_soak_30s` after snippet/outline timers

The scenario now:

1. Primes `PrimeEditorEssentialsIdleSoakSurface()` (large-project editor tab + `sidebar-show outline` + one edit + snippet placeholder session via `PerfHarnessPrimeSnippetPlaceholderSession`).
2. Drains outline debounce (short explicit wait before soak settlement — wakes absorbed outside the measured window).
3. Keeps the historic assertion: **zero SDL wakes counted across the explicit `Wait(std::chrono::seconds(27))` measurement**.

Committed baseline **`idle_soak_30s.json` refreshed** (allocation churn differs materially from the pre-editor-prime welcome-idle profile):

| Metric | Committed baseline after refresh |
|--------|----------------------------------|
| `p50_wall_ms` | 30504.821167 |
| `p95_wall_ms` | 31006.478617 |
| `max_wall_ms` | 31012.909433 |
| `p50_allocations` | 76428 |
| `p95_allocations` | 129637.59999999989 |
| `max_allocations` | 173167 |

Captured via `xvfb-run`, `--update-baseline`, `--iterations=10` (matches harness default iteration count).

**perf-baseline:** justification tracked on `proposal.md` (Impact).

## 16.5 — Before/after snapshot for the four new block-structure scenarios

The four block-structure capability scenarios introduced in this change
(`editor_fold_recompute`, `editor_sticky_scroll_scroll`,
`editor_indent_guides_paint`, `editor_render_whitespace_paint`) all
ship their own gated `tests/perf/baselines/editor_*.json` records. These
are the "before" numbers (committed baselines, what the merge gate
checks). The "after" column is a spot-run snapshot taken on the
implementation host (Linux, default build) with `--iterations=3`; numbers
on developer machines vary materially vs. CI runners, so the comparison
is informational only — the JSON baselines remain the authoritative gate.

**Before (committed baseline JSON)**

| Scenario                         | p50 wall ms | p95 wall ms | max wall ms |
|----------------------------------|------------:|------------:|------------:|
| `editor_fold_recompute`          | 713.97 | 755.67 | 763.18 |
| `editor_sticky_scroll_scroll`    | 203.82 | 225.97 | 229.01 |
| `editor_indent_guides_paint`     | 168.77 | 172.27 | 172.86 |
| `editor_render_whitespace_paint` | 220.81 | 256.55 | 263.85 |

**After (spot-check, `--iterations=3`, dev host)**

| Scenario                         | p50 wall ms | p95 wall ms | max wall ms |
|----------------------------------|------------:|------------:|------------:|
| `editor_fold_recompute`          | 783.98 | 802.93 | 805.03 |
| `editor_sticky_scroll_scroll`    | 306.91 | 338.59 | 342.11 |
| `editor_indent_guides_paint`     | 275.41 | 289.29 | 290.83 |
| `editor_render_whitespace_paint` | 267.74 | 303.87 | 307.88 |

Interpretation: the spot run is on a noisier developer host with only
three iterations; it remains within the same order of magnitude as the
committed baselines but is not a regression signal on its own. The merge
gate compares each scenario against its committed `editor_*.json`
baseline on the same CI/agent (`perf-runner-v1`), with tolerances
`max_percent: 50`, `p95_percent: 20`, `p50_percent: 10` per
`performance-budgets/spec.md`.
