# Perf Harness

The perf harness is the primary regression oracle for startup and interactive-performance changes.
It runs scenario workloads through `microide_perf` and compares measured aggregates against committed
baselines.

## What These Numbers Are And Are Not

The harness exists to detect microide-vs-itself regressions. It is not a tool for comparing
microide against other editors.

What committed baselines under `tests/perf/baselines/` reliably tell you:

- whether a change regressed a specific scenario versus the previously committed baseline on the
  same runner class (`perf-runner-v1`) with the same SDL driver hints, seed, and fixtures
- whether idle behavior holds the zero-wake invariant over the soak window
- whether a hard-coded gate threshold (e.g. `file_finder_cold` ≤ 50 ms) still holds

What they do **not** tell you:

- how microide compares to VSCode, Zed, Helix, Sublime, or any other editor — no comparative
  measurement is performed, none is published, and the existing numbers are not meaningful in
  that comparison
- behavior under a GPU-accelerated renderer or a real window system by default; the reference
  (gated) lane pins `--renderer=software --video=dummy`, and the harness applies both itself. Two
  **advisory lanes** exist: `--renderer=auto|<sdl-driver>` for GPU-only paths like the batched glyph
  atlas, and `--video=x11|wayland|auto` for window-system present cost. Both are printed but never
  gated or written to baselines (neither is cross-machine portable), exactly like the DAP advisory
  scenarios
- behavior on other hardware than `perf-runner-v1`; cross-machine numbers are advisory
- whether LTO "proves" cross-translation-unit extractions are free. LTO can recover some inlining
  loss, but residual sticky-scroll/render-path regressions still need direct profiling and explicit
  fix-or-accept decisions

Treat the perf harness as a regression alarm, not a marketing instrument. If you write commit
messages, PR descriptions, or release notes, use phrasing like "no regression on
`<scenario>`" or "improves `<scenario>` p50 from X to Y on `perf-runner-v1`," not "fastest" or
"X% faster than $other_editor."

## Gating Policy: Loose Wall, Exact Allocations

The two gated metrics are deliberately asymmetric, and reading a result depends on
knowing which one fired.

- **Allocation counts are the oracle.** The counting `operator new`/`delete` counts
  **per thread** (`tests/perf/AllocationCounter.cpp`), so a scenario reports what its
  own thread did and nothing else. That number is byte-identical run to run — across
  three full gated runs of one unchanged binary it never moved once. It scales with
  the work performed, so it catches an added allocation on a hot path, a cache that
  stopped hitting, and an O(n) walk that became O(n²), and it catches them precisely.
  Default tolerances stay tight: **10 / 20 / 50 %**. Never widen an allocation
  envelope to make a red gate green — if a count is not reproducible, the scenario is
  measuring something it should not, and the fix is to make the scenario deterministic
  (see `file_finder_cold`, which waits for the file index rather than letting a
  background rebuild land in a random subset of its iterations).
- **Wall times are advisory-by-tolerance.** This shared reference runner lands in
  stable CPU modes tens of percent apart; the same three full runs produced 0, 3 and 0
  wall failures against baselines captured from that same binary, with overshoots
  above +150 % on the loaded run. A 10 % wall gate here is a coin flip, and a suite
  that is red on a third of its runs detects nothing because nobody reads it. Defaults
  are **100 / 150 / 200 %** — still enough to catch an algorithmic blowup.

What this gives up is a constant-factor wall regression under ~2x. That is the job of
the interleaved `tools/perf-compare.py` current-vs-main run, where shared machine load
cancels because both sides pay it.

Shared tolerance constants live in `tolerance::` in `tests/perf/PerfHarness.h`; prefer
them over fresh numbers.

## CPU Time And Resident Growth

Wall time and allocation counts were the only two gated metrics for the first 93
baselines, which left priorities 3 (low CPU) and 4 (low memory) stated but not
measured. Two more metrics close that:

- **`cpu_ms`** — process CPU time, user + system, from `getrusage(RUSAGE_SELF)`,
  which sums **every thread**. This is the metric wall time structurally cannot
  provide: a change that holds its latency by moving work onto the background
  executor is neutral on wall and a large regression here. Tolerances default to
  the wall envelopes, since CPU carries the same scheduler jitter.
- **`rss_growth_bytes`** — resident-set delta across one iteration, from
  `/proc/self/statm`. Deliberately the delta and not absolute RSS: the harness runs
  every iteration of every scenario in one process, so absolute RSS is dominated by
  whatever ran earlier and is not attributable to the scenario. Growth is. Steadier
  than wall but not as deterministic as an allocation count (page granularity,
  allocator arena behaviour), so tolerances sit between the two: **25 / 35 / 60 %**,
  with a one-page (4 KiB) noise floor so a near-zero baseline cannot turn a single
  page of jitter into an unbounded regression.

This is separate from the existing hard RSS ceiling in `AdvisoryPerfScenarios.cpp`
(256 MiB idle, 160 MiB growth). That is a ceiling; these are ratchets. A change
taking idle RSS from 60 MB to 250 MB passed the ceiling silently and now does not.

**Existing baselines are not gated on them.** A baseline that predates these metrics
records neither, and `LoadBaseline` marks it ungated rather than comparing against
an implicit `0.0` — which would have failed all 93 scenarios the moment the metrics
shipped. They start gating when the baseline is next re-recorded on the reference
runner, which is a deliberate act like any other rebaseline.

Run-to-run stability, measured over four independent runs of one unchanged binary
on the reference runner (`editor_column_selection_burst`, 10 iterations each — note
the iteration-count caveat in the sweep section below):

| metric | spread |
| --- | --- |
| `p50_allocations` | 4,247 every run — the oracle, as documented |
| `p50_rss_growth_bytes` | 210,944 bytes every run — bit-identical |
| `p50_cpu_ms` | 15.31 – 20.35 (**33 %**) |

So resident growth is gateable at a tight envelope and CPU is not. CPU inherits the
scenario's *resolved* wall envelope rather than the tolerance struct's default —
without that, a scenario with a widened 100 % wall tolerance would carry a 10 % CPU
tolerance and flag on its own recorded baseline immediately.

### What the first full CPU/RSS sweep found: almost nothing

A 95-scenario sweep was run once the metrics existed. The result is a clean bill of
health, and the way it was nearly misread is the useful part.

**At 5–6 iterations the data looked alarming.** `typing_large_file` reported 2.02 ms
wall against 3.97 ms CPU, `scroll_large_file` 1.71 against 3.12 —
apparently ~2× wall spent on other threads during the two hottest interaction paths.
`editor_buffer_find_incremental` reported 2.9 MB of resident growth *per iteration*,
which reads exactly like a leak.

**All of it was the cold pass.** Re-measured at 20 iterations, per-iteration:

