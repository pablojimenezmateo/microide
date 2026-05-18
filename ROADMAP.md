# MicroIDE Roadmap

Reviewed on 2026-05-10.

This file is the forward-looking plan for the current branch. `docs/active-work.md` remains the
source of truth for shipped baseline and accepted scope cuts.

This roadmap intentionally does not list every generic IDE feature that could exist. It tracks
the next meaningful slices for this codebase under the repo policy:

1. correctness
2. speed
3. low CPU usage
4. low memory usage
5. maintainability and simplicity
6. compatibility only when explicitly required

## Current Position

The large plugin-platform expansion is no longer hypothetical work. Manual Lua plugins, host-owned
registries, async subprocess plumbing, LSP or DAP transport, and SCM or annotation or auth
provider surfaces are already in the tree.

That changes what the roadmap should optimize for. The next work is not "add another foundation."
It is:

- **prove the diff / merge / git workstation as the validated product wedge.** This is the
  capability set where microide is most clearly differentiated today: shared decorated-text-grid
  pipeline across editor / compare / merge, three-way merge with whole-side apply, `[`/`]` hunk
  navigation, outgoing-base-branch compare, working-tree-aware merge result reopen, and a
  standalone `microide_diff_bench`. Cohere docs, regression coverage, and remaining polish around
  this wedge before broadening scope again.
- validate the shipped foundations (LSP / DAP / tasks / tests / SCM) against real workflows
- harden the remaining host boundaries, especially around `WorkspaceShell` (file decomposition
  is satisfied; ownership decomposition is partial — see `docs/known-tech-debt.md` items 15–16)
- keep UI latency stable while the new runtime surfaces are exercised under load

## Immediate Slice

Finish the validation and host-boundary pass before widening product scope again.

Editor essentials: folding, language contract, indent guides, occurrence
highlights, pair and indent behavior, shaping toggles, and save normalization are
landed; finish partial surfaces (sticky scroll UI, snippet sessions, outline
view, remaining spec tests and perf baselines) under
`openspec/changes/editor-essential-capabilities/tasks.md` and
`docs/editor-essentials.md`.

### 1. Validate shipped async and provider runtimes

Priority:
- P0

Goals:
- validate real LSP, DAP, task, test, SCM, and auth integrations before advertising broader
  support
- confirm that callback delivery, wake routing, cancellation, shutdown, and project switching stay
  correct under repeated concurrent activity
- keep completion, code actions, and similar UI surfaces host-owned and
  minimal while the runtime contracts settle

Exit criteria:
- no runtime request can stall render or input handling
- project switch, save, close, reload, and shutdown behavior remain correct while background work
  is active
- at least one real integration is exercised end to end for each newly shipped platform surface we
  intend to keep claiming as product capability
- regression coverage exists for the callback, cancellation, and fallback paths that were found to
  be fragile during validation

### 2. Narrow the remaining `WorkspaceShell` blast radius

Priority:
- P0

Goals:
- keep shrinking `WorkspaceShell`, `WorkspaceActionContext`, and related coordinator reach now
  that the major shell-breakdown pass has landed
- prefer explicit shell-owned services, registries, and focused callbacks over additional shell
  helper growth
- keep plugin expansion from turning into indirect shell exposure

Exit criteria:
- new plugin or runtime work does not require widening shell friendship or adding broader mutable
  reach into shell internals
- the next significant action, tab, sidebar, tooling, or render paths touched on this branch move
  toward narrower subsystem APIs instead of reinforcing transitional facades
- no new production compatibility shims are added around stale shell boundaries

### 3. Harden project, editor, and terminal correctness under real use

Priority:
- P1

Goals:
- validate auto-reload, path mutation, diagnostics refresh, blame refresh, and compare or merge
  state after on-disk changes
- continue UTF-8, IME, and serialization hardening while the text model remains byte-oriented
- validate the embedded terminal against real long-running and full-screen programs instead of
  extending escape coverage from guesswork

Exit criteria:
- on-disk file changes produce predictable reload or prompt behavior for clean versus dirty buffers
- editor, compare, merge, diagnostics, and blame state stay coherent after rename, delete, reload,
  and external edits
