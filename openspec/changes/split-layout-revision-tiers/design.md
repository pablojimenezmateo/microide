## Context

`TextViewport::DocumentState::layout_revision` is a single `std::size_t` (TextViewport.h:255)
bumped from exactly one site — `TextViewport::InvalidateDerivedCaches(start_line)`
(TextViewport.cpp:1599). Every reader in the editor stack uses that same counter
as its cache-validity key:

- `TextViewport::wrapped_row_layouts_revision_` (TextViewport.cpp:3127, 3152, 3213)
- `TextViewport::highlight_state_revision_` (TextViewport.cpp:1473, 1481, 1625, 1677)
- `TextViewport::cached_max_visual_columns_revision_` (TextViewport.cpp:3002, 3073, 3098)
- `EditorViewRenderer::bracket_match_cache_.layout_revision` (EditorViewRenderer.cpp:322–341)
- `EditorViewRenderer::indent_guides_cache_.layout_revision` (EditorViewRenderer.cpp:420–437)
- `EditorViewRenderer` per-line view-model key (EditorViewRenderer.h:107, 136, 148)
- `FoldingModel::Snapshot::layout_revision` (FoldingModel.h:33) — *separately
  owned by the fold model; this design leaves it untouched.*

The current writer is reason-blind: every caller of `InvalidateDerivedCaches` bumps
the same counter regardless of whether it just edited a line, toggled soft wrap,
recolored a theme, or moved a decoration overlay. Round-4 lazy invalidation
already made the reset O(1) for syntax state; the remaining cost is **readers
re-running** because their stored revision lags the now-bumped global revision.

`FoldingModel` already owns its own `layout_revision` and is independent of the
document counter, so the tiers we introduce live exclusively on
`TextViewport::DocumentState`.

## Goals / Non-Goals

**Goals:**

- Split the single `DocumentState::layout_revision` into four independent
  counters representing four distinct causes of cache invalidation:
  `content_revision`, `syntax_revision`, `layout_shape_revision`,
  `presentation_revision`.
- Make every cache key the *minimum* tier set it actually depends on, so a
  scroll does not invalidate the highlight cache, a theme change does not
  invalidate the wrapped-row layout cache, etc.
- Make the invalidation *reason* explicit at every call site via a typed
  `InvalidationReason` enum; remove the reason-blind
  `InvalidateDerivedCaches(start_line)` signature.
- Preserve correctness across every existing editor scenario: no derived cache
  may return stale data for any mutation that previously bumped the single
  revision.
- Add per-tier perf counters so future regressions ("we're bumping
  `content_revision` from a scroll") are visible in the harness JSON.

**Non-Goals:**

- Touching `FoldingModel`'s independent `layout_revision`. It is already
  scoped to fold model state and not affected by this split.
- Adding a fifth tier for fold collapse. Fold collapse rewrites the visible
  row map — it belongs to `layout_shape_revision` and we will route it there.
- Public API or plugin-host changes. `TextViewport` is host-internal.
- Reworking the LRU eviction policy on `visible_line_cache_` /
  `highlight_cache_` — those stay byte-for-byte the same, only their key
  comparison changes.
- Refactoring `RenderTextWidthCache` / `RenderTextTextureCache` in
  `SdlTtfTextBackend`. Their existing keys (string + color + font) already
  hold across edits; the proposal mentions them only to note they intentionally
  stay out of the tier system.

## Decisions

### Decision 1: Four tiers, named by cause not by consumer

The tiers are named for the *mutation* that bumps them, not for the cache that
reads them. This means call sites pick the tier by asking "what did I just
change?", which is a stable mechanical decision, instead of "what caches
should this invalidate?", which couples writers to readers.

- `content_revision` — bumped when `DocumentState::lines` is mutated
  (insert / delete / paste / undo / redo / replace-buffer / formatter apply).
- `syntax_revision` — bumped when language identification, syntax-highlight
  contract, theme color set, or token classifier changes — i.e. when the same
  bytes would produce a different token color.
- `layout_shape_revision` — bumped when soft-wrap toggle, fold collapse / expand,
  tab size, visible column count, or font metrics change — i.e. when the same
  bytes produce a different visual row geometry.
- `presentation_revision` — bumped when decoration spans, overlay rendering,
  selection-only render-time hints, review-comment markers, or LSP-diagnostics
  overlays change — i.e. when no buffer mutation has occurred but the rendered
  pixels would still differ.

**Alternatives considered:**

- *Two tiers (content vs. presentation).* Too coarse: theme change still
  invalidates wrapped-row layouts, soft-wrap toggle still invalidates highlight
  cache. Doesn't unlock the main wins.
- *Per-cache revisions on the writer side ("highlight_revision",
  "wrapped_row_revision").* Writer must know which caches exist, which couples
  unrelated code. Adding a new cache later would mean editing every mutation
  site.

### Decision 2: Typed `InvalidationReason` enum, no overloads

`InvalidateDerivedCaches(start_line)` becomes
`InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line)`
with `enum class InvalidationReason { ContentEdit, SyntaxConfig, LayoutShape, Presentation }`.
Internally the implementation fans out into tier bumps and the existing
suffix-clearing logic.

A `ContentEdit` bump *also* bumps `presentation_revision` (any content change
moves decoration anchors implicitly). `LayoutShape` *also* bumps
`presentation_revision`. `SyntaxConfig` only bumps itself plus
`presentation_revision`. This implication graph stays inside the implementation;
callers always pass the single reason that *caused* the call.

**Alternatives considered:**

- *Free function `BumpContentRevision()`-style helpers.* Lets the call site
  decide which tier to touch directly, but separates the bump from the
  invalidation-suffix logic that today is tied to `start_line`. We want both
  in one call.
- *Bitmask of tiers (`InvalidationReason::ContentEdit | SyntaxConfig`).* Real
  mixed mutations don't happen in current call sites; if one ever does, we can
  call the function twice or extend the enum then.

### Decision 3: Cache keys hold the minimum tier set

Each cache stores exactly the revisions it depends on, and compares all of
them for equality. Concretely:

| Cache | Keys on |
| --- | --- |
| `wrapped_row_layouts_` | `layout_shape_revision` |
| `visible_line_cache_` | `content_revision`, `presentation_revision` |
| `highlight_cache_` | `content_revision`, `syntax_revision` |
| `line_highlight_states_`, `highlight_checkpoints_` | `content_revision`, `syntax_revision` |
| `cached_max_visual_columns_` | `content_revision`, `layout_shape_revision` |
| `bracket_match_cache_` (renderer) | `content_revision`, `layout_shape_revision` |
| `indent_guides_cache_` (renderer) | `content_revision`, `layout_shape_revision` |
| Per-line view-model key (`EditorViewRenderer.h:107/136/148`) | `content_revision`, `layout_shape_revision`, `presentation_revision` |

The cache-key struct gains a `RevisionSet` member that is a small POD of up to
four `std::uint64_t` fields plus an equality operator. Members that the cache
doesn't depend on stay zero in its `RevisionSet`.

**Alternatives considered:**

- *Single combined `std::uint64_t = hash(tiers)`.* Cheap to compare but loses
  the ability to assert per-tier counters and makes debugging harder.
- *Read all four tiers on every cache lookup unconditionally.* Same comparison
  cost but obscures the design — readers should be visibly typed by which
  tiers they depend on.

### Decision 4: Per-tier perf counters drive the scroll-only fixture

We add `editor.content_revision_bumps`, `editor.syntax_revision_bumps`,
`editor.layout_shape_revision_bumps`, `editor.presentation_revision_bumps` to
`util::PerfCounterId` and increment them inside the new
`InvalidateDerivedCaches`. The new
`editor_scroll_only_no_content_bump` perf scenario asserts
`content_revision_bumps == 0 && syntax_revision_bumps == 0 &&
layout_shape_revision_bumps == 0` over the measurement window — scrolling MAY
bump `presentation_revision` (selection caret moves, hover overlay updates) but
MUST NOT bump the others. If a future change re-introduces a cross-tier bump
on scroll, the scenario fails.

### Decision 5: One-shot migration, no compatibility shim

`DocumentState::layout_revision` is removed entirely; there is no
`std::size_t& layout_revision = content_revision` alias. The architectural-lint
test in `tests/ArchitectureInvariantsTests.cpp` gains a check that forbids
re-introducing a `layout_revision` member name on `DocumentState` or its alias.
Every call site is migrated in the same change.

`FoldingModel::Snapshot::layout_revision` (FoldingModel.h:33) is **not**
renamed — it's an independent concept (fold-model revision, not document
revision) and renaming it would broaden the change without benefit. The lint
guard is scoped to `TextViewport::DocumentState`.