| scenario | iter 0 ratio | steady-state ratio |
| --- | ---: | ---: |
| `typing_large_file` | 2.01 | **1.00** |
| `scroll_large_file` | 2.37 | **1.01** |
| `file_finder_cold` | 1.75 | **1.03** |
| `git_sidebar_activate` | 1.95 | **1.08** |
| `multi_project_switch` | 1.93 | **1.54** |

`editor_buffer_find_incremental` grows 19.3 MB on iteration 0, then 4.6 / 1.5 / 5.0 MB,
then **exactly zero for every iteration after the third**, with allocations flat at
3,476. That is an allocator arena reaching steady state, not a leak.

This is the "percentiles land on the cold pass" trap documented below, in a new
dimension: thread-pool spin-up and arena growth are *first-iteration* costs, so CPU
and RSS are far more cold-sensitive than wall time. **Never read a cpu_ms or
rss_growth_bytes number off fewer than ~20 iterations, and prefer the per-iteration
series to the percentile.** The p50 of six samples sits on a warming iteration.

What survives: 94 of 95 scenarios run at a steady-state CPU/wall ratio of ≤1.1, and
resident growth reaches zero within four iterations everywhere checked. The single
exception is `multi_project_switch` at 1.54 (22.5 ms wall, 34.7 ms CPU) — concurrent
file-index, git and language-server work for the new root, plausibly by design, and
the one place worth a look if project-switch CPU ever matters.

`repo_open_rss_idle` is worth knowing separately: 503.79 ms wall against 5.44 ms CPU.
Its half-second "wall time" is a soak sleep, not work — read its allocation and CPU
numbers, never its wall number.

## Reading A Measurement

Most "regressions" this harness reports are not regressions. The heuristics below
came out of chasing ones that were not, and they are cheap to apply before
spending a session on a number.

### Re-run anything flagged at 25 iterations

`tools/perf-compare.py` defaults to `ITERATIONS=10` — five samples per side. That
is too few here: many scenarios have a 5-6x p50/p95 spread from a cold first
iteration, so the median of five swings wildly and the 2σ band does not filter it
out.

Evidence: in one review pass 8 of 9 flagged "regressions" (+16 % to +54 %)
dissolved or flipped to improvements at `ITERATIONS=25`; only one survived. In a
later pass all four flagged scenarios collapsed, two into improvements — one that
had read as "+75.8 % max_wall" came back **9.5 % faster** across all three
percentiles.

### Triage heuristics

- **Identical allocation counts on both sides means the same work is being done.**
  A wall delta there is code layout, inlining, or machine noise — never an
  algorithmic regression. This is the single most useful discriminator.
- `max_wall_ms`-only movement with a flat p50 is a single-sample outlier. Ignore
  it.
- p50 **and** p95 **and** max all moving the same direction is a real signal.
- Check whether the change even touches what the scenario exercises:
  `git diff --stat <base>..HEAD -- src/<subsystem>/`.
- Compute the mean p50 delta across all scenarios as a bias check. ~0 % with a
  roughly even slower/faster split means no systematic regression.

`tools/perf-compare.py <commit>` also bisects: pass an intermediate commit to find
where a delta appeared.

### Measure on a quiet machine

The machine is a lane, exactly like the video and CPU-cluster lanes. A full gate
run once flagged `external_change_refresh_open_diff` at 67.3 ms against a 28.5 ms
baseline (+136 %) because it was launched while `perf-compare.py` was tearing down
two multi-GB worktree builds. The same binary measured 26.5 ms standalone, 25.5 ms
in the git set, and 29.8 ms in a clean full suite, with allocations flat (~61.5k)
in all four. A file-I/O + subprocess scenario is where concurrent teardown shows up
first.

Finish other jobs, confirm nothing is running (`pgrep -x microide_perf`, no
builds), then measure.

### The scenario may not measure the thread you changed

The allocation counter is **per thread** and most scenarios measure the shell
thread, so work you moved off it is invisible here by design — but so is work you
*optimised* off it.

`terminal_scroll_long_output` is the standing example: it is render/scroll-bound,
while terminal VT parsing (`AppendOutputLocked` → `PutGlyphLocked` /
`PutAsciiRunLocked`) runs on the reader thread. A ~25x parse-throughput win
(per-char → bulk-ASCII) moved this scenario by **1.00x**. To measure the parse
path, microbench `TerminalSessionTestAccess::AppendOutput` directly on a large
printable-ASCII blob and time it — that is how a 35.5 ms → 1.4 ms per-256 KiB
result was obtained.

Before using a committed baseline as your "before", check it is not stale: the
committed `terminal_scroll_long_output` baseline once sat at 29,643 p50
allocations against a live ~2,221, which fabricates a huge phantom improvement.
Prefer a same-session A/B against a `main` worktree.

### Standalone vs full-suite gaps are a lead, not a known artifact

Scenarios in one `microide_perf` process share process-global state (SDL, fonts,
the glyph atlas, the heap), so a scenario can measure differently standalone than
in the full suite. Two examples:

- `cold_startup_no_project`: 590 p50 allocations standalone vs 165 in-suite (165
  is its committed baseline). Warm-vs-cold process state.
- `compare_scroll_selection`: 105 ms / 2 git spawns standalone vs 296 ms / 45 git
  spawns in-suite. That one was **a real bug**: a larger address space makes each
  `fork`+`exec` dear enough that inline blame loses the race against the scroll,
  and `GitBlameService` discarded every result whose viewport had moved — so the
  cache never got written, the re-validation throttle never had anything to hit,
  and every frame re-spawned the probe chain. Fixed in `c7d4f71b` (gate
  `GenerationsStillCurrent`, not `RequestStillCurrent`, for anything already paid
  for). 296 → 139 ms.

So: **diff the `perf_counters` block between the two runs** — that localised the
blame storm in one step (`subprocess.spawns` 2 vs 45,
`git.blame_validation_skips` 99 vs 0). Counters are in every `--report-json` per
iteration. But confirm the gap **reproduces on a clean run** first: if the only
movers are small (`subprocess.wait_ms`, a few extra rows) and allocations are
flat, it was load, not ordering.

An earlier version of this guidance said a `--scenarios=X` number is never
comparable to a committed baseline. That is too strong and it cost a session —
most scenarios reproduce their baseline standalone just fine.

### Percentiles land on the cold pass

At `--iterations=8`, p95 lands on iteration 0's cold pass (font/atlas fill,
initial file-index build, first session write). `cold_startup_no_project`
p95_allocations was 5373 at 8 iterations and 590 at 25 — equal to its own p50.
Fix this per scenario with `warmup_iterations = 1`, **not** by widening
tolerances.

GCC enforces designated-initializer order: `.warmup_iterations` must sit in
declaration order relative to the tolerance fields, or the scenario will not
compile.

### After an A/B

`git worktree remove --force` the throwaway checkout. `tools/perf-compare.py`
invokes the harness **per scenario**, so its numbers are standalone-equivalent —
it cannot see in-suite effects, and is not a substitute for a full gated run.

