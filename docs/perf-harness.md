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
- behavior under a GPU-accelerated renderer; the reference harness pins the software renderer
- behavior on other hardware than `perf-runner-v1`; cross-machine numbers are advisory
- whether LTO "proves" cross-translation-unit extractions are free. LTO can recover some inlining
  loss, but residual sticky-scroll/render-path regressions still need direct profiling and explicit
  fix-or-accept decisions

Treat the perf harness as a regression alarm, not a marketing instrument. If you write commit
messages, PR descriptions, or release notes, use phrasing like "no regression on
`<scenario>`" or "improves `<scenario>` p50 from X to Y on `perf-runner-v1`," not "fastest" or
"X% faster than $other_editor."

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
| Search and indexing | `project_search_literal`, `project_search_regex`, `search_first_result`, `file_finder_cold` | p50/p95/max wall time | project search, file finder, background executor |
| Shell surfaces | `compare_tab_open`, `merge_tab_open`, `compare_scroll_large_fixture`, `merge_scroll_large_fixture`, `merge_scroll_interleaved_hunks`, `compare_scroll_selection`, `git_sidebar_activate` | p50/p95/max wall time, allocation counts | compare/merge services, sidebar services |
| Repo-open memory | `repo_open_rss_idle` | open-to-idle wall time, allocation counts, enforced steady-state RSS budget | workspace init, project catalog, tree/index startup |
| Terminal and output | `terminal_scroll_long_output` | p50/p95/max wall time, allocation counts | terminal panel, scroll and redraw integration |
| Idle and long soak | `idle_soak_30s`, `long_soak_8h`, `switch_and_idle` | wake-up count, wall time, allocation counts | event loop, scheduled wake handling, watchers |

When a hotspot class has no deterministic coverage, add a scenario + baseline in the same change
before closing the performance pass.

## Known Coverage Gaps

The current harness is useful, but it is not complete. These gaps are still open and should be
described honestly in README / roadmap text until they are closed:

- the gated suite now covers large-file open-to-first-paint, but it still does not cover every
  large-file interaction. Cursor jumps and edit-after-open traces remain worth adding if those
  regressions recur.
- the large-surface interaction gates now cover compare and merge scroll bursts, interleaved merge
  hunks, and compare scrolling with a multi-row selection, but they still do not cover every
  compare/merge interaction pattern. Hunk-navigation and mixed edit/scroll traces on large fixtures
  remain worth adding if those regressions recur.

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
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
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
- `sdl_video_driver`, `sdl_renderer_driver`: resolved at report-time from
  `SDL_VIDEODRIVER` and the harness's hard-coded `software` renderer hint
- `scenarios`, `iterations`, `layout_mode`, `seed`: exact workload definition
- `isolated_app_root`: a stable string so the report records whether artifacts
  were retained for triage

Baseline updates SHALL only be taken from reports whose `provenance` is
`reference`; local-advisory reports are useful for triage and ranking but never
authoritative for `tests/perf/baselines/*.json` movement.

## Deterministic Input Checklist

Before trusting results from a scenario run:

1. use fixed fixtures committed under `tests/perf/fixtures/` (avoid host-dependent project trees)
2. keep random behavior deterministic via `MICROIDE_PERF_SEED` (default is fixed to `1337`)
3. drive frame work through explicit `PumpFrames(...)` calls
4. keep iteration count explicit (`--iterations=N`, default `10`)
5. run under software renderer (`SDL_HINT_RENDER_DRIVER=software`) and fixed window (`1920x1080`)
6. keep plugin-dependent scenarios explicit and bounded; do not rely on incidental plugin state
7. capture JSON reports (`--report-json`) for reproducible hotspot triage diffs

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

- `repo_open_rss_idle` (gate): opens the large-project fixture, pumps the first frames, waits
  500 ms at idle, and asserts steady-state RSS ≤ 64 MiB on Linux while also tracking wall time and
  allocations through the normal baseline machinery
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
xvfb-run -a ./build/microide-perf-make/microide/microide_perf --smoke
```

If SDL fails to initialize under Xvfb, force the expected environment variable names:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf --smoke
```

Use `SDL_VIDEODRIVER` (not `SDL_VIDEO_DRIVER`).

## Baseline Workflow

Per-scenario baselines are stored under `tests/perf/baselines/<scenario>.json`.

Update one or more baselines:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf \
    --scenarios=<comma-separated-scenarios> \
    --iterations=10 \
    --update-baseline
```

Check against existing baselines:

```bash
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
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
