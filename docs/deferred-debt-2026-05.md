# Deferred debt (May 2026)

Companion to the three May 2026 inventories
(`tech-debt-inventory-2026-05.md`, `ui-ux-debt-inventory-2026-05.md`,
`test-coverage-debt-inventory-2026-05.md`). Each of those passes addressed the
focused, low-risk findings inline and explicitly deferred a tail of larger items.
This document collects those deferred items in one place with the rationale for
deferral, the expected value, and a rough effort estimate, so a future
prioritisation pass has the context without re-deriving it.

**Effort scale (rough, single-engineer):**

- **S** — under a day
- **M** — 1–3 days
- **L** — about a week
- **XL** — multiple weeks, likely needs design discussion

**Value scale:**

- **High** — visible to users or eliminates a recurring risk class
- **Medium** — improves contributor confidence or coverage
- **Low** — nice-to-have, polish

---

## From the tech-debt inventory

### Split `TextViewport.cpp` (~3,077 lines)

- **Why deferred:** the inventory flags this as a size/risk concentration, not a
  concrete defect; existing architectural lint already pins the dangerous
  behaviours (no `ApplyLineEdit` full-document copies, no
  `BuildEditorViewModel` non-incremental writes). Splitting a 3k-line TU
  touches many call sites and the resulting layout decisions are not obvious
  from the file alone.
- **Value:** Medium — review load and merge-conflict surface drop; faster
  iteration on viewport edits.
- **Effort:** L — careful slicing (selection ops vs. fold ops vs. caret motion
  vs. rendering hooks), running ASAN + fuzz after each slice.
- **Recommendation:** wait until a specific viewport change forces a touch on
  the file, then split the affected slice. Avoid a "split for split's sake"
  PR.

### Split `CompareModel.cpp` (~1,165 lines), `FileIndexWatcher.cpp` (~1,268), `ProcessBackend.cpp` (~1,105)

- **Why deferred:** same reasoning as `TextViewport`. The platform bridges
  (`FileIndexWatcher`, `ProcessBackend`) are large because they aggregate
  per-OS backends; splitting along that natural seam is straightforward but
  produces a wide diff with little behavioural change.
- **Value:** Medium — improves reviewability; opens the door to per-OS test
  isolation.
- **Effort:** M per file.
- **Recommendation:** opportunistic — split when the next platform-specific
  feature lands.

### Route `editor::SyntaxDefinitionLoader` through `plugin::LuaRuntime`

- **Why deferred:** the loader's direct `lua_State*` use is intentional for
  runtime syntax loading and predates the tighter "Lua only behind
  `LuaRuntime`" policy. Reconciling means either widening `LuaRuntime` to
  expose a syntax-definition mode, or moving syntax loading into a plugin-host
  helper. Both involve real design choices.
- **Value:** Medium — collapses the architectural exception and removes the
  documentation drift around it.
- **Effort:** M–L — non-trivial because syntax loading happens at editor
  startup and needs predictable lifetime against the plugin runtime.
- **Recommendation:** only worthwhile if the plugin runtime gets a second
  caller that wants similar isolation. Otherwise document the exception
  (already partly done via `LuaErrorMessage.h` extraction) and move on.

### Fully unify `ReadStringField` Lua helpers

- **Why deferred:** the two existing variants have genuinely different
  paradigms — `editor/SyntaxDefinitionLoader`'s
  `bool ReadStringField(..., out, error)` is fail-loud; `plugin/...`'s
  `std::optional<std::string> ReadStringField(...)` is fail-quiet. Merging
  them means picking one paradigm and rewriting every call site.
- **Value:** Low — minor; the truly shared sliver (`LuaErrorString`) was
  consolidated.
- **Effort:** M — touches every Lua-table parser in `src/plugin/*`.
- **Recommendation:** skip unless a third caller appears.

---

## From the UI/UX inventory

### Choose a single authoritative "status narrative"

- **Why deferred:** the inventory itself flags this as **Needs design
  decision** — pick whether the footer bar subsumes plugin/LSP summaries, or
  whether the breadcrumb stays the extension surface. Either choice involves
  spec edits + render-path moves. Not a debt I should resolve unilaterally.
- **Value:** Medium — eliminates the "which strip is authoritative?" confusion
  for users and plugin authors.
- **Effort:** M — code is small; the lever is the design call.
- **Recommendation:** raise as a small product/design proposal first, then
  implement.

### Multi-caret per-caret-selection surround

- **Why deferred:** real feature work, not documentation or cleanup. The
  multicursor spec's "Purpose: TBD" was a symptom; the underlying engine work
  is what's missing.
- **Value:** Medium — power-user editing capability; ties off a "partial"
  flag in `active-work.md`.
- **Effort:** L — needs careful interaction with snippet sessions and undo
  stack ordering.
- **Recommendation:** track on the editor backlog; not a cleanup pass.

### Diff / merge keybinding onboarding

- **Why deferred:** documentation/UX surface, not a bug. Adding an in-app
  cheat sheet or hint row would help discoverability without changing
  semantics.
- **Value:** Low–Medium — better first-use experience for compare/merge.
- **Effort:** S–M — a small render-time legend or a help overlay row.
- **Recommendation:** worth tackling alongside any future merge UX change.

### Project search progress denominator ("X of Y files")

- **Why deferred:** new feature, not cleanup. Requires producer/consumer
  changes in the search service to report indexed-file totals.
- **Value:** Medium — visibly better feedback on large repos.
- **Effort:** M — touches `ProjectSearchService`, the sidebar and overlay
  summaries, and probably the perf harness.