## Rebaselining

`--update-baseline` with no `--scenarios` rewrites every gated baseline, and that is
the intended way to use it after a change that moves many scenarios. Do it on an idle
machine, then run the suite a few times and confirm the failures that remain are wall
failures with byte-identical allocation counts — that is what "machine load, not a
regression" looks like.

Stale baselines fail silently: gates only trip on *increases*, so a baseline captured
before an improvement lets the scenario pass against a number no longer connected to
the code. The committed set had drifted 3–8x that way, in part because a bare
`--update-baseline` used to abort partway through on the first advisory-only scenario
it reached, leaving everything after it untouched.

## Configure

```bash
cmake --preset microide-perf
cmake --build build/microide-perf-make -j8
```

`microide-perf` enables:

- `CMAKE_BUILD_TYPE=RelWithDebInfo`
- `MICROIDE_WARNINGS_AS_ERRORS=ON`
- `MICROIDE_PERF_HARNESS_BUILD=ON`

## Scenario Authoring

Scenarios are registered in `tests/perf/PerfMain.cpp` and use `ScenarioContext` helpers from
`tests/perf/PerfHarness.{h,cpp}`.

When adding a scenario:

1. register a unique scenario name
2. keep setup deterministic (fixtures + explicit waits)
3. drive behavior through context helpers (`Open`, `OpenTab`, `Type`, `Scroll`, `KeyDown`, `Wait`)
4. pump frames intentionally (`PumpFrames`) so rendering work is included consistently
5. decide whether the scenario belongs in smoke (`.smoke = true`) or gate-only (`.smoke = false`)

## Hotspot Audit Matrix

Use this matrix for repository-wide hotspot passes so each critical workflow has deterministic
coverage and a clear owner.

| Workflow class | Primary scenarios | Key metrics | Primary subsystem ownership |
| --- | --- | --- | --- |
| Startup (no project, small, large) | `cold_startup_no_project`, `cold_startup_small_project`, `cold_startup_large_project` | p50/p95/max wall time, allocation counts | app bootstrap, session restore, workspace init |
| Editing and render throughput | `typing_small_file`, `typing_large_file`, `scroll_large_file`, `large_file_open_first_paint`, `multi_tab_cycle` | p50/p95/max wall time, allocation counts | editor, text viewport, render view-model pipeline |
| Large-file workout (opt-in) | `editor_moby_dick_workout` | per-phase p50/p95/max wall time, allocation counts | editor, text viewport, clipboard, undo history, resize/relayout |
| Search and indexing | `project_search_literal`, `project_search_regex`, `search_first_result`, `file_finder_cold` | p50/p95/max wall time | project search, file finder, background executor |
| Shell surfaces | `compare_tab_open`, `merge_tab_open`, `compare_scroll_large_fixture`, `merge_scroll_large_fixture`, `merge_scroll_interleaved_hunks`, `compare_scroll_selection`, `git_sidebar_activate` | p50/p95/max wall time, allocation counts | compare/merge services, sidebar services |
| Git workstation | `git_sidebar_refresh_large_repo`, `git_sidebar_refresh_many_untracked`, `diff_open_1000_file_changes`, `diff_next_hunk_large_file`, `diff_stage_hunk_large_patch`, `diff_stage_selected_lines`, `merge_open_many_conflicts`, `merge_next_conflict_large_file`, `merge_accept_hunk_interleaved`, `merge_edit_result_then_scroll`, `commit_open_with_large_staged_set`, `external_change_refresh_open_diff`, `external_change_refresh_open_merge` | p50/p95/max wall time, allocation counts, per-iteration `perf_counters` | `GitRepositoryService`, compare/merge services, staging, commit workflow, file watchers |
| Repo-open memory | `repo_open_rss_idle` | open-to-idle wall time, allocation counts, enforced steady-state RSS budget | workspace init, project catalog, tree/index startup |
| Terminal and output | `terminal_scroll_long_output` | p50/p95/max wall time, allocation counts | terminal panel, scroll and redraw integration |
| Idle and long soak | `idle_soak_30s`, `long_soak_8h`, `switch_and_idle` | wake-up count, wall time, allocation counts | event loop, scheduled wake handling, watchers |
| Debugger / DAP | `debug_value_tree_expand_large`, `debug_value_tree_rebuild`, `debug_value_tree_paging`, `dap_protocol_encode_decode`, `debug_breakpoints_model_rebuild`, `debug_pane_hittest_geometry`, `debug_session_stop_to_variables` | p50/p95/max wall time, allocation counts | `DebugValueTree`, `DapProtocol`, `DebugBreakpointsModel`, debug pane geometry, `DebugService`/`DebugSession` |
| LSP / language server | `lsp_semantic_tokens_decode`, `lsp_publish_diagnostics_parse`, `lsp_document_symbols_parse`, `lsp_message_framing` | p50/p95/max wall time, allocation counts | `lsp_protocol` decode helpers, `JsonRpcMessageFramer` transport framing |
| Syntax highlighting | `syntax_highlight_cpp_lines`, `syntax_highlight_python_lines`, `syntax_advance_state_cpp_lines` | p50/p95/max wall time, allocation counts | `runtime_syntax::HighlightLine` / `AdvanceState` -- the per-line token path every cold scroll and file open pays synchronously |
| Tech-debt hot-path coverage | `assist_ranked_union_merge`, `plugin_status_item_update`, `settings_rows_rebuild`, `reference_snippet_file_window`, `multi_caret_remap_burst`, `snippet_many_mirror_edit`, `user_config_record_decode`, `branch_review_presentation_markers` | p50/p95/max wall time, allocation counts (tight, decoupled from wall) | the TD-2026-07-17A rewritten hot paths — `assist_merge::RankedUnion`, `registry_interop::ApplyStatusItemUpdate`, `SettingsOverlayService::RebuildSettingsRows`, `util::ReadFileLineWindow`, `detail::ResolveMultiCaretRemapSites`, snippet mirror shifts, user-config decode, `ApplyBranchReviewPresentationMarkers` |
| Editor preference application | `settings_change_many_tabs` | per-phase p50/p95/max wall time, allocation counts (**1% p50 allocation tolerance** — see below) | `ApplyEditorPreferencesToAllTabs`: the walk every settings change, project activation and session restore makes over every open tab in every editor group. Split into two measured phases so the cheap per-viewport setters and the filetype-detect + language-contract family can never hide inside one number. |
| Plugin contribution-cap budgets | `plugin_status_items_resolve_at_cap`, `plugin_keybindings_resolve_at_cap` | p50/p95/max wall time, allocation counts | the resolve seams whose measured cost derives the caps in `plugin/PluginContributionLimits.h` (TD-2026-07-17-019): `ResolveStatusItems` at `kMaxPluginStatusItems`, `ResolveKeybindings` at `kMaxPluginContributionsPerKind`. Re-measure these before raising either cap. |

