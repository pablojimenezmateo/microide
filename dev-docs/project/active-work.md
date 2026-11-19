# MicroIDE Active Work

QA coverage follow-up (2026-10-23): twelve added scenarios cover selection and
replacement transactions, tab/group close and save lifecycles, path identity and
file formats through restart, scoped project replacement, query recovery and
canceled tree operations. Driver regression tests now run in each local QA lane;
startup errors retain evidence without aborting the suite, polling honors the
slow-build allowance, and duplicate scenario keys fail import. Continue closing
interaction gaps using exact buffer/disk receipts alongside the retained-redraw
oracle; command dispatch coverage alone does not establish correct behavior.

Reviewed 2026-10-07 (remote-projects groundwork: the G11 parity harness with ten
local/remote rows including real gdb and clangd, LSP/DAP path translation at the
transport, `${workspaceFolder}` in host commands, the project's locality fixed at
open, git metadata asked of the host, and every git/formatter wait the parity
counts exposed moved off the shell thread; completion that follows typing and
opens itself, once per word; see § 7 and `known-tech-debt.md` § TD-2026-10-07-320
through 323). Previously reviewed 2026-09-29 (remote-projects groundwork G4: the asynchronous open, the
guarded completion every off-thread buffer operation shares, and the dedicated
reader; see § 7 and `known-tech-debt.md` § TD-2026-09-29-312 through 316).
Previously reviewed 2026-09-22 (groundwork G1, G2, G3 and part of G9; see
`known-tech-debt.md` § TD-2026-09-22-301, 303). Previously reviewed
2026-09-05 (see `known-tech-debt.md` § TD-2026-09-05-288 for that
pass). Shipped baseline: **v2.12.0** — the multi-caret / soft-wrap / word-selection
correctness pass shipped in it, with Add Cursor Above/Below, Add Cursors to Line
Ends, Join Lines, and the non-ASCII glyph cluster atlas (see `CHANGELOG.md`
§ 2.12.0).

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
- `AssistService` is an owner of six feature units in `src/workspace/assist/`
  (`CompletionAssist`, `SnippetAssist`, `CodeActionAssist`, `NavigationAssist`,
  `RefactorAssist`, `SignatureHelpAssist`) over one `AssistHost`. Its
  `AssistHost::Operations` seam is shared by all six; it should shrink into
  per-feature ports over time, not grow into a shell callback bag

### 3. Terminal hardening

The emulator covers the full-screen and shell workflows exercised so far. As of
2026-10-06 the panel around it is agent-shaped: a tab is a group of up to six
side-by-side panes, the panel maximizes into an immersive mode that leaves only
the menu bar and project strip (and restores itself the moment another surface
takes focus), background tabs carry an activity dot, and an exited pane relaunches
in place with Enter (the dropped-ssh case). All of it lives on
`TerminalPanelService`; the shell's declaration surface is at its lint cap, so
new terminal behaviour lands there.

- broaden validation with actual terminal programs rather than extending escape
  coverage from guesswork
- fill remaining ANSI gaps only where real usage justifies them
- keep resize, redraw, scrollback, and wake behaviour robust under long-running
  output
