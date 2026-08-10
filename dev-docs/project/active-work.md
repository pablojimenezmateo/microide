# MicroIDE Active Work

Reviewed 2026-08-03. Shipped baseline: **v2.8.0**.

This file answers one question: **what should be worked on next, and what is
deliberately not being built.**

It is not a changelog and not a shipped-feature inventory. Those exist:

| you want | read |
| --- | --- |
| what shipped, when | `CHANGELOG.md` |
| what the product *is* | `README.md` § Highlights |
| why a subsystem is shaped as it is | `dev-docs/project/implementation-guide.md` |
| durable behavioural contracts | `openspec/specs/` |
| per-change proposals and specs | `openspec/changes/archive/` |
| open, actionable debt | `dev-docs/project/known-tech-debt.md` |
| UI rules that were each broken once | `dev-docs/project/ui-invariants.md` |

## Priority Order

1. speed
2. correctness
3. low CPU usage
4. low memory usage
5. architectural clarity
6. compatibility only when explicitly required

Speed leads because latency is the product. Correctness still outranks CPU,
memory, and clarity, so it is never traded for those — but a pathological input
may be bounded with a declared cap or fallback to keep the common path fast.
`AGENTS.md` § Priority Order and `openspec/specs/product-vision/spec.md` carry
the authoritative wording.

Broad refactors are acceptable when they improve the result. Do not preserve
stale boundaries, legacy helpers, or accidental compatibility if they block
speed or correctness.

## Validation Focus

The native diff/merge/git workstation flow is what gets exercised end to end:

> open repo → inspect changes → diff files → resolve merge conflict → stage/commit

A change that degrades that path is a regression regardless of what else it
improves.

## Active Work

Everything below is *open*. A phase disappears from this file when it closes —
its record lives in `CHANGELOG.md` and `openspec/changes/archive/`.

### 1. Validate the shipped runtimes against real tools

The LSP, DAP, task, test, and SCM runtimes are all in the tree and all
covered by end-to-end *fake*-server tests. The remaining work is real-tool
validation, because that is where the contracts actually get tested.

- exercise at least one real integration per platform surface before claiming
  broader support; `gdb-dap` and clangd are the current real ones
- confirm callback delivery, wake routing, cancellation, shutdown, and project
  switching stay correct under repeated concurrent activity
- no runtime request may stall render or input handling
- add regression coverage for whatever proves fragile during validation

### 2. Plugin platform: keep the seams narrow

The registries, the worker-thread boundary, and the host-renders-data
presentation surface are shipped and validated zero-cost when unused. The
standing rules, not a task list:

- never expose `WorkspaceShell` wholesale; add a narrow registry or service
- editing, compare, merge, search, git, and terminal stay built-in product
  features even though plugins can extend around them
- keep completion and code-action overlays host-owned and minimal; do not fork
  the command prompt into a second editor interaction model
- add async/background plugin task surfaces only when a real plugin workload
  needs one
- `AssistService::Operations` is a transitional seam. It should shrink into
  smaller explicit assist ports over time, not grow into a shell callback bag

### 3. Terminal hardening

The emulator covers the full-screen and shell workflows exercised so far.

- broaden validation with actual terminal programs rather than extending escape
  coverage from guesswork
- fill remaining ANSI gaps only where real usage justifies them
- keep resize, redraw, scrollback, and wake behaviour robust under long-running
  output

### 4. Editor correctness and scale

- continue UTF-8 and IME hardening over the piece tree's byte-offset storage
  (32-bit offsets, ~4 GiB per-file ceiling)
- validate large-file thresholds on larger repositories; adjust from measured
  behaviour, not guesswork
- keep blame shadow text asynchronous, viewport-scoped, and cheap enough to
  preserve typing and scrolling latency
- expand compare/merge coverage — editor-side regressions there are still too
  easy to miss

### 5. Project and git service hardening

- keep external tool usage behind `src/project/*` service boundaries
- keep tightening subprocess error reporting and git command behaviour around
  the system `git` path