When a hotspot class has no deterministic coverage, add a scenario + baseline in the same change
before closing the performance pass.

### Editor preference application

`settings_change_many_tabs` (in `tests/perf/EditorEssentialsPerfScenarios.cpp`) opens 40 `.cpp`
tabs over a dedicated fixture — `tests/perf/fixtures/settings_tabs_project/`, which carries its own
`.editorconfig`; a shared fixture could not be used because dropping one in would have moved
`multi_tab_cycle`, `cold_startup_large_project` and `multi_project_switch` off their baselines.

Two things about it are deliberate and easy to break:

- **The allocation tolerance is 1%, not the default 10%.** The regression this scenario exists to
  catch is a few hundred allocations against a ~64k baseline. At the default tolerance the gate
  would sit an order of magnitude above the signal and pass a real regression — which was verified
  by reverting the fix and watching the gate stay green until the tolerance was tightened.
- **Both phases are fixed-cost now, not per-tab.** As of TD-2026-08-03-110 neither the cheap family
  nor the contract family allocates per open tab, so running the scenario at `kTabCount` 10 instead
  of 40 leaves the cheap family byte-identical and the contract family within 2 allocations. Read a
  few percent here as noise in the fixed per-settings-change overhead; read a large jump as per-tab
  work having come back — and re-running at a different `kTabCount` is the cheap way to tell which.

### Debugger / DAP scenarios

Live in `tests/perf/DebugPerfScenarios.cpp`. Two flavors, split by determinism:

- **Six pure-unit micro-benchmarks** (`debug_value_tree_expand_large`, `debug_value_tree_rebuild`,
  `debug_value_tree_paging`, `dap_protocol_encode_decode`, `debug_breakpoints_model_rebuild`,
  `debug_pane_hittest_geometry`) construct the real data structures directly and measure the hot
  paths the step/render loop consumes — `debug_value_tree_rebuild` is literally the render-ready
  flat row list the bottom-panel render TU draws, and it is allocation-stable (zero in-phase
  allocations on steady state). These are **gated** (`smoke = true, baseline_gated = true`) with
  committed reference-runner baselines under `tests/perf/baselines/`.

  Three of them — `debug_value_tree_paging`, `debug_breakpoints_model_rebuild`, and
  `debug_pane_hittest_geometry` — carry **decoupled tolerances** (wall 75/250/400 %, allocations
  10/20/50 %), the same split the tech-debt coverage scenarios use. They measure single-digit
  milliseconds of deterministic computation, and this runner cannot hold a 10 % wall envelope over
  work that small: repeat runs of one unchanged binary land in two stable modes ~40 % apart. Their
  allocation counts, by contrast, are byte-identical run to run — that is the real complexity gate,
  and it stays tight. `debug_pane_hittest_geometry` additionally repeats its hit-test sweep 100×
  (same inputs, more of them) because one sweep resolved in ~0.1 ms, below anything this runner can
  time. Do not copy these tolerances onto scenarios that hold their envelope.
- **One live mock-adapter session scenario** (`debug_session_stop_to_variables`) drives a real
  `DapManager` + `DebugSession` against an embedded Python DAP adapter and measures the
  stop → stackTrace → scopes → variables latency; it is subprocess-backed (noisier), skips
  gracefully when `python3` is unavailable, and stays **advisory** (`smoke = false,
  baseline_gated = false`).

### LSP / language-server scenarios

Live in `tests/perf/LspPerfScenarios.cpp`. All four are pure-unit micro-benchmarks over the LSP
wire path — the decode helpers in the `lsp_protocol` namespace and the `LspMessageFramer` framing
codec — so they are deterministic and **gated** (`smoke = true, baseline_gated = true`) with
committed baselines. They cover: `lsp_semantic_tokens_decode` (resolve a large delta-encoded
`semanticTokens/full` run into absolute tokens, re-run on every edit), `lsp_publish_diagnostics_parse`
(re-materialize a full `publishDiagnostics` array on every publish), `lsp_document_symbols_parse`
(walk a recursive `DocumentSymbol[]` outline on every save), and `lsp_message_framing` (drain a
chatty `Content-Length`-delimited stream fed in partial chunks — the transport hot path and resync
surface). The completion-item decode path is not yet covered here because it is an inline lambda in
`WorkspaceLspClientRequests.cpp` rather than a shared `lsp_protocol` helper; extract it before adding
a scenario.

### Syntax-highlight scenarios

Live in `tests/perf/SyntaxHighlightPerfScenarios.cpp`. Three pure-unit
micro-benchmarks over `runtime_syntax::HighlightLine` / `AdvanceState`, reading a
committed 50k fixture once and threading `SyntaxState` line to line exactly as
`TextViewport`'s per-line cache does on a cold scroll. Gated, with the tech-debt
coverage set's decoupled tolerances (loose wall, tight allocations).

They exist because the highlighter is the top main-thread scope of any scroll
through fresh content -- every newly exposed line is a token-cache miss the
render path resolves synchronously, since the off-thread prefetch cannot land
inside the frame that scrolled -- and it had no scenario of its own. The
interactive scenarios that reach it also pay file open, layout, render and
present, so even a 2x change in the highlighter moved them a few percent, well
inside their wall envelopes.

`syntax_advance_state_cpp_lines` covers the `want_tokens = false` replay the
checkpoint chain runs. It must stay far cheaper than the token path; a
regression that made it re-run pattern rules would be invisible in the other two.

### Tech-debt hot-path coverage scenarios

Live in `tests/perf/TechDebtCoveragePerfScenarios.cpp`. The TD-2026-07-17A burndown rewrote a set
of correctness-preserving-but-perf-sensitive hot paths (mostly O(n²) → indexed/hashed lookups, plus
the coordinate/cross-boundary rewrites), but several of those functions had no scenario exercising
them at scale, so `tools/perf-compare.py` could not have caught an accidental return to quadratic
behavior. These nine pure-unit micro-benchmarks each drive one rewritten hot path at a scale where
its complexity dominates, and are **gated** (`smoke = true, baseline_gated = true`) with committed
reference-runner baselines.

They lean on **decoupled wall vs allocation tolerances** (see below): the allocation counts are
exactly deterministic run-to-run (the real complexity oracle, gated tight at 10/20/50%), while the
wall envelopes are widened (75/250/400%) to absorb the software-render scheduler jitter this
shared runner shows on sub-50 ms work. A return to O(n²) still blows the allocation gate by
hundreds-plus percent; a constant-factor wall regression is caught precisely by the interleaved
`perf-compare.py` current-vs-main run, where machine load cancels.

### Wall vs allocation tolerances

