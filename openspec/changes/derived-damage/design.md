# Design

## Context

- Frames are prepared once (`WorkspaceShell::PrepareFrameOnce`) and then painted
  through one or more damage clips. Damage reaches prep two ways: imperative
  `RequestRedrawRect` calls from event handlers, coordinators and services, and ~40
  "painted_*" comparisons in `PreparedFrameState` that prep already derives
  (scrollbars, dividers, tab labels, menu rows, breadcrumb labels, the project
  strip...). The second way is this design, done one bug at a time.
- Render TUs already consume view models from `RenderViewModelBuilder` and a lint
  (`CheckRenderSurfaceStateAccess`) keeps them off live project state -- but several
  surfaces' models are partial (they hold pointers or read the viewport directly),
  and the editor surface paints straight from `TextViewport` through
  `EditorViewRenderer::Render`.
- `EditorViewRenderer::Render` skips rows outside the current clip: building every
  visible row's decorated text per clip cost typing ~40% once. Building per-row
  paint inputs for every visible row on every frame is therefore the one place this
  design can cost real time.
- The QA redraw verifier (`MICROIDE_QA_VERIFY_REDRAW`) compares every partial
  frame with a full render; it is the oracle for every stage, unchanged.

## Goals / Non-Goals

**Goals:**
- A migrated surface cannot show stale pixels because a damage request was missing.
- The guarantee is mechanical (snapshot + lint), not a review rule.
- No paint scenario's p50 up more than 5%, no new steady-state allocations.

**Non-Goals:**
- Clip-edge and in-clip stale paint (drawing outside a rect, fractional-scale
  rounding); a separate clip-discipline change.
- Removing imperative damage requests wholesale: they stay as hints (they make the
  common case cheap and immediate) and are pruned only where a snapshot makes one
  redundant and it is on a hot path.
- Changing what any surface draws.

## Decisions

### D1. Snapshot = 64-bit fingerprint per damage unit, built at prep

Each unit's snapshot is a hash (the codebase's FNV-1a helpers, or a faster 64-bit
mix if the spike shows FNV is the cost) folded over exactly the fields its paint
reads, plus the unit's rect. Prep keeps, per unit, the fingerprint and rect it last
painted; a unit whose fingerprint or rect changed is damaged (old rect and new).

*Alternatives:* storing full copies and comparing them (memory per unit, copying
strings each frame -- rejected); hashing rendered pixels (requires painting the
whole window -- that is the rejected full repaint); keeping per-input revision
counters (back to enumerating inputs by hand, the failure mode itself -- rejected).

### D2. Completeness comes from paint reading only the snapshot's source

The fingerprint is computed from the same view-model struct paint consumes, by the
builder that fills it. A field paint reads is therefore a field in the struct, and a
field in the struct is in the hash: the hash function is written next to the struct
and a unit test asserts every field changes the fingerprint (a reflection-free
field-count check per struct). The render-surface lint is extended so a migrated
TU's paint reads only its view-model parameters and immutable resources.

### D3. Grain per surface

- Whole surface: status bar, breadcrumb row, title/menu bar, project strip.
- Per item: tab strips (per tab), bottom-panel tab row, debug-pane rows, sidebar
  rows (visible rows only).
- Per row: editor panes (visible rows, gutter included), compare/merge panes.
- Terminal bodies: per row (`DerivedSurface::BottomPanelTerminalGrid`). The session
  keeps one generation, not a dirty bit per row, so a pane hashes its visible
  lines' cells when it takes a new snapshot (`KeyTerminalGridRows`, never per
  frame) and each row's unit folds that key with what the frame paints over the
  row: its slice of the selection, its find matches, the hovered link. Their
  chrome (find bar, tab row) is per surface.

Coarser grain costs extra paint when a unit changes; finer grain costs more hashing
per frame. The spike (D5) picks the editor's grain by measurement.

### D4. Editor rows: per-row paint inputs move to prep

Today paint builds each visible row's inputs (line text, syntax spans, selection,
search/occurrence/bracket fills, diagnostics, inlay and EOL decorations, fold
marks, gutter text, blame) inside `Render`. They move into a prep-time pass that
produces a `RowPaint` record per visible row into reused scratch (no allocation in
steady state), fingerprints each, and hands the records to paint. Paint then draws
damaged rows from their records and no longer gathers inputs itself.

Net cost: today a keystroke gathers inputs for the damaged rows only; after, prep
gathers them for every visible row. The spike measures this before anything lands.
If it exceeds the budget, the fallback keeps the record pass lazy: a row's
fingerprint is built from cheap per-line keys (line content version, the overlay
stores' per-line revisions, caret/selection line spans) and its full record is
built only when it is damaged. The fallback reintroduces a list of inputs, so it is
taken only if measured necessary, and then the field-completeness test of D2 runs
against the record (which paint still draws from) rather than the key.

### D5. Stage 0 spike decides the editor strategy

Before the editor stage, a spike builds and fingerprints every visible row at prep
behind a debug toggle (nothing consumed), and measures `editor_typing_paint`,
caret blink, `scroll_large_file`, and a 4K-window variant, base vs spike,
interleaved, same disk (the method in the perf harness docs: the gate is noisy on
this machine, only A/B on one disk is evidence). Decision rule: within budget ->
D4 as written; over budget -> D4's lazy fallback.

### D6. Hand-written painted_* comparisons retire as snapshots subsume them

Each stage deletes the `PreparedFrameState` fields its snapshots cover (and the
prep code that compared them), so the derivation lives in one place per surface.

## Risks / Trade-offs

- [Prep cost on every frame, including caret blinks] → the caret is its own unit
  (a rect, not part of a row's fingerprint), so a blink hashes nothing else; the
  spike measures the rest; budget enforced per stage.
- [Over-damage from inputs that change without changing pixels (a revision bumped
  by an edit elsewhere)] → fingerprints fold values, not revisions, where a
  revision is coarser than the row; perf counters (`units damaged per frame`)
  expose over-damage in perf-tests.
- [Snapshot and paint drift: a later edit makes paint read something outside the
  struct] → D2's lint and field-completeness test fail the build.
- [Hash collision leaves a stale unit] → 64-bit fingerprints; probability per frame
  is negligible next to the class being removed.
- [Large one-shot refactor of `EditorViewRenderer::Render`] → staged: records are
  introduced alongside the existing path, paint switches over, then the old
  gathering is deleted; the redraw verifier and the QA lane run at each step.

## Migration Plan

Each stage is one or more commits to `main`, each green on `tests`, `qa` (redraw
verifier), `perf-tests`, and the base-vs-head perf A/B:

0. Baseline numbers and two counters (units fingerprinted, units damaged per frame).
1. Framework + a whole-surface pilot (status bar and breadcrumb row): proves the
   mechanism, the lint and the completeness test on a low-risk surface.
2. Editor spike, then editor rows (D4/D5). Largest share of sightings (60-65%).
3. Bottom panel and right pane (debug pane rows, panel tab row).
4. Tab strips and sidebar rows.
5. Retire subsumed `painted_*` fields; update `architecture-invariants.md` and the
   handbook's render-path rules.

Rollback: every stage is self-contained; reverting a stage restores its surface's
imperative damage, which still exists until stage 5.