- move avoidable filesystem and git refresh work off latency-sensitive UI paths
- layer new plugin-facing capabilities on structured services rather than
  letting UI code or plugin glue parse command output directly

### 6. Testing and performance discipline

- **the perf baselines are fully rerecorded as of 2026-08-07** (TD-2026-08-07-161,
  now resolved). The deterministic half went first with
  `--update-baseline=deterministic`, which rewrites only the metrics that do not
  depend on machine state; the timing and resident half followed on an idle
  perf-runner-v1 with the full `--update-baseline`. The suite re-gated 100 PASS /
  0 FAIL at the time; by 2026-08-10 a third of it was red without a line of
  product code changing, from the harness change described under isolation below
  (TD-2026-08-10-168) — a rebaseline is a snapshot of a measurement REGIME, and
  the file records the numbers but not the regime (TD-2026-08-07-167). A wall/cpu/rss
  failure is evidence rather than something to check against a stale
  record first. Rebaseline the same way: confirm no *allocation* gate is near its
  envelope before rewriting, because those are deterministic and a tight one means
  the code moved — rerecording then buries a regression instead of closing drift
- a scenario must not read the developer's home directory. Every scenario's shell
  loaded `~/.local/state/microide/recents` until 2026-08-07, because
  `PerfHarness::Driver` holds a `WorkspaceShell` **by value** and the isolated
  app-root was established one statement later. `repo_open_rss_idle` read 369
  allocations on a fresh machine and 627 on a used one, from identical code
  (TD-2026-08-07-165). Reproducibility is not correctness — that gate measured the
  same wrong number every time