Each baseline carries two independent tolerance sets: `p50/p95/max_percent` for the wall metrics and
`alloc_p50/p95/max_percent` for the allocation metrics (`tests/perf/baselines/*.json`). A scenario
sets them via `tolerance_*_percent` and `tolerance_alloc_*_percent` on its `Scenario` (a negative
allocation tolerance means "inherit the matching wall tolerance", the default). This lets a
jitter-prone micro-benchmark keep a tight, deterministic allocation gate while widening only its wall
envelope, instead of trading one against the other. Baselines written before the split omit the
allocation keys; `LoadBaseline` defaults them to the wall values, so their behavior is unchanged. A
full (non-smoke) run prints a per-scenario `[perf] PASS/FAIL` line and, on failure, each blown metric
with baseline vs measured, the delta percent, and the tolerance it exceeded — so a tripped gate names
the offending metric instead of surfacing only an exit code.

Promotion path ("advisory first, promote later"): once a deterministic scenario's numbers are
stable, set `baseline_gated = true` (and `smoke = true` to gate CI) and capture its baseline on
the reference runner with `--update-baseline`. Pure-unit scenarios are the promotion candidates;
keep live-session scenarios advisory (their subprocess timing is inherently noisy).

## Known Coverage Gaps

The current harness is useful, but it is not complete. These gaps are still open and should be
described honestly in README / roadmap text until they are closed:

- the gated suite now covers large-file open-to-first-paint, and `editor_moby_dick_workout`
  additionally traces cursor-jump-to-end/middle, window resize, whole-document
  select-all/cut/paste/undo/redo, and a mid-document typing burst on a real ~1.2 MB / ~22k-line
  prose file. That scenario is **opt-in**: its fixture is a network fetch
  (`generate_editor_essentials_perf_fixtures.py --fixture moby`, kept out of `--fixture all`), so
  it is `run_by_default = false` (explicit `--scenarios=editor_moby_dick_workout`) yet
  `baseline_gated = true` — it enforces its committed baseline when run on the reference runner.
  Other large-file interaction traces remain worth adding if those regressions recur.
- the large-surface interaction gates now cover compare and merge scroll bursts, interleaved merge
  hunks, compare scrolling with a multi-row selection, and the Git workstation scenario set below
  (sidebar refresh, diff open/navigation/staging, merge open/navigation/accept/edit-scroll, commit
  open, external refresh). Stage/discard and every compare/merge interaction pattern are still not
  fully covered.
- the TD-2026-07-17A hot-path coverage set (above) closed the biggest algorithmic gaps, but a few
  of that burndown's rewritten paths are still **not** perf-gated, by deliberate triage — each is
  either threaded (the process-global allocation counter is non-deterministic there) or needs a
  heavy integration harness, and each already has correctness-test coverage. Left open, with the
  reason:
  - **TD-2026-07-17A-005** (`TaskExecutor` keyed coalescing / blame) and **-108** (plugin syntax
    reload on the worker) run off-thread; a deterministic single-thread perf gate is not meaningful
    for them. **-033** (LSP `didOpen` post-present hydration deferral) likewise depends on the live
    shell frame loop.
  - **TD-2026-07-17A-066** (`WorkspaceShell::ApplyLspWorkspaceEdit` bucket-index map) needs a live
    shell with many open buffers and writes edits to disk; its quadratic is bounded by the
    edit-count cap and covered by the `ApplyLspWorkspaceEdit` unit tests.
  - **TD-2026-07-17A-076** (plugin settings-snapshot revisioned cache) only pays off under repeated
    snapshot capture with unchanged settings via a real Lua plugin; the cache-hit wall delta is a
    weak signal and its correctness is pinned by `SettingsRegistry/SnapshotCacheInvalidation`.

Do not paper over these gaps with broad wording like "memory is benchmarked" or "diff/merge is
fully covered." Say exactly which scenarios exist.

Advisory-only scenarios are explicit-only: they do not run in `--smoke`, they do not participate in
baseline comparison, and `--update-baseline` refuses them by design. The current default scenario
set has baselines for all registered non-smoke scenarios.

## Isolated Run Contract

`microide_perf` runs every scenario inside an isolated app-root so local state on
the developer's machine cannot contaminate measurements. Before SDL initialization
the harness:

1. creates a fresh directory under the system temp dir (e.g.
   `/tmp/microide-perf-<pid>-<rand>`) with empty `config/`, `state/`, `cache/`,
   and `data/` subdirectories
2. sets `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`, and `XDG_DATA_HOME`
   to those subdirectories so `platform::ResolveAppDirectory(...)` cannot see real
   user state (`~/.local/state/microide/workspace-session`, user config, plugin
   caches, etc.)
3. tears the sandbox down at shutdown unless `--keep-artifacts` is passed

This means `cold_startup_no_project` always starts from an empty workspace
session even when the developer has real projects restored on their machine; the
regression test `PerfHarnessIsolation/ColdStartupIgnoresRealUserSession`
exercises this contract end-to-end without requiring SDL.

### Artifact Retention For Triage

When a scenario fails and you need to inspect what the harness wrote into its
sandbox, run with `--keep-artifacts`:

```bash
./build/microide-perf-make/microide/microide_perf \
    --scenarios=<failing-scenario> \
    --iterations=1 \
    --keep-artifacts
```

The harness prints the retained path on stderr (`[perf] keeping isolated app-root
at /tmp/microide-perf-<pid>`). Inspect, then remove it manually when finished.

### Report Provenance Metadata

Each `--report-json` and `--report-text` emission now carries a metadata block at
the top so reviewers can distinguish reference-gate evidence from local advisory
runs without re-reading the command line:

- `runner_class`: `perf-runner-v1` when `--reference-runner=perf-runner-v1` is
  passed, otherwise `local-advisory`
- `provenance`: `reference` for the gate runner, `advisory` for any local or
  alternative-runner run
- `sdl_video_driver`, `sdl_renderer_driver`: the lanes actually measured, read
  back from SDL after init (`dummy` / `software` for a reference run). A value
  other than `dummy` means the run paid window-system present cost and its wall
  numbers are advisory
- `scenarios`, `iterations`, `layout_mode`, `seed`: exact workload definition
- `isolated_app_root`: a stable string so the report records whether artifacts
  were retained for triage

Baseline updates SHALL only be taken from reports whose `provenance` is
`reference`; local-advisory reports are useful for triage and ranking but never
authoritative for `tests/perf/baselines/*.json` movement.

## Deterministic Input Checklist

Before trusting results from a scenario run:

1. use fixed fixtures under `tests/perf/fixtures/` (avoid host-dependent project trees); large
   synthetic trees are gitignored but reproduced deterministically from committed generators +
   `.sha256` manifests (see "Generated editor-essentials fixtures" below)
2. keep random behavior deterministic via `MICROIDE_PERF_SEED` (default is fixed to `1337`)
3. drive frame work through explicit `PumpFrames(...)` calls
4. keep iteration count explicit (`--iterations=N`, default `10`)
5. run under software renderer (`SDL_HINT_RENDER_DRIVER=software`, the `--renderer` default) and fixed
   window (`1920x1080`); pass `--renderer=auto` for the advisory GPU lane (never baseline-gated)