## Risks / Trade-offs

- **Risk: caller passes the wrong `InvalidationReason`, silent staleness.**
  → Mitigation: every call site is reviewed and matched against the documented
  cause-to-tier table in this design doc. Unit tests in `TextViewportTests.cpp`
  exercise each mutation shape (insert, delete, paste, undo, redo,
  set-soft-wrap, set-tab-size, fold collapse, theme change, decoration update)
  and assert which tier(s) bumped. If a caller is wrong, the test catches it
  before merge.

- **Risk: a derived cache's `RevisionSet` omits a tier it actually depends on.**
  → Mitigation: dedicated tests that mutate via each `InvalidationReason` and
  then read from each cache, asserting the cache rebuilds when (and only when)
  the right tier bumps. The cache-key table in Decision 3 is the source of
  truth and the tests check every row.

- **Risk: harness baselines move for nearly every editor scenario, hiding a
  small regression in the noise.**
  → Mitigation: the change record includes the required `perf-baseline:` line
  and explicit before/after numbers. The scroll-only fixture and the per-tier
  counters give us granular evidence that the move is in the expected
  direction (fewer cross-tier bumps), not just "numbers got smaller."

- **Trade-off: four counters per `DocumentState` instead of one** — 24 bytes
  of struct growth per open document. Negligible.

- **Trade-off: every cache-key struct gets a `RevisionSet` member** — minor
  size increase for caches whose entries are already much larger than the key.
  No measurable cache footprint impact expected.

- **Trade-off: the typed reason enum is mildly verbose at call sites** — the
  payoff is that future readers can see exactly which mutation kind triggered
  the invalidation. Worth it.

## Migration Plan

Single-PR migration; no flag, no shim.

1. Land tiers + enum + new `InvalidateDerivedCaches` signature with all
   existing callers re-routed and all cache keys updated, in one commit.
2. Re-run the full harness on `perf-runner-v1` and capture the
   `perf-baseline:` deltas in the same PR.
3. If a regression sneaks in, revert the single commit — no partial state to
   recover from.

Rollback: revert the single commit. There is no on-disk format change, no
persisted-state migration, and no API used outside `src/editor/` and
`src/render/`.

## Open Questions

- **Q: Does the per-line view-model cache really need `presentation_revision`,
  or can we narrow it further?** Today the per-line view model embeds
  decoration spans, so a presentation-only change would invalidate it. If
  future work extracts decorations into a separate per-frame overlay layer,
  the per-line view model could drop `presentation_revision` from its key.
  Tracked as a follow-up; the proposal does not block on it.

- **Q: Should `font metrics change` count as `layout_shape` or its own tier?**
  Today font reload is rare and full-rebuild; rolling it into `layout_shape`
  is correct. If a future change makes font-size cycling a hot path, we can
  promote it then.
