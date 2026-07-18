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
- behavior under a GPU-accelerated renderer by default; the reference (gated) lane pins the software
  renderer. An **advisory GPU lane** exists (`--renderer=auto|<sdl-driver>`) for measuring GPU-only
  paths like the batched glyph atlas — its numbers are printed but never gated or written to baselines
  (cross-machine GPU timings are not portable), exactly like the DAP advisory scenarios
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
| Large-file workout (opt-in) | `editor_moby_dick_workout` | per-phase p50/p95/max wall time, allocation counts | editor, text viewport, clipboard, undo history, resize/relayout |
| Search and indexing | `project_search_literal`, `project_search_regex`, `search_first_result`, `file_finder_cold` | p50/p95/max wall time | project search, file finder, background executor |
| Shell surfaces | `compare_tab_open`, `merge_tab_open`, `compare_scroll_large_fixture`, `merge_scroll_large_fixture`, `merge_scroll_interleaved_hunks`, `compare_scroll_selection`, `git_sidebar_activate` | p50/p95/max wall time, allocation counts | compare/merge services, sidebar services |
| Git workstation | `git_sidebar_refresh_large_repo`, `git_sidebar_refresh_many_untracked`, `diff_open_1000_file_changes`, `diff_next_hunk_large_file`, `diff_stage_hunk_large_patch`, `diff_stage_selected_lines`, `merge_open_many_conflicts`, `merge_next_conflict_large_file`, `merge_accept_hunk_interleaved`, `merge_edit_result_then_scroll`, `commit_open_with_large_staged_set`, `external_change_refresh_open_diff`, `external_change_refresh_open_merge` | p50/p95/max wall time, allocation counts, per-iteration `perf_counters` | `GitRepositoryService`, compare/merge services, staging, commit workflow, file watchers |
| Repo-open memory | `repo_open_rss_idle` | open-to-idle wall time, allocation counts, enforced steady-state RSS budget | workspace init, project catalog, tree/index startup |
| Terminal and output | `terminal_scroll_long_output` | p50/p95/max wall time, allocation counts | terminal panel, scroll and redraw integration |
| Idle and long soak | `idle_soak_30s`, `long_soak_8h`, `switch_and_idle` | wake-up count, wall time, allocation counts | event loop, scheduled wake handling, watchers |
| Debugger / DAP | `debug_value_tree_expand_large`, `debug_value_tree_rebuild`, `debug_value_tree_paging`, `dap_protocol_encode_decode`, `debug_breakpoints_model_rebuild`, `debug_pane_hittest_geometry`, `debug_session_stop_to_variables` | p50/p95/max wall time, allocation counts | `DebugValueTree`, `DapProtocol`, `DebugBreakpointsModel`, debug pane geometry, `DebugService`/`DebugSession` |
| LSP / language server | `lsp_semantic_tokens_decode`, `lsp_publish_diagnostics_parse`, `lsp_document_symbols_parse`, `lsp_message_framing` | p50/p95/max wall time, allocation counts | `lsp_protocol` decode helpers, `LspMessageFramer` transport framing |
| Tech-debt hot-path coverage | `assist_ranked_union_merge`, `review_comments_registry_lookup`, `plugin_status_item_update`, `settings_rows_rebuild`, `reference_snippet_file_window`, `multi_caret_remap_burst`, `snippet_many_mirror_edit`, `user_config_record_decode`, `branch_review_presentation_markers` | p50/p95/max wall time, allocation counts (tight, decoupled from wall) | the TD-2026-07-17A rewritten hot paths — `assist_merge::RankedUnion`, `ReviewCommentsRegistry`, `registry_interop::ApplyStatusItemUpdate`, `SettingsOverlayService::RebuildSettingsRows`, `util::ReadFileLineWindow`, `detail::ResolveMultiCaretRemapSites`, snippet mirror shifts, user-config decode, `ApplyBranchReviewPresentationMarkers` |

When a hotspot class has no deterministic coverage, add a scenario + baseline in the same change
before closing the performance pass.

### Debugger / DAP scenarios

Live in `tests/perf/DebugPerfScenarios.cpp`. Two flavors, split by determinism:

- **Six pure-unit micro-benchmarks** (`debug_value_tree_expand_large`, `debug_value_tree_rebuild`,
  `debug_value_tree_paging`, `dap_protocol_encode_decode`, `debug_breakpoints_model_rebuild`,
  `debug_pane_hittest_geometry`) construct the real data structures directly and measure the hot
  paths the step/render loop consumes — `debug_value_tree_rebuild` is literally the render-ready
  flat row list the bottom-panel render TU draws, and it is allocation-stable (zero in-phase
  allocations on steady state). These are **gated** (`smoke = true, baseline_gated = true`) with
  committed reference-runner baselines under `tests/perf/baselines/`.
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
wall envelopes are widened (75/250/400%) to absorb the software-render/xvfb scheduler jitter this
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
   the allocation counter is process-**global** (counts every thread), so background work counts too
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
xvfb-run -a env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  ./build/microide-perf-make/microide/microide_perf \
    --reference-runner=perf-runner-v1 \
    --scenarios=git_sidebar_refresh_large_repo,git_sidebar_refresh_many_untracked,diff_open_1000_file_changes,diff_next_hunk_large_file,diff_stage_hunk_large_patch,diff_stage_selected_lines,merge_open_many_conflicts,merge_next_conflict_large_file,merge_accept_hunk_interleaved,merge_edit_result_then_scroll,commit_open_with_large_staged_set,external_change_refresh_open_diff,external_change_refresh_open_merge \
    --iterations=10 \
    --update-baseline
```

Local dummy-driver runs are advisory (`provenance=advisory`); they are useful for smoke and
triage but SHALL NOT replace `perf-runner-v1` evidence when updating committed baselines.

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

The maintainer's development workstation is the designated `perf-runner-v1` host. Baselines
regenerated there with `microide_perf --update-baseline` (under `xvfb-run -a env
SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy`, passing `--reference-runner=perf-runner-v1` so reports
carry reference provenance) are authoritative, not local-advisory. One caveat: the
`repo_open_rss_idle` hard 256 MiB steady-state RSS gate cannot pass on this host under xvfb
software-GL (llvmpipe inflates idle RSS to ~300 MiB), so that scenario is excluded from workstation
rebaselines and its committed baseline is left unchanged.

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