6. keep plugin-dependent scenarios explicit and bounded; do not rely on incidental plugin state
7. capture JSON reports (`--report-json`) for reproducible hotspot triage diffs
8. drain async subsystems to a fixed state before/inside the measured window rather than snapshotting
   a race. A scenario over a project's file index or project search must wait for the initial index
   build (`WaitForFileIndexPath`) and for the search worker to actually finish (`WaitForProjectSearchFinished`,
   backed by the non-consuming `ProjectSearchService::WorkerFinished`) and then drain exactly once —
   snapshotting mid-flight makes the metric swing wildly (`search_first_result` swung ~80× before this)
9. use `Scenario::warmup_iterations` for scenarios whose first passes do one-time cold work the rest
   reuse (initial index build, background-subsystem settling). Discarded warmup passes bring the reused
   driver to steady state so every measured iteration is uniform; the whole run shares one process, and
   the allocation counter is process-**global** (counts every thread), so background work counts too.
   **A baseline whose p95 is several times its own p50 is the symptom.** Six gates carried that shape
   until 2026-08-03: any scenario that opens a project and then measures a small amount of work pays
   the cold open (file-index build, initial watch batch, session write) in iteration 0 and nowhere
   else -- `typing_large_file` ran 5691, 392, 391, 390, ... -- so p95/max tracked which iteration index
   the cold pass landed on rather than the tail of the measured work, and flapped between runs of an
   unchanged binary. Check the per-iteration allocation counts in a `--report-json` before trusting a
   wide percentile spread; the fix is a warmup pass, never a wider envelope
10. the harness pins project search to a single worker (`MICROIDE_SEARCH_WORKER_LIMIT=1`, set in
    `PerfMain`): with a global allocation counter, N parallel search workers make measured allocations
    non-deterministic. A scenario whose steady-state median is deterministic but whose flat baseline
    leaves the tail no headroom may loosen `Scenario::tolerance_{p95,max}_percent` (keeping p50 tight)
    so an incidental background wake can't false-positive

Current notable scenarios:

- `switch_and_idle` (smoke): open fixture project A, open 20 tabs, switch to fixture project B,
  open 15 tabs, switch A→B, then idle for 30 frames
  - fixture roots:
    - `tests/perf/fixtures/switch_project_a`
    - `tests/perf/fixtures/switch_project_b`
  - baseline:
    - `tests/perf/baselines/switch_and_idle.json`

- `file_finder_cold` (gate): builds the in-process file index from the 10 000-file flat fixture,
  simulates file-finder open, measures time to first rendered result; asserts ≤ 50 ms
  - fixture root:
    - `tests/perf/fixtures/file_finder_large/`
  - baseline:
    - `tests/perf/baselines/file_finder_cold.json`
  - skips gracefully when fixture directory is absent

- `git_sidebar_activate` (gate): opens the pre-seeded 1 000-file git fixture project, activates
  the git sidebar, measures time from activation to first rendered git-status frame; asserts ≤ 200 ms
  - fixture root:
    - `tests/perf/fixtures/git_status_project/`
  - baseline:
    - `tests/perf/baselines/git_sidebar_activate.json`
  - skips gracefully when fixture directory is absent

### Generated editor-essentials fixtures

The large synthetic editor fixtures are deterministic and **generated on demand** rather than
checked into git (they are ~16 MB of regenerable text). Their data trees are listed in
`tests/perf/fixtures/.gitignore`; the committed `tests/perf/fixtures/editor_essentials_*.sha256`
manifests are the authoritative contract.

| Fixture | Generator output |
| --- | --- |
| `tests/perf/fixtures/editor_essentials_50k_cpp/` | 50k-line synthetic C++ buffer |
| `tests/perf/fixtures/editor_essentials_50k_py/` | 50k-line synthetic Python buffer |
| `tests/perf/fixtures/editor_essentials_1mb/` | exactly 1 MiB mixed-content text |

CTest's `microide_perf_fixtures` setup test runs the generator with `--ensure` before
`microide_perf_tests` (wired via `FIXTURES_SETUP`/`FIXTURES_REQUIRED`), so a fresh checkout
reproduces them automatically. `--ensure` only regenerates trees that are missing or do not match
the committed `.sha256`, and it never rewrites the manifest — if regeneration fails to reproduce the
committed hash it aborts (catching Python/platform drift). Regenerate manually (and refresh the
committed `.sha256` after intentionally changing a generator) with:

```bash
python3 tests/perf/generate_editor_essentials_perf_fixtures.py --fixture all   # rewrites .sha256
python3 tests/perf/generate_editor_essentials_perf_fixtures.py --ensure --fixture all  # restore only
```

### Git workstation fixtures and scenarios

Generate deterministic Git workstation fixtures (not checked into git; listed in
`tests/perf/fixtures/.gitignore`):

```bash
bash tests/perf/generate_git_workstation_fixtures.sh
```

Fixture roots:

| Fixture | Purpose |
| --- | --- |
| `tests/perf/fixtures/git_1000_changed_project/` | 1 000 modified tracked files (sidebar refresh + diff open) |
| `tests/perf/fixtures/git_many_untracked_project/` | 1 000 tracked + 1 500 untracked |
| `tests/perf/fixtures/git_large_diff_project/` | ~12k-line `src/large.cpp` working-tree diff |
| `tests/perf/fixtures/git_large_staged_project/` | 800 staged modifications |
| `tests/perf/fixtures/git_many_conflicts_project/` | 420-block three-way merge inputs (`base.cpp`, `current.cpp`, `incoming.cpp`) |
| `tests/perf/fixtures/git_large_status_project/` | 5 000 tracked files (generator only; refresh scenarios use `git_1000_changed_project` to avoid multi-minute project open) |

Scenarios live in `tests/perf/GitWorkstationPerfScenarios.cpp`. Git-only paths use
`WorkspaceShellTestAccess::PerfPrimeGitRepository` plus
`PerfRunGitSidebarRefreshSync` (a synchronous testing seam on `GitRepositoryService`) so refresh
measurements settle without relying on background SDL wake pumping in `microide_perf`. Compare
scenarios open working-tree tabs through `OpenWorkingTreeComparison` rather than shell commands.

Gate scenarios (each has `tests/perf/baselines/<scenario>.json`):