- a `Measure()` body that builds its own input is measuring `operator+`.
  `plugin_status_item_update` was 95% scenario scaffolding and could not have seen
  a regression in the function it names (TD-2026-08-06-159's tail pass). Build
  scenario inputs outside the measured window. **Every phase has now been checked**
  — `tools/audit-perf-phase-scaffolding.py`, result in
  `dev-docs/performance/perf-phase-scaffolding-audit.md`: 1 of 115 above the 20%
  threshold, and that one legitimately (TD-2026-08-07-163). Re-run it after adding
  scenarios, and `CheckPerfMeasureBodiesDoNotBuildTheirOwnInput` now enforces it
  per-`Measure`-body — `perf-measure-builds-input: <reason>` in the body is the
  declared exemption for a scenario where construction IS the work
  (TD-2026-08-07-166)
- each scenario runs in its own child process (TD-2026-08-06-152), so a metric is
  a property of the scenario rather than of whatever ran before it. `--no-isolate`
  restores the shared-process form for a debugger or profiler. **The harness pumps
  three frames on the bare driver before the measured loop**, because that
  isolation moved a cost the baselines never contained: the first frames a PROCESS
  paints do ~8,600 allocations no later frame repeats, and while the suite shared
  one process only the first scenario paid it. Without the warm-up, 34 of 100
  scenarios failed p95/max allocations with a p50 that matched their baseline
  exactly (TD-2026-08-10-168) — a tail-only divergence is a signal to suspect the
  harness, not the code
- **a scenario that cannot run fails the run.** A missing fixture used to print a
  line and `return`, so the empty iteration was graded and PASSED —
  `editor_moby_dick_workout` reported 7 allocations against a baseline of 138,599
  (TD-2026-08-10-170). One policy now: `RequireFixture` →
  `ScenarioContext::SkipScenario` → `SKIP` plus a failed run for a gated scenario.
  Every fixture generator is a ctest setup test under `FIXTURES_SETUP
  perf_fixtures`, all with `--ensure` semantics. Remember the general shape: every
  gate here is one-sided, so nothing detects a measurement that COLLAPSED
- add regression coverage with every bug fix; never rely on "should be covered
  already"
- every `ScenarioContext::Measure` phase carries its own allocation gate
  (114 of them across 82 baselines). A scenario total is mostly setup, so the
  phase is the number that means what the scenario's name says; a baseline phase
  the run stops measuring FAILS rather than disappearing quietly
  (TD-2026-08-06-153)
- a scenario must leave the fixture tree exactly as it found it. One that did not
  had grown a fixture's worktree diff from 3 lines to 2,725 over 1,361 runs, and
  four gated scenarios had been measuring the accumulation (TD-2026-08-06-155)
- keep retained-redraw comparison tests serial under SDL dummy video — they
  share global SDL state
- measure with `MICROIDE_PERF_TRACE=1` before and after; code review does not
  confirm performance impact
- LTO is useful but is not a substitute for profiling a render hot path
- preserve the current redraw architecture unless profiling shows a new
  hotspot; what remains is policy tuning and coverage, not a redraw rewrite
- promote disk caching or parallel plugin syntax parsing only if profiling
  shows plugin Lua parsing / regex compilation as material startup cost

## Deferred Or Out Of Scope

Not current work unless deliberately promoted into its own phase.

- **Non-Linux host backends.** Linux is the supported host. The `src/platform/*`
  seams stay because they keep the boundary testable, but native Windows/macOS
  subprocess, terminal, and file-watcher backends are not being built, and the
  poll/snapshot watcher fallback is the accepted permanent baseline elsewhere.
- **Hosted perf gating.** Perf baselines are absolute timings from the pinned
  `perf-runner-v1` machine, so CI cannot re-measure them. CI *does* enforce that
  a changed baseline carries a `perf-baseline:` justification. Rebaselining
  stays a local, deliberate act. (General CI is no longer descoped — see
  `.github/workflows/checks.yml`.)
- **Plugin marketplace, remote install, signed-plugin verification, project-local
  plugin loading, Micro-plugin compatibility.** Ships `--disable-plugins` /
  `--safe-mode` only.
- **Cloud and collaboration features.**
- **AI/LLM runtime surfaces.** Retired from product scope; the
  authentication-provider and secret-storage surfaces went with them.
- **Settings overlay follow-ups.** Section subtitles are a static table keyed by
  category label, so a plugin-contributed category renders a title with no
  subtitle; there is no per-category collapse/expand; the section header band is
  fixed. Shipped scope is the scrollable category rail, fixed section header, and
  subsection sub-headers.
- **Multi-window.** Single-window is a product decision
  (`openspec/specs/product-vision/spec.md`), not a gap.

## Retired Docs

`ROADMAP.md` is gone (2026-08-04). It was last reviewed 2026-06-17, it was not in
`CLAUDE.md`'s source-of-truth list, and it contradicted shipped reality — its
"do not expand this phase into: debugger/DAP support" line described work that had
already shipped in v2.0.0, before that review date. Two forward-looking documents
that disagree are worse than one. This file is the only one now.

## Companion Docs

- `openspec/specs/product-vision/spec.md` — product thesis, priority order, non-goals
- `openspec/specs/diff-merge-editor/spec.md` — compare/merge behavioural contract
- `openspec/specs/performance-budgets/spec.md` — performance budget policy
- `AGENTS.md` — engineering policy, iteration loop, do-not-regress patterns
- `dev-docs/project/implementation-guide.md` — durable product and subsystem map
- `dev-docs/project/ui-invariants.md` — surface rules that were each broken once
- `dev-docs/project/known-tech-debt.md` — open, actionable debt
- `dev-docs/project/git-workstation.md` — supported / unsupported workflows
- `dev-docs/project/release-checklist.md` — tag, artifacts, tested-workflows matrix
- `dev-docs/project/editor-essentials.md` — language contract, folding, shaping, save normalization
- `dev-docs/performance/performance-findings.md` — shipped performance wins worth preserving
- `dev-docs/performance/startup-tracing.md`, `runtime-profiling.md` — profiling workflows
- `dev-docs/plugins/plugin-runtime-research.md` — plugin architecture notes
- `dev-docs/design/text-surface-unification.md` — text-input interaction contract
- `SECURITY.md` — trust model, safe mode, reporting