- the pane model is complete as of 2026-10-13: the panel is revived across
  sessions (tabs, splits, shares, each shell's directory, maximized; fresh
  processes, as VS Code's revive), a split inherits its pane's directory, links
  in an inactive pane hover, and every mouse button activates the pane it lands
  on. What VS Code has beyond that is process persistence (a terminal surviving
  a window reload with its scrollback), which is a host-daemon feature here: as
  of 2026-10-27 a remote project's host terminals survive quitting microide and
  closing the project, and reopening reattaches the same shells with their
  screen and history (remote-terminal-restore, below). Local terminals still
  revive as fresh shells.
- keyboard and pointer ownership is settled as of 2026-11-02 (TD-2026-11-02-428):
  a focused terminal gets every key but the reserved chords
  (`IsTerminalReservedChord`: Ctrl+Shift+<key>, Ctrl+`, Ctrl+PageUp/PageDown,
  Ctrl+Tab), linted for every built-in; a program that owns the mouse gets the
  arrow, the hand over a control it lights on hover (`TerminalHoverAffordance`)
  and Shift-to-select past it; motion is reported once per cell. New workbench
  shortcuts that must work from a terminal go on a reserved chord. Next on this
  axis: OSC 22 (a program naming its pointer shape) when a program people use
  sends it.

### 4. Editor correctness and scale

- continue UTF-8 and IME hardening over the piece tree's byte-offset storage
  (32-bit offsets, ~4 GiB per-file ceiling). The composition path now has
  coverage (`tests/ImeCompositionTests.cpp`): the preview is recorded, replaced,
  abandoned on an empty update, cleared on commit, committed to every caret, and
  the conversion keys it swallows are pinned against the buffer. What is still
  open here is the piece-tree side, not the SDL composition plumbing.
- validate large-file thresholds on larger repositories; adjust from measured
  behaviour, not guesswork
- keep blame shadow text asynchronous, viewport-scoped, and cheap enough to
  preserve typing and scrolling latency
- expand compare/merge coverage — editor-side regressions there are still too
  easy to miss. The shared soft-wrap row table has direct tests
  (`tests/DiffWrapLayoutTests.cpp`, `tests/MergeWrapRowsTests.cpp`), and the
  surfaces' own edit paths are now swept over EVERY registered action rather than
  one remembered verb: `WorkspaceShell/CompareEveryActionKeepsTheDiffModelInSync`
  and `WorkspaceShell/MergeEveryActionKeepsConflictTrackingHonest`. The merge
  sweep found the open half of that gap — only five call sites re-tracked the
  conflict spans, so every shaping verb left them pointing at lines that had
  moved (fixed 2026-09-14). The compare surface's own row loop and key handler
  (TD-2026-08-13-206/207) were folded into the editor's on 2026-08-14.
- soft wrap is checked as a PROPERTY now, not as fixtures:
  `tests/EditorWrapNavigationPropertyTests.cpp` walks every visual row (with and
  without a collapsed fold in the way) requiring one press to be one row, and
  `tests/EditorWrapInvarianceTests.cpp` runs the same random edit sequence on two
  viewports that differ only in wrap and requires identical bytes and carets. The
  same pass fixed a secondary caret painted by two rows at a wrap boundary and one
  painted by none at the end of a wrapped line.

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
- **the allocation half was re-recorded 2026-08-12** (TD-2026-08-11-184, closed):
  eight gates, 42-98 % tighter, on the canonical `microide-perf` lane with
  `--update-baseline=deterministic`. What unblocked it: a baseline can now declare
  its timing half ADVISORY (`timing_is_advisory`). The deterministic metrics
  (allocations, phase allocations, net heap) gate normally and the
  machine-sensitive ones are measured, printed, and explicitly not enforced with
  the reason on the verdict line — so "no idle box available" no longer means "no
  gate at all", which is what had left two soft-wrap scenarios gating on nothing
  (TD-2026-08-12-186)
- **`p50_net_heap_bytes` is a retention measure only for allocations freed on
  the same thread inside the same window** (TD-2026-08-12-191, resolved — the
  four red gates were an in-window RSS-probe allocation, a scenario measuring
  its own fixture, and three scenarios whose frees land on the writer thread;
  the last three carry `gate_net_heap_metrics = false` with the reason at the
  registration). The metric is also **build-configuration dependent**: baselines
  record `build_config` now, and a mismatch unenforces exactly the metrics that
  move with it (wall, cpu, `mean_rss_growth_bytes`, `p50_net_heap_bytes`) with
  both names on the verdict line. `rss_growth_bytes` is the cross-check that
  tells a real retention from a thread-accounting artifact
- **the wall gate is normalised against the machine clock as of 2026-08-12**
  (TD-2026-08-06-140, step one), weighted by each iteration's own cpu/wall ratio —
  full correction where wall is work, none where wall is sleep. Cutting the
  100/150/200 envelopes down to something that actually gates is step two and
  needs an idle runner plus a multi-run stability measurement
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
- **a `Measure()` body that is a wall-clock wait gates the runner, not the code.**
  `linter_on_save`'s gated phase wrapped a 120 ms `WaitForDiagnostics` deadline
  loop that pumps a frame per poll, so its allocation count was "how many times a
  loop got round" — 3,561 / 3,644 / 3,711 across three runs of one unchanged
  binary, against a 2,745 baseline, gated at 10 %. Worse, the wait had **never
  once succeeded**: nothing here lints JavaScript, so it always timed out and the
  poll loop WAS the measurement (TD-2026-08-10-179). Drive the path instead of
  waiting for it, and `SkipScenario()` when the setup does not land. The other
  `WaitFor*` helpers were audited clean the same day and the check is a lint now;
  a wait that belongs inside a measured window declares itself with a
  `perf-measure-waits-on-clock:` comment and the three-run determinism evidence
  (TD-2026-08-10-180, resolved)
- **`p50_net_heap_bytes` can move 260x with retention completely unchanged, and
  `mean_rss_growth_bytes` is what adjudicates.** The metric counts
  `operator new` bytes inside the measurement window, so moving an allocation's
  *timing* moves it — even when nothing about what is retained changes. Chunking
  the piece tree's add buffer did exactly that: the old single `std::string`
  reserved capacity in one iteration and absorbed several iterations' appends into
  it for free, so `operator new` saw nothing while the pages were touched anyway.
  `editor_toggle_comment_large_selection` went 10,661 → 2,815,870 while its
  `mean_rss_growth_bytes` sat unmoved at its long-standing 2,811,221 baseline —
  the two now agree to 0.2 % where they used to disagree by 260x
  (TD-2026-08-14-234). Before calling a net-heap jump a leak, check whether the
  resident number moved with it; if it did not, the accounting moved, not the
  memory.
- **a geometric buffer makes a scenario's retention series bimodal, and the p50
  picks whichever mode the iteration count lands in.** `editor_smart_indent_typing`
  read 26,348 against a 14,004 baseline at the default 10 iterations and 1,876 at
  25, from a byte-deterministic series that was a ~1,800-byte floor plus growing
  doubling steps. The gate was reading where the doubling boundaries fell, not what
  the scenario retains (TD-2026-08-14-233). A net-heap gate that moves with the
  iteration count is measuring an allocator growth curve.
- **check the allocation tracer's drop warning before reading its table.** An
  aggregating instrument with a bounded key space drops each new site whole when
  full, so the listing stays correctly sorted and becomes wrong. At the old
  1024-bucket size, 74 % of a phase was dropped and the printed "#1" was really its
  #4 (TD-2026-08-10-178). Make an overflow message state the consequence, not the
  remedy
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

### 7. Remote projects: shipped

Remote projects have shipped: a project opens on a host over ssh with a
per-uid `microide-server`, a local mirror, host terminals that survive a quit,
host git, path translation for language servers and debuggers, and a Remote menu
and Host view. The requirements live in `openspec/specs/remote-*` (with
`notification-actions`, `save-continuations`, `project-identity`,
`performance-budgets`, `workspace-architecture`); the latency budgets are measured
by `tests/perf/RemotePerfScenarios.cpp`. The dated record of the groundwork
(G1-G11) and Phases 1-3 moved, verbatim, to
`dev-docs/archive/remote-projects-groundwork-history.md`.

Nothing on the remote side is open beyond what the soak's remote job finds; the
two follow-ups this section used to list (TD-2026-10-08-325, -326) were resolved
in October.

The agent-shaped workflow -- agents running on the host, the user supervising from
one machine or another -- got its own pass on 2026-11-18 (`CHANGELOG.md`
§ Unreleased): terminal alerts that reach past an unfocused window, a pasted
screenshot and a `path:line` reference handed to a terminal, the host's free disk
in the Host view, a worktree pick, review since a marked checkpoint, File History
from the mirror's object store, and the files open in a project offered to the
next machine that connects. A second pass on 2026-11-19 closed what daily use
of that workflow still tripped on: a host program's `$EDITOR` opens a tab
(`git commit`, an agent CLI's prompt editor), `Ctrl+Shift+N` goes to the next
terminal that rang, a review's notes are copied or typed into a terminal as
`path:line: note` lines, Git: Review Commit… lists recent commits, Switch
Worktree says what waits in each tree, a terminal attached from two machines is
the size of the one typing, and review marks and notes travel between machines.
Deliberately left out: rendered markdown or image views, and anything that would
make the editor an agent harness (`openspec/specs/non-ai-product-scope`). Not
done, and worth doing only when asked for: File History exists for remote
projects only (a local project has no mirror holding earlier contents), and a
local project's terminals take neither `$EDITOR` nor `$BROWSER` requests (their
programs reach the desktop and their own editor themselves).

> Decisions already made with the user — do not re-ask:
> - Notification actions are **VS Code-style inline buttons**; each is an
>   `ActionId` + args, never a closure.
> - Quit and close-project **wait for formatter saves with a progress row and a
>   Cancel button**; never freeze, never exit with a save in flight, never exit
>   unformatted on a timeout.
> - Everything in the design's § 11 table (server per uid, socket under `$HOME`,
>   `proc/spawn` with argv arrays, one ssh channel per project, mosh-style
>   prediction, no UDP, self-install, Phase 1 removed).
>
> Seams to build on, not around: `platform::ProcessLauncher` (Run owns cwd
> mapping; `LocalPathFromHost`; `ExpandWorkspaceFolder`), `project::ProjectLocality`
> (fixed at `OpenProjectTab`), `project::GitMetadataFor`, `HostPathTranslator`,
> `FileReadService` with `read_path = false` for off-thread waits, and
> `tests/parity/` (loopback launcher, recording local launcher, spawn-count check,
> shrink-only known gaps).
>
> Pitfalls this codebase has already paid for:
> - Decide a real-tool test's SKIP from a **local reference run**, never from the
>   run under test; the gdb parity row first passed with path translation off.
> - Never capture `this` of a `Make*Coordinator()` value in an async completion;
>   rebuild the coordinator in the completion.
> - Never edit source while `tools/run-checks.sh` is building a sanitizer lane.
> - `ctest | tail` hides the exit status; check `$?` of ctest itself.
> - Run the sanitizer lanes once at the END of a batch, with
>   `MICROIDE_BUILD_JOBS=6` (12 jobs ran out of memory on the ASAN build).
> - No time-based assertions while the machine is under load; parity budgets are
>   counts.

### 8. Safety program: no crash, no lost data

Adopted 2026-11-08, all ten workstreams to be done; detail and tracker in
[`safety-plan.md`](safety-plan.md). Its centre is the QA monkey (W1). It runs
continuously on the development PC, following main and rotating through builds,
displays, scales, fixtures and profiles. It is steered by state and command coverage
and a corpus of interesting prefixes, and it checks data integrity, liveness,
latency, leaks and internal invariants on every step. Every deterministic finding is
minimised into a ready-to-fix bundle for an agent that reviews and fixes them. CI
stays the fast, fixed-seed gate. Around it: the data-loss oracle and fault injection
(W4, W5), assert-armed and cache-invariant builds (W2, W3), scale and sequence
fuzzing (W7, W8), and lints plus an audit that every check still bites (W6, W9, W10).

## Deferred Or Out Of Scope

Not current work unless deliberately promoted into its own phase.

- **Non-Linux hosts.** Linux is the only host. The macOS and Windows code
  paths were removed (2026-10-25) and CMake refuses to configure elsewhere; the
  `src/platform/*` seams stay because they keep the boundary testable.
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

Moved to `dev-docs/archive/` on 2026-11-14, as record rather than direction: this
file's remote-projects groundwork and phase history
(`remote-projects-groundwork-history.md`; remote projects have shipped) and the
2026-07-08 merge-candidate bug audit of a branch long since merged
(`merge-candidate-bug-audit-2026-07-08.md`).

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
- `dev-docs/design/remote-projects.md` — remote-project design (shipped; see § 7 and `openspec/specs/remote-*`)
- `SECURITY.md` — trust model, safe mode, reporting