- `git_sidebar_refresh_large_repo`: prime `git_1000_changed_project`, sync sidebar refresh, assert ≥ 500 entries
- `git_sidebar_refresh_many_untracked`: prime `git_many_untracked_project`, sync refresh, assert ≥ 1 000 tracked entries
- `diff_open_1000_file_changes`: refresh sidebar, open first changed-file compare tab
- `diff_next_hunk_large_file`: open `git_large_diff_project` compare, jump next hunk repeatedly
- `diff_stage_hunk_large_patch`: stage current compare hunk on large patch
- `diff_stage_selected_lines`: stage a multi-line right-pane selection
- `merge_open_many_conflicts`: open three-way merge from `git_many_conflicts_project`, assert hunk model built
- `merge_next_conflict_large_file`: temp interleaved merge fixture, next-conflict navigation burst
- `merge_accept_hunk_interleaved`: accept-current on interleaved hunks
- `merge_edit_result_then_scroll`: type in merge result pane then scroll
- `commit_open_with_large_staged_set`: prime `git_large_staged_project`, open commit workflow
- `external_change_refresh_open_diff`: open compare, simulate external file change, refresh
- `external_change_refresh_open_merge`: open merge, simulate external change, refresh

Capture or refresh all Git workstation baselines on the reference runner (required before merge
when baselines move):

```bash
./build/microide-perf-make/microide/microide_perf \
    --reference-runner=perf-runner-v1 \
    --scenarios=git_sidebar_refresh_large_repo,git_sidebar_refresh_many_untracked,diff_open_1000_file_changes,diff_next_hunk_large_file,diff_stage_hunk_large_patch,diff_stage_selected_lines,merge_open_many_conflicts,merge_next_conflict_large_file,merge_accept_hunk_interleaved,merge_edit_result_then_scroll,commit_open_with_large_staged_set,external_change_refresh_open_diff,external_change_refresh_open_merge \
    --iterations=10 \
    --update-baseline
```

Runs without `--reference-runner=perf-runner-v1` are advisory (`provenance=advisory`); they are
useful for smoke and triage but SHALL NOT replace reference-runner evidence when updating committed
baselines.

- `repo_open_rss_idle` (gate): opens the large-project fixture, pumps the first frames, waits
  500 ms at idle, and gates memory while also tracking wall time and allocations through the normal
  baseline machinery. Two checks, because RSS is a **process** number and every scenario in a run
  shares one process:
  - **always**: the RSS growth this scenario's own open adds (entry → idle) must stay under
    160 MiB. This is the order-independent gate — it measures only what the scenario controls.
  - **only in a fresh process** (RSS at scenario entry ≤ 128 MiB): steady-state RSS must stay
    under the 256 MiB absolute budget.

  The absolute check used to run unconditionally, which in a full-suite run made it assert that
  the summed peak footprint of every earlier scenario was under budget: it read 279 MiB there
  versus 153 MiB for the same scenario run on its own, and which side of the line it landed on
  depended on scenario order and iteration count. Do not restore an unconditional absolute check.
  - fixture root:
    - `tests/perf/fixtures/large_project/`
  - baseline:
    - `tests/perf/baselines/repo_open_rss_idle.json`

- `large_file_open_first_paint` (gate): opens the 1 MiB editor fixture and measures the first
  frames after file open to catch first-paint regressions that typing and scroll bursts do not
  isolate
  - fixture root:
    - `tests/perf/fixtures/editor_essentials_1mb/`
  - baseline:
    - `tests/perf/baselines/large_file_open_first_paint.json`

- `compare_scroll_large_fixture` (gate): creates a temporary git repo around the 1 MiB mixed-content
  fixture, opens a working-tree-vs-HEAD compare tab, then drives an 80-step scroll burst to catch
  sustained large-surface regressions
  - fixture root:
    - `tests/perf/fixtures/editor_essentials_1mb/`
  - baseline:
    - `tests/perf/baselines/compare_scroll_large_fixture.json`

- `merge_scroll_large_fixture` (gate): builds a temporary large merge fixture from the 1 MiB
  mixed-content seed, opens the merge tab, then drives an 80-step scroll burst to catch sustained
  merge-surface regressions
  - fixture root:
    - `tests/perf/fixtures/editor_essentials_1mb/`
  - baseline:
    - `tests/perf/baselines/merge_scroll_large_fixture.json`

- `merge_scroll_interleaved_hunks` (gate): builds synthetic base / current / incoming files with
  hundreds of interleaved merge hunks, opens the merge tab, then drives a sustained scroll burst to
  catch hunk-density regressions that the tail-only large fixture cannot surface
  - baseline:
    - `tests/perf/baselines/merge_scroll_interleaved_hunks.json`

- `compare_scroll_selection` (gate): creates a temporary git repo with many interleaved compare
  hunks, opens a working-tree-vs-HEAD compare tab, holds a multi-row right-pane selection, then
  drives a sustained scroll burst to exercise the selection-aware compare render path
  - baseline:
    - `tests/perf/baselines/compare_scroll_selection.json`

- `search_first_result` (gate): initiates a search on the 10 000-file fixture with a pattern that
  matches one file near the end of the corpus, measures time to first result batch; asserts ≤ 100 ms
  - fixture root:
    - `tests/perf/fixtures/file_finder_large/`
  - baseline:
    - `tests/perf/baselines/search_first_result.json`
  - skips gracefully when fixture directory is absent

- `window_resize_stress` (smoke): repeatedly resizes the window between compact and regular
  dimensions while pumping frames, used to catch layout and resize-path regressions
  - fixture root:
    - `tests/perf/fixtures/small_project/`
  - baseline:
    - `tests/perf/baselines/window_resize_stress.json`

- `idle_soak_30s` (gate): 3-second settle then 27-second soak; asserts that the file-index watcher
  thread and git executor thread generate zero SDL wake events during the soak period after startup
  work completes; verifies the event loop reaches `SDL_WaitEvent` at rest
  - no fixture required

## Run Under Virtual Display

```bash
./build/microide-perf-make/microide/microide_perf --smoke
```

The harness selects its own video driver (`dummy`) and renderer (`software`) before `SDL_Init`, so
no display, `xvfb-run`, or `SDL_VIDEODRIVER` export is needed — and none should be used, because a
real window system charges present cost the baselines do not contain (see "Reference Runner Class").

**How wrong this goes if you wrap it anyway:** running the gate under `xvfb` + x11
inflated frame-pumping scenarios by **2-12x**, and hybrid-CPU core placement added
another **2.4x** on top. Both read exactly like a broad code regression across
unrelated subsystems — which is the tell: a real regression is localised, a lane
error moves everything that pumps frames. When a wall number looks impossible,
**check the lane before the code**.

`--video=x11|wayland|auto` is the explicit windowed lane: provenance is never
`reference`, `--update-baseline` refuses it, and its wall metrics print as
`[advisory: windowed video lane, not comparable]` while allocation gates still
enforce. `tools/perf-compare.py` does not wrap in xvfb either.
`PerfBaseline/RecipesDoNotPinAVideoDriver` fails if a doc or tool reintroduces the
recipe — including this one.

## Baseline Workflow

Per-scenario baselines are stored under `tests/perf/baselines/<scenario>.json`.

Update one or more baselines:

```bash
./build/microide-perf-make/microide/microide_perf \
    --scenarios=<comma-separated-scenarios> \
    --iterations=10 \
    --update-baseline
```