- **Recommendation:** good candidate for the next search-UX iteration.

### Git tree-vs-bar cleanliness reconciliation

- **Why deferred:** the footer "Project" segment OR's tree-snapshot and
  sidebar-git-snapshot cleanliness — a refresh-lag edge case. The fix is to
  resolve which snapshot is authoritative or surface a hint when they
  disagree.
- **Value:** Low — only matters during transient refresh windows.
- **Effort:** S — once the design choice is made.
- **Recommendation:** defer until a user-visible disagreement is reported.

### Compact terminal hit-pad regression coverage

- **Why deferred:** the existing `responsive-shell-layout` spec already
  mandates the hit pads; the inventory called out *perceptual* regression
  risk in compact mode, not a known break.
- **Value:** Low — preventative.
- **Effort:** S — one focused regression test.
- **Recommendation:** add the test when next touching compact-mode chrome.

### Accessibility / AT bridge

- **Why deferred:** explicit **Needs design decision** — current product is
  custom-rendered SDL with no AT layer. Picking a baseline (explicit non-goal
  vs. narrow AT bridge milestone) is a product choice.
- **Value:** High — opens the IDE to a meaningfully larger audience; informs
  whether `responsive-shell-layout` WCAG citations stay pointer-only.
- **Effort:** XL — anything beyond "documented non-goal" is multi-week work
  with platform variation.
- **Recommendation:** decide the stance first (probably "documented non-goal
  for v1, narrow bridge milestone tracked separately") before any code.

### Fill `Purpose: TBD` paragraphs in 11 remaining specs

- **Why deferred:** writing intent for archived specs without input from
  their authors risks fabricating history. `workspace-status-bar` was filled
  because the active wiring made the intent reconstructible from current
  code.
- **Value:** Low–Medium — reduces onboarding friction for planners.
- **Effort:** S per spec, but cumulatively M; pair-review each paragraph with
  the original author or a recent contributor.
- **Recommendation:** opportunistic — fill each as the spec is next
  consulted.

---

## From the test-coverage inventory

### `PersistenceService` orchestration regression tests

- **Why deferred:** existing `WorkspaceShellSessionTests` and project tests
  already cover restore indirectly. Adding named tests would require
  fixture-heavy session-restore scaffolding that mostly duplicates what the
  shell tests exercise.
- **Value:** Medium — explicit coverage when format/order changes happen.
- **Effort:** M — fixture work to seed `PersistedRecord` files at known
  versions.
- **Recommendation:** add named tests *when* the next persistence format
  change lands, not pre-emptively.

### IME / composition tests

- **Why deferred:** needs SDL composition-seam knowledge that isn't visible
  from a coverage pass alone. Writing useful tests requires understanding the
  specific composition path through the single-line editor and the editor
  viewport.
- **Value:** Medium — IME bugs are user-visible and easy to regress.
- **Effort:** M — likely needs a small composition-fake at the input-event
  boundary.
- **Recommendation:** write the first one when an IME bug is reported; use
  the seam discovered then to backfill coverage.

### Hermetic git mocks

- **Why deferred:** large infrastructure change. Existing tests spawn real
  `git` and are vulnerable to per-machine version drift, but the cost of
  replacing every call with a subprocess mock is large and the flake rate is
  currently low.
- **Value:** Medium — eliminates a low-frequency flake class; faster local
  test runs.
- **Effort:** L — needs a shared subprocess-mock seam and porting
  `GitService` / `GitBlameService` / shell git fixtures.
- **Recommendation:** prerequisites first (the subprocess seam), then port
  the noisiest fixtures.

### PTY scripted-write fakes

- **Why deferred:** PTY timing flakes are a known class. Eliminating them
  needs a scripted-master abstraction in `TerminalSession`; that's a real
  design change, not a fix.
- **Value:** Medium — reduces terminal-test flake.
- **Effort:** L — touches `TerminalBackend` and `TerminalSession`.
- **Recommendation:** schedule alongside the next terminal feature.

### Perf fixture-absence gates

- **Why deferred:** `microide_perf` currently *skips* scenarios when fixtures
  are missing; CI may silently miss regressions. Switching skip → fail is a
  CI semantics change with implications for local dev (where fixtures may be
  deliberately absent).
- **Value:** Low–Medium — closes a quiet-failure path in perf gates.
- **Effort:** S — opt-in flag (e.g. `--require-fixtures`) used only in CI.
- **Recommendation:** small, worth doing soon; pair with a CI-only invocation
  using the flag.

### Split `microide_tests` + CTest `RUN_SERIAL` locks

- **Why deferred:** only relevant *when* splitting the test binary. Today
  everything runs serially inside one binary, so the global SDL state risk
  is contained.
- **Value:** Low — preventative.
- **Effort:** M — depends on how the split is structured.
- **Recommendation:** revisit only if the single-binary build time becomes a
  problem.

### Fill remaining `Purpose: TBD` specs

- See the matching item under UI/UX above — same reasoning and
  recommendation.

---

## Triage suggestion

If a future cleanup pass has budget for two or three items, the
highest-leverage choices are:

1. **Perf fixture-absence gates** — small effort, eliminates a silent-failure
   class in CI.
2. **Project search progress denominator** — medium effort, visible UX win.
3. **Choose the status-narrative authority** — small code change once the
   design call is made; eliminates a recurring source of confusion in plugin
   docs.

Everything else benefits from being opportunistic: touch it when an adjacent
change makes the cost incremental rather than standalone.
