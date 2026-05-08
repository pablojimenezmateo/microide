## Context

The shipped editor pipeline already gives us most of the moving parts we need:

- `editor::TextViewport` owns text storage, selection model, multi-caret state, undo history, soft wrap, and layout caches. Edit operations already publish a canonical applied edit (`editor-edit-delta-pipeline`), which lets new commands (move/duplicate/delete line, toggle comment, snippet expansion) participate in incremental redraw and incremental LSP sync without rebuilding the workspace's plumbing.
- `editor::SyntaxHighlighter` and `RuntimeSyntaxRegistry` produce per-line highlight states with stable definition IDs and coarse checkpoints. The same registry is the natural home for per-language metadata that today only lives implicitly in the syntax rules — bracket pairs, comment markers, indent hints, fold hints.
- `EditorViewRenderer` paints through the host-owned decorated text grid (`diff-merge-editor`'s shared row-decoration pipeline). New row decorations (active occurrences, indent guides, fold markers, sticky scroll header) belong here as ordered layers, not as ad hoc paint passes.
- `WorkspaceSidebarRegistry`, `WorkspaceSettingsRegistry`, `WorkspaceKeybindingRegistry`, `WorkspaceCommandRegistry`, and `PluginHost` already provide the contribution seams. New commands, settings, and a sidebar view all flow through these, so the new editor capabilities reach plugins through narrow host-owned APIs.
- `WorkspaceLspClient` / `WorkspaceLspManager` already expose async request infrastructure with SDL wake-event delivery; adding `documentSymbol` (and optionally `foldingRange`) is one new request kind, not a new transport.

The shell-architecture invariants from `2026-04-29` and `2026-05-06` constrain how this work lands:

- No `WorkspaceShell&` parameters in coordinator constructors; new coordinators consume service interfaces.
- Render TUs must not allocate strings or read project state directly; new view-model fields are populated by `RenderViewModelBuilder`.
- `lua_State*` stays inside `plugin/LuaRuntime`; the Lua surface for the new language contract is exposed through the existing `PluginHostLuaApi.inc` plumbing.
- `TextViewport` non-const paths must not snapshot-copy `document_->lines`; folding and shaping actions reuse the existing range-history primitives.
- All new render text materialization happens in `RenderViewModelBuilder`, never in render TUs.

## Goals / Non-Goals

**Goals:**

- Bring the editor up to "VSCode/Zed parity for typical editing" without dragging in either editor's full UI surface.
- Keep every new behavior deterministic, host-owned, and testable in isolation. Plugins contribute *data* (pairs, comments, snippets, fold hints, document symbols) and *providers* (LSP servers); the host owns the rendering, redraw, and policy.
- Reuse the existing TextViewport edit primitives, undo history, applied-edit publishing, multi-caret model, and view-model builder. No new top-level surface, no new persistence format, no new rendering backend.
- Hold typing and scroll latency at or below current budgets. Folding, occurrences scan, indent-guide compute, and sticky-scroll resolve all stay viewport-scoped and incremental.
- Land the work as one coherent slice so the seven new capability areas share consistent language metadata, settings, and command-action plumbing.

**Non-Goals:**

- Minimap (heavy renderer work; defer until profiling shows demand).
- Command palette redesign (`Ctrl+P` and the existing command prompt remain unchanged).
- Tree-sitter or any incremental parsing rewrite. Folding and indent intelligence use the existing regex-based syntax registry plus structural scans; if a future tree-sitter pass lands, it can replace the syntactic backend without changing the spec.
- AI-driven inline completion or inline suggestions. `non-ai-product-scope` still applies.
- Persistent fold state across sessions in the first cut. Fold state is per-tab session-only (cleared on reload) until user feedback shows persistence is worth the complexity.
- Snippet marketplace, snippet sharing, or LSP-server-driven snippet contributions in the first cut. Only plugin-declared snippets are wired; LSP-server snippets ride the existing completion path verbatim.
- Sticky scroll for compare and merge tabs in this slice. Scope is normal editor tabs; compare/merge can adopt the same primitive in a follow-up under `diff-merge-editor`.

## Decisions

### Decision: Folding model is a host-owned `FoldingModel` keyed off TextViewport revisions

A new `editor::FoldingModel` owned per editor tab computes fold ranges from two stacked sources, in priority order:

1. **Bracket pair ranges** when the active language declares a bracket set. Computed from a single linear scan that tracks bracket nesting, skipping over syntax-highlighted comment/string regions (we already have per-line `SyntaxState` so `IsCachedHighlightState` plus the highlight tokens give us "skip strings/comments" without re-tokenizing).
2. **Indent-block ranges** for any region not already covered by a bracket pair. Indent ranges open on a non-blank line whose indent is followed by a strictly-greater indent on the next non-blank line, and close at the next non-blank line whose indent returns to or below the opener.

Why two stacked sources rather than only one:

- Indent-only folding works for every language (Python, YAML, Markdown, Lua) without any per-language metadata, and is what users on Zed expect by default.
- Bracket-only folding is precise for `{`-heavy languages (C/C++, Java, JS/TS, Rust, Go) where indentation is conventional but not authoritative.
- Stacking lets a single language declaration ("brackets are `()`, `[]`, `{}`") give precise folds for nested function bodies *and* keep indent folds for top-of-file comment blocks.

Why a separate `FoldingModel` rather than data on `TextViewport`:

- `TextViewport` is the shared edit primitive; pinning fold state onto every TextViewport (including compare panes, merge panes, and read-only buffers) inflates the type for surfaces that don't fold.
- Per-tab `FoldingModel` lets us key recompute on `(layout_revision, tab_size, language_id)` and keep the model pure / serializable for tests.

Recompute strategy:

- Fold ranges are recomputed lazily, gated by `layout_revision`. The model exposes `EnsureFoldsForVisibleRange(visible_start, visible_end)` so the render path only forces compute for the rows that actually paint.
- Edits invalidate fold state by setting a dirty flag; the next fold query rebuilds incrementally from the affected line down to the next outermost closer that the edit could have touched (bracket-balance is recomputed from the previous balanced line, indent ranges from the affected outer block).
- An explicit budget in `FoldingModel::ComputeWithBudget(max_lines)` caps per-frame work. If the budget is exceeded the model returns "partial" and the renderer paints the resolved ranges only; the rest is finished in idle time. This keeps the fold compute off the typing latency path on very large files.

**Alternatives considered:**

- *Run folding on every keystroke synchronously.* Simple but O(N) per keystroke on large files; rejected because typing latency is a top-3 priority.
- *Run folding asynchronously on a background worker.* Adds the same ordering hazards we just resolved in the LSP and search paths and wakes us up to redraw out-of-band. Rejected for v1; the lazy + viewport-bounded compute is enough.
- *Fold by syntax tree only.* Requires tree-sitter or a parser per language. Out of scope.

### Decision: Sticky scroll is a derived view-model field, not a separate render layer

`RenderViewModelBuilder` computes the *enclosing fold-stack* (top-most enclosing fold range whose opener is above `scroll_line`) and emits up to N (default 3) "sticky" line indices into the editor view model. The render path paints those lines as a fixed band above the gutter/text grid using the existing decorated text-grid primitive — no new render surface, no new caret model.

Why a view-model field:

- The architectural invariant is that render TUs do not read project state. Computing the sticky stack in the builder keeps `WorkspaceShellRenderFrame` free of fold knowledge.
- The sticky band is a paint of *existing* lines, so it reuses the same syntax run, indent guide, and selection layers.

**Alternatives considered:**

- *Separate "sticky scroll widget" surface.* Doubles the layout math and makes scrolling animation harder. Rejected.

### Decision: Bracket / comment / pair / fold contract is one host service, language-keyed

A new `workspace::WorkspaceLanguageContract` service owns:

```
struct LanguageContract {
  std::string language_id;
  std::vector<std::pair<std::string, std::string>> bracket_pairs;   // ("(", ")") etc.
  std::vector<AutoClosePair> auto_close_pairs;                       // open, close, scope hints
  std::vector<SurroundPair> surround_pairs;
  std::optional<std::string> line_comment;                           // "//"
  std::optional<std::pair<std::string, std::string>> block_comment;  // ("/*", "*/")
  std::vector<std::string> indent_after_open_patterns;               // regex hints
  std::vector<std::string> dedent_on_close_patterns;
  std::vector<FoldMarker> fold_markers;                              // optional explicit markers
  std::vector<SnippetSpec> snippets;
};
```

It is built from three sources, merged in priority (later overrides earlier):

1. A built-in default table covering the languages currently shipped in `RuntimeSyntaxGenerated.cpp` (C/C++, JS/TS, Python, Lua, Rust, Go, JSON, YAML, Markdown, Shell).
2. Plugin contributions through the existing `PluginHost` registry, exposed via four new Lua tables that mirror existing contribution-table conventions (`ctx.brackets`, `ctx.comments`, `ctx.indents`, `ctx.snippets`). Existing `ctx.syntax` gains optional sibling fields so a plugin that declares syntax can declare pairs/comments alongside it.
3. User-level overrides through `WorkspaceSettingsRegistry` keys (e.g. `editor.brackets.add`, `editor.snippets.disabled`).

Resolution is deterministic: language id is taken from the syntax registry's `DetectFiletype`. If no contract matches, defaults are: bracket pairs `()`, `[]`, `{}`; auto-close pairs the same plus `"`/`"`, `'`/`'`, ` ` ` /` ` `; surround pairs the same plus `<`/`>`; line comment unset (no toggle-line-comment available); block comment unset; indent hints empty (smart indent falls back to "preserve previous indent" — current behavior).

Why one service instead of one per concern:

- All three concerns (auto-pair, smart indent, folding) consume the bracket set. Splitting them forces every plugin to redeclare the same data.
- Snippets share the same language-id keying and the same lifecycle (rebuilt on plugin reload).

**Alternatives considered:**

- *Read directly from the syntax registry's existing `symbol.brackets` regex tokens.* Tempting because the data is already there, but `[(){}]` regexes lose the open/close pairing — we'd need a parser pass to recover it. Rejected.

### Decision: Auto-close, surround, smart indent, and snippet expansion run inside `TextViewport` as typed-input transformations

Pair/surround behavior is a transformation on the `InsertText` / `InsertCharacter` / `InsertNewline` path:

- On `InsertCharacter('(')` with no selection: insert `()` and place caret between them. If the next character is already `)` and the typed character is the close `)`, skip-over instead of inserting (the "type close to advance through close" idiom).
- On `InsertCharacter('(')` with a non-empty selection: replace selection with `(<selection>)` and keep the selection. Same shape for every surround pair.
- On `InsertNewline` with caret between an open/close pair: insert `\n<indent+1>\n<indent>` and place caret on the middle line at `indent+1` (the "split braces" idiom).
- Smart indent on `InsertNewline` consults `WorkspaceLanguageContract::indent_after_open_patterns` against the line's text up to the caret and adds one indent unit if matched; consults `dedent_on_close_patterns` against the typed character (e.g. `}`) on an indent-only line and subtracts one indent unit before inserting.

Why inside `TextViewport`:

- All four behaviors are conceptually a single applied edit; running them outside the viewport would publish two applied edits and break `editor-edit-delta-pipeline`.
- The viewport already owns the multi-caret loop; pair/surround/smart-indent must apply at every caret. Lifting that loop to the workspace would either duplicate it or skip secondary carets.
- Undo/redo composition is automatic: the existing range-history entry already captures the whole transformation as one step.

The `TextViewport` constructor gains a `LanguageContractView` parameter (a non-owning, read-only view of the relevant pairs/indent rules). The active editor tab refreshes this view when the language id or contract revision changes; the viewport never owns or queries the workspace contract.

Snippet expansion uses a new `editor::SnippetEngine` that lives next to `TextViewport`. On expansion the engine inserts the snippet body through `TextViewport::ReplaceRange`, then registers an opaque `SnippetSession` on the tab with the placeholder positions. `Tab` / `Shift+Tab` while a session is active drives placeholder navigation; any caret motion that leaves the active placeholder cleanly cancels the session. Sessions are per-tab and per-undo-stack, so a single undo unwinds the entire snippet expansion.

**Alternatives considered:**

- *Run pair/surround in a workspace-level coordinator.* Forces duplicating the multi-caret loop and re-publishing applied edits. Rejected.
- *Implement snippets entirely in plugin-land.* Plugins lack access to the canonical undo and applied-edit pipeline; rejected for the same reason completion overlays stayed host-owned.

### Decision: Code shaping actions are new `ActionId` entries dispatched through the existing action coordinator

Move-line, duplicate, delete-line, indent/outdent, toggle-comment, sort-lines, add-cursor-at-next-match, add-cursor-at-all-matches-in-selection are new `ActionId` enum entries with corresponding `WorkspaceCommandRegistry` and `WorkspaceMenuRegistry` entries. Each maps to a small free function in a new `editor::ShapingActions` translation unit that operates on a `TextViewport&` (the existing applied-edit primitives suffice).

Why action ids rather than ad hoc key-handling:

- Plugins can trigger them through the `commands` table without learning a parallel API.
- The existing `WorkspaceKeybindingRegistry` mechanism handles user override / disable.
- The architectural-lint rules already cover this seam.

Highlight-occurrences is a *render-time* derived layer rather than an action: the view model exposes `occurrence_ranges` for the current word under the primary caret (computed by a viewport-bounded scan in `RenderViewModelBuilder`); the renderer paints them as a low-contrast underlay. Add-cursor-at-next-match is the *action* that promotes the next occurrence to a secondary caret.

### Decision: Symbol outline is a sidebar view backed by a new `OutlineProvider` indirection

A new `WorkspaceOutlineService` polls the active editor tab's `(language_id, layout_revision)` and asks for symbols from, in order:

1. The active LSP server for that language id, via `textDocument/documentSymbol`.
2. A deterministic regex-based fallback per language, defined in the language contract (e.g. `^\s*(class|fn|def|function)\s+([A-Za-z_][A-Za-z0-9_]*)`).

The result is a tree of `(name, kind, range, selection_range, children)`. Outline state is per-tab and updated on a debounce after edits (default 150 ms — same hysteresis as the existing diagnostics refresh).

The outline view registers in `WorkspaceSidebarRegistry` with id `outline`, reusing the registry's policy / persistence path. Clicking an outline node moves the active caret to the symbol's `selection_range` and scrolls the viewport.

**Alternatives considered:**

- *Build outline from syntax tokens.* The token stream is regex-based and has no notion of "name", so we'd be reinventing a small parser anyway. The regex-fallback table covers the same languages with less code and stays scoped to the outline view.

### Decision: Save-time normalization is three independent settings on the existing save pipeline

`WorkspaceSettingsRegistry` gains:

- `editor.save.trim_trailing_whitespace`: bool, default true.
- `editor.save.ensure_final_newline`: bool, default true.
- `editor.indent.detect_on_open`: bool, default true.

The save participant pipeline (already exists for formatter execution) gains a built-in pre-formatter normalization step. Auto-detect runs inside `TextViewport::OpenFile` after `DecodeLines`, scanning at most the first 256 non-blank lines for `^( +|\t)` patterns and choosing tabs vs spaces of width N (with a tie-break that prefers the existing project setting). The detection result is applied to `tab_size` and `soft_tabs` for that tab only and is *never* persisted (re-detected on each open) so projects with mixed indent histories don't get a stale stuck setting.

### Decision: Every new capability is user-disable-able through setting + command + menu

Every capability listed in this change SHALL be reachable from three independent enable/disable surfaces, in addition to whatever tuning knobs it exposes:

1. **Setting**: a `WorkspaceSettingsRegistry` entry of the shape `editor.<area>.enabled` (boolean, default documented per capability) that round-trips through user-level and project-level persistence, with project-level overriding user-level.
2. **Command**: a stable `ActionId` toggle entry registered in `WorkspaceCommandRegistry` (e.g. `ActionId::ToggleEditorFolding`, `ActionId::ToggleEditorAutoClosePairs`) so the toggle is dispatchable from the command prompt and bindable through `WorkspaceKeybindingRegistry`.
3. **Menu**: a `checkable` `MenuItem` registered in `WorkspaceMenuRegistry` under the appropriate menu (`View` for presentation toggles, `Selection` for multi-cursor toggles, `Preferences → Editor` for input/save normalization toggles, plus a one-line summary in the `Settings` overlay's "Editor → Essentials" group) whose checked state is bound to the setting's effective value.

When a capability is disabled:

- Its setting is `false`.
- Its command `ActionId` is still enabled (so the user can flip it back on); when invoked, it flips the setting and emits the redraw event for the affected tab.
- Its menu entry renders as unchecked.
- The editor reverts to the prior built-in behavior with no residual UI artefact: the gutter mark / indent guide / sticky band / occurrence underlay / snippet overlay / outline view either disappears or never paints.
- Plugin-contributed data for the capability (e.g. snippets, bracket pairs) remains registered but is unused; re-enabling the capability picks the data back up without a plugin reload.

The Settings overlay gains a single "Editor → Essentials" section that lists every new toggle in one column, so a user can audit and flip the entire set in one place. Each toggle in that section also exposes a "Reset to default" affordance that reverts the user-level value (and, when invoked from a project tab, the project-level override) back to the shipped default.

Default values are chosen to match VSCode/Zed defaults where they agree:

| Capability | Setting key | Default |
| --- | --- | --- |
| Code folding (gutter, motion, sticky scroll) | `editor.fold.enabled` | true |
| Sticky scroll band | `editor.fold.sticky_scroll.enabled` | true |
| Indent guides | `editor.view.indent_guides.enabled` | true |
| Render whitespace | `editor.view.render_whitespace` | false |
| Symbol outline sidebar view | `editor.outline.enabled` | true |
| Bracket match highlight + jump | `editor.brackets.match_highlight.enabled` | true |
| Auto-close pairs | `editor.brackets.auto_close.enabled` | true |
| Surround selection with pair | `editor.brackets.surround.enabled` | true |
| Smart indent on Enter / dedent on close | `editor.indent.smart.enabled` | true |
| Toggle line/block comment | `editor.shaping.toggle_comment.enabled` | true |
| Move/duplicate/delete line | `editor.shaping.line_ops.enabled` | true |
| Sort lines | `editor.shaping.sort_lines.enabled` | true |
| Add cursor at next/all match | `editor.multicursor.add_at_match.enabled` | true |
| Highlight occurrences of word under caret | `editor.occurrences.enabled` | true |
| Snippet expansion | `editor.snippets.enabled` | true |
| Trim trailing whitespace on save | `editor.save.trim_trailing_whitespace` | true |
| Ensure final newline on save | `editor.save.ensure_final_newline` | true |
| Auto-detect indent on open | `editor.indent.detect_on_open` | true |

Project-level keys mirror the user-level keys so a project can opt out (or in) without changing the user's global default.

**Alternatives considered:**

- *Single global "Editor essentials" master toggle.* Too coarse — users that want auto-close pairs but not sticky scroll would have to disable everything. Rejected.
- *Hide tuning knobs and only expose the master toggle.* Forces every preference into a binary, conflicting with the existing pattern (`editor.tab_size`, `editor.indent_width`, etc.). Rejected.
- *Bind toggles only to settings, not to commands/menus.* Inconsistent with the existing `Wrap`, `ToggleStatusBar`, `ToggleLayoutMode` pattern and harder to discover. Rejected.

### Decision: Per-hot-path performance budgets, caching strategy, and isolated harness scenarios

Every new behavior in this change runs on at least one of the editor hot paths (typing, caret motion, scroll, frame build, save, file open). The aggregate `typing_large_file` and `scroll_large_file` baselines already gate regressions on the combined cost, but a regression in any one of the new paths can be masked by improvements elsewhere. To keep each path measurable, this change SHALL add isolated harness scenarios alongside the existing aggregate ones, plus an explicit caching strategy for the per-frame paths.

**Per-hot-path budget table** (reference Linux host, `perf-runner-v1`; budgets are advisory targets, harness-enforced through committed JSON baselines under `tests/perf/baselines/`):

| Hot path | Trigger | Target budget | Isolated scenario |
| --- | --- | --- | --- |
| Bracket match scan (caret-adjacent) | every caret motion | ≤ 50µs P95 on 50k-line file | `editor_bracket_match_caret_motion` |
| Fold model full recompute | edit invalidates broad range | ≤ 4ms P95 with `ComputeWithBudget(2000)` partial fallback on 50k-line file | `editor_fold_recompute` |
| Fold viewport-bounded refresh | scroll, layout revision | ≤ 200µs P95 on 50k-line file | `editor_fold_viewport_refresh` |
| Sticky scroll resolution | scroll, fold state change | ≤ 30µs P95 on 50k-line file with depth ≤ 3 | `editor_sticky_scroll_scroll` |
| Indent guides compute | every frame, visible rows | ≤ 100µs P95 on 50k-line file with `indent_width = 4` | `editor_indent_guides_paint` |
| Render whitespace overlay | every frame when enabled | ≤ 150µs P95 on 50k-line file at typical viewport | `editor_render_whitespace_paint` |
| Occurrence scan (word under caret) | caret moves to new word | ≤ 100µs P95 viewport-bounded on 50k-line file with common word | `editor_occurrences_scan` |
| Word-under-caret detection | caret motion | ≤ 5µs P95 (cached per layout revision) | covered by `editor_occurrences_scan` |
| Smart indent + dedent-on-close | every Enter / close-paren keystroke | within `typing_large_file` budget | `editor_smart_indent_typing` |
| Auto-close pair insert | every paired-open keystroke | within `typing_large_file` budget | `editor_auto_close_typing` |
| Surround selection | open-pair keystroke with selection | ≤ 200µs P95 with 8 carets, 80-char selections each | `editor_surround_multi_caret` |
| Add cursor at next match | command invocation | ≤ 200µs P95 on 50k-line file with 10k matches | `editor_add_cursor_next_match` |
| Toggle line comment (multi-line) | command invocation | ≤ 1ms P95 on 1000-line selection | `editor_toggle_comment_large_selection` |
| Move/duplicate/delete line (multi-caret) | command invocation | ≤ 500µs P95 with 32 carets on 50k-line file | `editor_shaping_multi_caret` |
| Sort lines | command invocation | ≤ 5ms P95 on 10000-line selection | `editor_sort_lines_large` |
| Snippet expansion (initial) | trigger acceptance | ≤ 300µs P95 for 20-placeholder body | `editor_snippet_expand` |
| Snippet linked-placeholder edit | typing inside placeholder | ≤ 200µs P95 with 10 linked occurrences | `editor_snippet_placeholder_edit` |
| Outline LSP refresh | debounced 150ms after edits | no perturbation of `typing_large_file` budget while debouncing | `editor_outline_lsp_refresh` |
| Outline regex fallback | tab activation, no LSP | ≤ 5ms P95 on 50k-line file with conventional regex hints | `editor_outline_regex_fallback` |
| Save trim + ensure-newline normalization | every save with toggles on | ≤ 5ms P95 on 1MB buffer | `editor_save_normalization` |
| Auto-detect indent on open | every file open with toggle on | ≤ 2ms P95 on 1MB file (capped at 256 non-blank lines) | `editor_indent_detect_open` |
| Aggregate decorated-grid layers | every frame | within existing `scroll_large_file` and `typing_large_file` budgets | `scroll_large_file`, `typing_large_file` (existing) |

The aggregate `typing_large_file` and `scroll_large_file` baselines remain the merge gate; the isolated scenarios above are added so that a regression in one capability does not get washed out by an improvement in another, and so each capability's owner can profile and iterate against its own baseline.

**Caching strategy.** Every per-frame hot path is cache-keyed on the smallest invariant set that makes it correct, and the cache is bounded so memory does not balloon on long-running sessions:

- **Bracket match pair**: cached per `(viewport_pointer, layout_revision, primary_caret_line, primary_caret_column)`. Cache size 1 (only the current pair); invalidates trivially on any caret move or edit.
- **Fold ranges**: cached per `(layout_revision, tab_size, language_id)`; computed lazily by `EnsureFoldsForVisibleRange` so first-frame work is bounded by visible row count plus a documented look-ahead. On edit, the model marks dirty and recomputes incrementally from the affected line down to the next outermost balanced closer (bracket source) or the first line whose indent ≤ opener indent (indent source). `ComputeWithBudget(max_lines)` returns partial when work exceeds the budget; the renderer paints resolved ranges and finishes the rest in idle.
- **Sticky scroll stack**: cached per `(scroll_line, fold_model_revision)`; cache size 1.
- **Indent guides**: cached per `(layout_revision, indent_width, visible_row_start, visible_row_end)`; cache size 1, replaced on viewport change.
- **Occurrence scan**: cached per `(word_under_caret, layout_revision, visible_row_start, visible_row_end)`; cache size 1, recomputed when the caret moves to a different word or the viewport scrolls.
- **Word-under-caret resolution**: cached per `(layout_revision, primary_caret_line, primary_caret_column)`; cache size 1.
- **Outline tree (LSP path)**: cached per tab; refreshes on debounced 150ms timer after the last edit; LSP request is cancelled and reissued if a newer edit arrives before the response.
- **Outline tree (regex fallback)**: cached per `(layout_revision, language_id)` per tab; recomputed only when the layout revision changes.

All caches live next to the data they cache (per `TextViewport`, per `FoldingModel`, per editor tab) so they are deallocated together when the tab closes. No new global cache is introduced.

**Allocation discipline.** Each new render-layer scan path SHALL avoid allocating per-frame:

- Fold gutter marks, indent guide runs, sticky line entries, occurrence ranges, whitespace glyph runs, and bracket-match pair are all populated into pre-sized `std::vector<>` fields on the editor view model that are `clear()`-ed (preserving capacity) every frame, not freshly constructed.
- `RenderViewModelBuilder` reuses a single thread-local scratch buffer for the per-line scans (bracket walk, indent walk, occurrence walk, whitespace walk).
- Snippet placeholder positions are stored as `SelectionRange` values, not strings.
- Auto-close, surround, smart indent, and dedent-on-close paths inside `TextViewport` SHALL produce exactly one applied edit per typed event so the existing applied-edit pipeline already amortizes the allocation cost.

The architectural-lint rule `CheckRenderTusDoNotAllocateStrings` is extended to cover the new render-layer code in §14.1, so accidental string materialization in render TUs hard-fails the build.

**Harness budget enforcement and baseline updates.** Every new isolated scenario commits a JSON baseline under `tests/perf/baselines/` with `tolerances` matching the existing convention (`max_percent: 50`, `p95_percent: 20`, `p50_percent: 10`). Capturing the baseline is part of §13.5, and any future change that touches one of the listed hot paths SHALL include a green run for the scenario or update the baseline with a `perf-baseline:` justification per `performance-budgets/spec.md`.

**Alternatives considered:**

- *Rely solely on the existing aggregate scenarios.* Under-resolves: a regression in one capability can be masked by an improvement in another. Rejected.
- *Make every isolated scenario blocking.* Doubles the harness runtime; rejected. The aggregate scenarios remain the merge gate; the isolated ones provide diagnostic resolution and ownership tracking.
- *Inline the new layers' work into existing render paths without a view-model-field separation.* Conflicts with the architectural invariant that render TUs do not compute product logic; rejected.

### Decision: Render layers, view-model fields, and view-model builder additions

`EditorViewModel` gains, in order of paint:

- `fold_gutter_marks: vector<FoldGutterMark>` (caret pos, glyph state).
- `indent_guide_columns: vector<IndentGuideRun>` (column, start row, end row, active flag).
- `whitespace_glyph_runs: vector<WhitespaceGlyphRun>` (only when render-whitespace is on).
- `occurrence_ranges: vector<SelectionRange>` (host-computed, viewport-bounded).
- `sticky_lines: vector<StickyLine>` (resolved enclosing fold stack, capped at 3).
- `bracket_match_pair: optional<BracketMatchPair>` (positions of the bracket adjacent to the caret and its match).
- `outline_breadcrumb_segments: vector<OutlineSegment>` (existing breadcrumb gains a "→ symbol path" tail when outline is available).
- `snippet_session: optional<SnippetSessionView>` (active placeholder index, all placeholder ranges).

Each is computed once per frame in `RenderViewModelBuilder` and consumed by the existing render TUs. No render TU gains new product logic; all new paint is added inside `EditorViewRenderer::Render` as ordered layers under the existing decorated text-grid pipeline.

## Risks / Trade-offs

- **[Risk] Folding compute on very large files exceeds typing budget** -> **Mitigation:** lazy / viewport-bounded compute, `ComputeWithBudget`, partial-result fallback, and a perf scenario `editor_fold_recompute` that asserts the typing/scroll budget on a 50k-line fixture.
- **[Risk] Auto-pair / smart indent fights the user's muscle memory** -> **Mitigation:** every pair behavior has a per-language and global setting; defaults are conservative (no auto-close inside string/comment regions, no smart indent if the line already has trailing non-whitespace after the close).
- **[Risk] Symbol outline drift between LSP and fallback regex** -> **Mitigation:** outline view shows a small "indexing" marker until the first LSP `documentSymbol` arrives, then swaps in. Cache last-good outline per tab so toggling tabs is instant.
- **[Risk] Snippet placeholder navigation interacts badly with multi-caret** -> **Mitigation:** snippet sessions disable secondary carets while active and restore them on session exit. The session is a single undo unit so escape via undo is always consistent.
- **[Risk] Adding 7 capability areas in one change is too much code in one slice** -> **Mitigation:** the work is staged in `tasks.md` so each capability lands as its own commit; the contract in `WorkspaceLanguageContract` and the new `ActionId` enums are the only cross-cutting bits and can land first as Step 1.
- **[Trade-off] Fold state is per-session and not persisted** -> intentional v1 cut; revisit once we measure actual user demand and have a persisted-record schema for it.
- **[Trade-off] Snippets are plugin-declared only (no LSP-server snippets in v1)** -> intentional v1 cut; LSP-server snippet bodies (`InsertTextFormat=2`) are honored verbatim through the existing completion overlay path, but no expansion / placeholder navigation. Full LSP snippet support follows once the host-owned engine is proven.
- **[Trade-off] Tree-sitter is not introduced** -> intentional v1 cut; the regex syntax + structural scans cover the new capabilities and keep the build dependency surface narrow. A future change can swap the syntactic backend without breaking the spec.

## Migration Plan

1. Land `WorkspaceLanguageContract` service, plugin Lua surface (`ctx.brackets`, `ctx.comments`, `ctx.indents`, `ctx.snippets`), built-in defaults, and the `LanguageContractView` plumbing into `TextViewport`. No user-visible behavior change yet — the contract is defined but unused.
2. Land bracket-match highlight + jump-to-matching-bracket. Smallest user-visible step that exercises the contract.
3. Land auto-close pairs + surround. Behind a setting `editor.brackets.auto_close` (default true) so users can opt out without redeploying.
4. Land smart indent on `InsertNewline` and dedent-on-close-pair. Behind `editor.indent.smart` (default true).
5. Land code shaping actions (toggle comment, move/duplicate/delete line, indent/outdent, sort lines, add cursor at next match, add cursor at all matches in selection, highlight occurrences). Each is independent and lands as one commit per group.
6. Land `FoldingModel`, fold gutter, fold-aware caret motion, sticky scroll. Behind `editor.fold.enabled` (default true).
7. Land indent guides, render whitespace, scroll-past-end. Each behind its own `editor.view.*` toggle.
8. Land save-time normalization (trim trailing whitespace, ensure final newline, auto-detect indent on open). Defaults true; users can disable per-project.
9. Land outline sidebar view + LSP `textDocument/documentSymbol` request + regex fallback per language.
10. Land snippet engine + plugin snippet contributions + `Insert Snippet…` overlay. Honor LSP snippet bodies through the existing completion overlay (no placeholder navigation for LSP snippets in v1).
11. Update `docs/active-work.md`, `guidelines/plugins.md`, `guidelines/ui-shell.md`, and add `docs/editor-essentials.md`.
12. Run sanitizer presets (ASAN/UBSAN/TSAN) and the perf harness scenarios; commit before/after numbers in the change record per `performance-budgets`.

Rollback strategy: every new capability sits behind a setting whose default lives in `WorkspaceSettingsRegistry`. Setting the flag to `false` disables the feature without touching code paths in other capabilities. If a regression is found in one area, that one setting flips off while the rest remains shipped.

## Open Questions

- Should the outline view default to enabled in the sidebar registry, or stay opt-in until the regex fallback is proven across the shipped language set? *Lean: opt-in, surface via `View → Outline` and `Ctrl+Shift+O`.*
- Should fold state survive tab close/reopen within the same session? *Lean: yes for "explicit user folds", no for "default-collapsed-by-language" — i.e., persist user toggles, not algorithm output. Defer until a user reports it matters.*
- Do we want a "highlight occurrences" mode that scans the whole document instead of viewport-only? *Lean: viewport-only in v1; whole-document is bounded only by document size and conflicts with our typing-latency budget.*
- Do we want LSP `textDocument/foldingRange` in v1? *Lean: optional. The bracket+indent fallback already gives correct folds; foldingRange is only a strict win for languages where the LSP knows about region markers (`#region`/`#endregion`). Add behind a per-language setting later.*
- Should snippet expansion be triggered through completion entries in addition to the explicit `Insert Snippet…` overlay? *Lean: yes — register snippets as completion items with a special kind, accept-completion runs the snippet engine instead of literal-insert.*