Check against existing baselines:

```bash
./build/microide-perf-make/microide/microide_perf --iterations=10
```

## Baseline Change Rule

If a change modifies any `tests/perf/baselines/*.json`, the change record must include a line that
starts with:

```text
perf-baseline: <reason>
```

Accepted locations:

- PR description
- Commit message

The `perf-baseline-tag` CI job enforces this.

## Reference Runner Class

The gate run is measured on a dedicated self-hosted runner class tagged `perf-runner-v1`. Baselines
must be updated from this class (or a machine with equivalent characteristics) before tightening or
replacing gate numbers.

The maintainer's development workstation is the designated `perf-runner-v1` host. Baselines
regenerated there with `microide_perf --update-baseline --reference-runner=perf-runner-v1` (so
reports carry reference provenance) are authoritative, not local-advisory.

### The reference lane is a property of the binary, not of your shell

Run the gate **bare**:

```bash
./build/microide-perf-make/microide/microide_perf --iterations=10 \
    --reference-runner=perf-runner-v1
```

No `xvfb-run`, no exported `SDL_VIDEODRIVER`. The harness sets its own lane before `SDL_Init`:

| knob | reference lane | advisory override | why |
| --- | --- | --- | --- |
| renderer | `software` | `--renderer=auto\|<driver>` | GPU numbers are not cross-machine portable |
| video | `dummy` | `--video=auto\|<driver>` | a real window system charges present cost the baselines never recorded |
| CPU set | `auto` (fastest cluster) | `--pin-cores=off\|<cpu-list>` | on a hybrid CPU the scheduler's choice is worth up to 2.4x |

All three overrides mark the run advisory, refuse `--update-baseline`, and are recorded in the
report metadata (`sdl_video_driver` and `cpu_affinity` name what was actually measured, read back
from SDL and from the applied affinity mask rather than from what was requested).

`--pin-cores=auto` groups the online CPUs by `cpuinfo_max_freq` and pins the process to the fastest
group; on a homogeneous machine (or one with no cpufreq data) it does nothing. perf-runner-v1 is a
Ryzen AI 9 HX 370 — 8 threads at 5.16 GHz on cpu 0-3/12-15, 16 at 3.29 GHz on the rest — where
`debug_value_tree_paging` measured **3.18-4.31 ms** pinned to the fast cluster and **7.63-7.76 ms**
pinned to the dense one, with byte-identical allocation counts. Unpinned it wandered across both and
failed a baseline recorded from the same binary minutes earlier. Every wall gate was carrying that
2x as unbounded noise.

**Do not wrap the gate in `xvfb-run`.** It was documented that way, and it silently invalidates
every wall gate. Measured 2026-08-03 on perf-runner-v1, same commit, same binary, dummy lane vs
`xvfb`+`x11`:

| scenario | dummy | xvfb/x11 | ratio |
| --- | --- | --- | --- |
| `git_sidebar_activate` | 3.9 ms | 45.1 ms | 11.5x |
| `window_resize_stress` | 13.9 ms | 139.5 ms | 10.0x |
| `cold_startup_no_project` | 2.3 ms | 17.5 ms | 7.5x |
| `compare_tab_open` | 1.6 ms | 10.3 ms | 6.5x |
| `menu_hover_switch` | 2.2 ms | 6.0 ms | 2.7x |
| `dap_protocol_encode_decode` | 38.0 ms | 33.0 ms | 0.87x |
| `syntax_highlight_cpp_lines` | 12.6 ms | 15.7 ms | 1.25x |
| `settings_rows_rebuild` | 19.0 ms | 16.6 ms | 0.87x |

The cost lands **only** on scenarios that pump frames and **not at all** on pure-unit ones, and
allocation counts come out byte-identical in both lanes. That combination is exactly what a broad
real regression looks like, which is why it survived two sessions of investigation: a whole-suite
A/B against the baseline commit reproduces it (both sides are windowed), and the allocation oracle
agrees with the baseline (allocations do not depend on the lane). A windowed run now downgrades its
wall metrics to `[advisory: windowed video lane, not comparable]` and enforces only the allocation
gates, so it can no longer masquerade as a regression report.

One caveat on this host: `repo_open_rss_idle`'s absolute 256 MiB steady-state gate is inflated when
run under software GL (llvmpipe). It measures ~153 MiB for a fresh process here, but it only runs
at all when the process is fresh (see the scenario's two-check contract above), so a full-suite run
exercises the order-independent growth budget instead.

**Check the machine is idle before trusting a number, and before rebaselining.** Wall gates on a
shared workstation are noisier than the committed tolerances assume, and this box has heterogeneous
cores (8 Zen5 at 5.16 GHz, 16 Zen5c at 3.29 GHz) — the same scenario measured 1.37x apart depending
on which cluster the scheduler picked. So: a failed wall gate is not evidence of a regression until
you have run the same scenario on a baseline checkout **in the same lane**, interleaved. And
identical allocation counts across a large wall delta mean the algorithm did not change — the
machine or the lane did. Do not rebaseline in that state.

## Ad-hoc Branch-vs-Commit Comparison

For a one-shot "current working tree vs some other commit" comparison (including
uncommitted changes on the current side), use `tools/perf-compare.py`:

```bash
tools/perf-compare.py                  # current tree vs main HEAD
tools/perf-compare.py <commit-sha>     # current tree vs explicit SHA
ITERATIONS=5 tools/perf-compare.py     # fewer iterations per scenario
SCENARIOS=typing_small_file,scroll_large_file tools/perf-compare.py
```

The script builds `microide_perf` in-place for the current tree (so uncommitted
changes are included), spins up a detached `git worktree` at the comparison SHA,
mirrors gitignored fixtures into it, runs every registered scenario one at a
time on both sides, and prints a coloured ASCII table comparing p50/p95/max
wall-time and allocation metrics, plus regression and improvement summaries.

Each scenario runs in its own `microide_perf` invocation, so a scenario that
throws (e.g. an RSS-budget overrun) is skipped without affecting the others.
Set `NO_COLOR=1` to disable colour, `KEEP=1` to keep the temporary worktree and
JSON reports for triage, and `REGRESS_PCT=<n>` to adjust the regression
highlighting threshold (default 5%). These ad-hoc numbers are local-advisory
and must not be used to update `tests/perf/baselines/*.json`.

A small per-scenario iteration cap is hard-coded in the script for
`idle_soak_30s` and `long_soak_8h`: both are deterministic-sleep scenarios
whose value is a single binary wake-budget assertion, so running them more
than once adds no signal. They always run at 1 iteration regardless of
`ITERATIONS`, saving roughly 10 minutes on a default 10-iter run.

## Smoke vs Gate Split

- local/PR smoke: small subset for quick signal (`microide_perf_tests` / `--smoke`)
- perf gate: full suite on `perf-runner-v1` with baseline comparison