- terminal resize, redraw, scrollback, and wake behavior remain robust under real workloads

### 4. Preserve responsiveness with targeted hot-path work

Priority:
- P1

Goals:
- keep render, text, and input hot paths cheap while the new plugin and runtime surfaces are
  exercised
- profile before choosing larger rewrites, especially in text rendering and invalidation behavior
- keep background worker behavior bounded and cheap under overlap between git, search, blame,
  and language tooling
- close the remaining measurement gaps before making broader perf claims, especially repo-open
  steady-state memory, large-file open-to-first-paint, and sustained compare / merge interaction;
  advisory scenarios may lead, but release/readme claims should wait for baseline-gated coverage

Exit criteria:
- typing, scrolling, drag, resize, and startup traces show no regression
- slow git, blame, search, tooling, or agent work does not starve unrelated background activity
- README / release positioning does not imply benchmark coverage for scenarios that still lack a
  deterministic harness entry
- larger render or text-backend rewrites are only started if profiling shows the current path is a
  stable bottleneck

## Next Phase

After the immediate validation pass, the next coherent phase should be workflow consolidation, not
another registry or protocol land-grab.

### Phase Goal

Turn the newly shipped provider and runtime surfaces into validated product workflows with narrower
host boundaries and no major latency regressions.

### Phase Scope

Focus this phase on:

- real LSP and DAP workflows that MicroIDE actually wants to keep, not protocol checkbox growth
- project, git, and file-watch correctness under active background work
- continued shell-boundary reduction where validation exposes the wrong ownership seam

Do not expand this phase into:

- richer debugger UX beyond what the first-pass runtime already supports
- plugin background execution unless a real plugin is blocked on it
- speculative renderer rewrites without profiling evidence

### Phase Deliverables

Land work in slices like:

1. Real-server validation and bug fixes for LSP, DAP, tasks, tests, and tool execution.
2. Real-world validation and bug fixes for async runtime surfaces and tooling workflows.
3. File-watch, reload, rename, delete, diagnostics, and blame correctness under external changes.
4. Narrower APIs for the shell-owned action, tooling, and render paths that still rely on broad
   coordinator reach.

### Phase Exit Criteria

This phase is done when:

- the shipped runtime surfaces are backed by real end-to-end validation rather than only fixture
  tests
- the highest-friction bugs found during validation have regression coverage
- the next round of feature work can build on narrower host seams instead of widening
  `WorkspaceShell`
- profiling still shows acceptable startup, typing, scrolling, resize, and idle behavior after the
  validation fixes land

## Next Priorities

### 1. Validate real LSP and DAP workflows before broadening UX

Do next:
- add only the protocol methods or runtime features that unlock real product workflows
- validate real server or agent behavior before expanding the advertised capability surface
- keep shell-owned completion and code-action overlays simple until real usage shows a need for
  richer UX

### 2. Keep plugin APIs narrow and registry-first

Do next:
- continue host-owned commands, menus, sidebars, settings, keybindings, status items, tooling, and
  provider registries
- add async plugin execution only when real plugin workloads justify it
- keep rendering host-owned; plugin contributions provide data, commands, or structured requests
- do not expose `WorkspaceShell` wholesale, directly or through growing façade objects

### 3. Tighten project and git service latency

Do next:
- move avoidable filesystem and git refresh work off latency-sensitive UI paths
- tighten subprocess error reporting, cancellation, and retry behavior around the system `git`
  path
- keep all external-tool invocation behind `src/project/*` or similarly narrow compiled service
  boundaries
- do not let plugin glue or UI code parse raw command output directly

### 4. Follow through on render and text performance only where measured

Do next:
- profile the current text-renderer path before choosing an atlas or batched backend rewrite
- pursue finer-grained invalidation in bottom-panel, compare, and merge surfaces only if repaint
  cost remains measurable
- preserve the retained-redraw architecture unless profiling identifies a clear replacement target

## Medium-Term Phases

### 1. Plugin platform hardening

Continue with:
- validation of the shipped plugin runtime against real repo-owned and user-installed plugins
- stronger host boundaries around SCM, annotation, auth, and tooling providers
- deletion of stale adapters, duplicated contribution paths, and accidental compatibility shims as
  the canonical host seams settle

### 2. Editor correctness and scale

Continue with:
- UTF-8 and IME hardening while the core text model remains byte-oriented
- measured validation of large-file thresholds rather than speculative tuning
- asynchronous, viewport-scoped blame behavior that stays cheap enough for typing and scrolling
- broader regression coverage where editor, compare, and merge interactions are still easy to
  break
- complete remaining **editor essentials** work (sticky-scroll band, snippet engine and UI, outline
  sidebar, syntax-aware bracket matching where required, and `tasks.md` section 13 perf baselines); see
  `docs/editor-essentials.md`

### 3. Diff and merge rewrite

The durable behavioral contract for this work lives in `openspec/specs/diff-merge-editor/spec.md`.
The implementation plan lives in `docs/diff-editor-merge-rewrite-plan.md` (create if missing).

Target end state:
- editor, compare, and merge share one row-decoration and text-grid rendering pipeline
- diff semantics do not degrade based on file-size thresholds
- compare and merge highlighting use the same layered rendering contract
- optimization happens after the rewritten semantics are correct and measured, using
  `microide_diff_bench` as the reference before/after tool

### 4. Keep shrinking the shell-centered architecture

Continue with:
- explicit action, render, input, and service seams
- state-scoped controllers and callback-scoped facades instead of wider shell reach
- no new production `friend` access
- smaller renderer inputs or subsystem facades where shell helper reach is still too wide

## Not On This Roadmap

These are explicitly deferred or out of scope unless deliberately promoted into their own phase:

- plugin marketplaces, remote install flows, or Micro-plugin compatibility
- cloud, collaboration, account, or sync features
- recent-project or recent-file surfaces
- debugger UX beyond the already-landed first-pass runtime and command plumbing
- feature work added only because a protocol supports it rather than because MicroIDE needs it

## Deferred Follow-ups From This Change

These were identified during the vision-alignment pass and should each become their own change:

1. **Unified decorated text-grid pipeline** — implement the shared row-decoration and text-grid
   rendering path across editor, compare, and merge as specified in
   `openspec/specs/diff-merge-editor/spec.md`. Requires `docs/diff-editor-merge-rewrite-plan.md`
   to exist first (currently missing — create it as the first task of that change).
2. **Numeric performance thresholds** — once a reference host is declared, add concrete
   millisecond startup, typing, and scroll frame targets to `openspec/specs/performance-budgets/spec.md`.
3. **Large-file diff-semantic correctness audit** — verify that the shipped diff pipeline has no
   file-size-threshold degradation paths; fix any found, per the no-degradation requirement in
   `openspec/specs/diff-merge-editor/spec.md`.

## Working Rules

- every meaningful bug fix adds or tightens regression coverage
- measure before and after performance-sensitive changes
- use `docs/startup-tracing.md` and `docs/runtime-profiling.md` instead of guessing
- update `docs/active-work.md` when a roadmap item lands or priorities materially change
- delete stale compatibility shims and contradictory docs as new seams become canonical

## Companion Docs

- `openspec/specs/product-vision/spec.md`: authoritative product thesis and in-scope/non-goal list
- `openspec/specs/diff-merge-editor/spec.md`: durable compare and merge behavioral contract
- `openspec/specs/performance-budgets/spec.md`: durable performance budget and measurement policy
- `docs/active-work.md`: shipped baseline, active priorities, and accepted scope cuts
- `docs/editor-essentials.md`: editor essentials settings, commands, and plugin contract surfaces
- `docs/implementation-guide.md`: durable product direction
- `docs/known-tech-debt.md`: concrete remaining debt worth preserving as a queue
- `docs/diff-editor-merge-rewrite-plan.md`: detailed compare and merge rewrite plan (create if missing)
- `docs/performance-findings.md`: shipped performance wins worth preserving
- `docs/startup-tracing.md`: startup profiling workflow
- `docs/runtime-profiling.md`: runtime and redraw profiling workflow
