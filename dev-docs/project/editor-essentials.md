# Editor essentials

This document describes the editor capabilities introduced under the
`editor-essential-capabilities` change: language contract, folding, presentation
layers, pair and indent behavior, shaping commands, save-time normalization,
and related settings. It reflects the code as implemented; items called out as
**partial** or **not implemented** match open tasks in
`openspec/changes/editor-essential-capabilities/tasks.md`.

Primary implementation references:

- Settings: `src/workspace/WorkspaceSettingsRegistry.cpp`
- Toggles and commands: `src/workspace/WorkspaceActionServices.cpp`,
  `WorkspaceCommandRegistry.cpp`, `WorkspaceMenuRegistry.cpp`,
  `WorkspaceKeybindingRegistry.cpp`
- Language merge: `src/workspace/WorkspaceLanguageContract.{h,cpp}`
- Editor render: `src/editor/EditorViewRenderer.cpp`,
  `src/workspace/RenderViewModelBuilder.cpp`

## Language contract

`WorkspaceLanguageContract` builds a per-`language_id` table used for auto-close,
surround, smart indent, comment metadata in the contract, snippet *definitions*,
and regex outline hints stored on the contract. Resolution merges, in order:

1. Host built-in defaults (`WorkspaceLanguageContract` internal table).
2. Plugin contributions via Lua `ctx.brackets`, `ctx.comments`, `ctx.indents`,
   and `ctx.snippets` (see `guidelines/plugins.md`).
3. User- and project-scoped string overrides (project wins on conflict):

| Setting | Effect |
| --- | --- |
| `editor.brackets.user_pairs` | Comma-separated `open\|close` tokens appended as extra bracket pairs globally. |
| `editor.brackets.user_disabled` | Comma-separated `open\|close` pairs removed from the resolved set. |
| `editor.comments.user_line` | If non-empty, replaces the line-comment marker for every language. |
| `editor.indents.user_open_patterns` | Comma-separated suffix tokens appended to indent-after-open lists. |
| `editor.snippets.user_disabled` | Comma-separated snippet ids filtered out of the merged list. |

`WorkspaceShell` pushes a non-owning `LanguageContractView` into each
`TextViewport` when language or contract revision changes (see
`WorkspaceShellEditor.cpp`).

## Settings overlay and scopes

Editor essentials toggles appear under **Settings** in the group path
**Editor → Essentials → …** (subsections: Block Structure, Pair And Indent,
Shaping And Save), with per-row reset where the overlay supports it.

Boolean settings use the usual string truthy/falsey parsing (`true` / `false`, etc.).
Project-scoped keys override user-scoped keys for the same id.

## Block structure

### Code folding

- **Settings:** `editor.fold.enabled` (default on, project-scoped).
- **Behavior:** Bracket- and indent-derived ranges via `editor::FoldingModel`;
  collapsed regions hide body rows in the viewport’s visible-row layout; vertical
  motion, paging, hit-testing, and soft-wrap layout are fold-aware.
- **Disabled:** Folding model is cleared/expanding; no fold gutter marks are
  passed into the editor renderer frame (`WorkspaceShellRenderFrame.cpp`).
- **Commands:** `Fold`, `Unfold`, `ToggleFoldAtCursor`, `FoldAll`, `UnfoldAll`
  (menu: **Edit**; default keys: `Ctrl+Shift+[`, `Ctrl+Shift+]`, chords
  `Ctrl+K Ctrl+0` / `Ctrl+K Ctrl+J` for fold-all / unfold-all via key coordinator).
- **Gutter:** `RenderViewModelBuilder::BuildEditorViewModel` emits
  `FoldGutterMark` entries; `EditorViewRenderer` draws a chevron-style control.
  The renderer may also draw a small square opener indicator when a
  `FoldingModel` is present (second affordance on opener lines).
- **Buffer search:** Collapsed folds enclosing the active match are expanded
  temporarily with restore logic when the overlay dismisses (see workspace
  buffer-search tests referenced from `tasks.md` task 5.8).

### Sticky scroll (partial)

- **Settings:** `editor.fold.sticky_scroll.enabled`,
  `editor.fold.sticky_scroll.max_depth` (int 1..8, default 3).
- **Commands / menu:** `ToggleEditorStickyScroll` under **View** (persists
  `editor.fold.sticky_scroll.enabled`).
- **Status:** No sticky band is computed or painted yet (`tasks.md` task 6 remains
  open). Toggles and settings round-trip; behavior is not user-visible beyond
  the setting.

### Symbol outline (partial)

- **Settings:** `editor.outline.enabled` (default on).
- **Commands / menu:** `ToggleEditorOutline` under **View**.
- **Status:** Outline sidebar / `documentSymbol` / regex fallback from the
  contract are **not** wired in the workspace sidebar registry yet (`tasks.md`
  task 11). The toggle and setting exist for forward compatibility.

### Indent guides

- **Settings:** `editor.view.indent_guides.enabled` (default on).
- **Rendering:** Vertical 1px fills at indent columns for visible rows;
  “active” guide uses `theme.text_muted` vs `theme.border`.
- **Computation:** `IndentGuides::ComputeIndentGuides` with cache on
  `EditorViewRenderer` keyed by layout revision, scroll, visible row count,
  indent width, caret line, and **fold revision** (cache invalidates on fold
  changes even though emphasis still derives from the caret line’s **leading
  visual indent** scan, not from fold nesting — fold-aware “enclosing block”
  emphasis is still pending per `tasks.md` task 7.3).

### Render whitespace

- **Settings:** `editor.view.render_whitespace` (default off).
- **Rendering:** Per-row inline fills (space dots, tab rules) using
  `theme.text_disabled`, emitted in the same decorated-row pass as other
  overlays. There is no separate `whitespace_glyph_runs` vector on
  `EditorViewModel` today (`tasks.md` task 7.4 — implementation uses inline fills).

## Pair and indent

### Bracket match highlight and jump

- **Settings:** `editor.brackets.match_highlight.enabled` (default on).
- **Rendering:** Single-cell fills for opener/closer using
  `theme.bracket_match_background`, layered in the per-row decoration list.
  Cached on `EditorViewRenderer` when caret/layout unchanged.
- **Jump:** `JumpToMatchingBracket` — `Ctrl+Shift+\` in the editor
  (`WorkspaceKeybindingRegistry.cpp`).
- **Scanner:** `FindBracketMatch` passes the live `TextViewport` into the line
  scan; each character consults `HighlightedLineTokens` (same pipeline as
  `SyntaxState` / folding) and **ignores** brackets in `String` or `Comment`
  token columns. `FindBracketMatchInLines` accepts an optional syntax viewport
  pointer for tests. Contract-defined bracket pairs still do not change which
  characters count as `{ } ( ) [ ]` for this matcher.

### Auto-close and surround

- **Settings:** `editor.brackets.auto_close.enabled`,
  `editor.brackets.surround.enabled` (project-scoped defaults on).
- **Behavior:** `TextViewport::InsertCharacter` / `InsertText` consult
  `LanguageContractView` (including inhibit-in-string / inhibit-in-comment
  flags), skip-over-close, split-braces on newline between pairs, etc.
- **Surround:** Single- and multi-line selections are supported for the primary caret
  and for every secondary caret that carries a per-caret selection range via
  `AddSecondaryCaretWithRange`.

### Smart indent and dedent-on-close

- **Settings:** `editor.indent.smart.enabled` (project-scoped, default on).
- **Behavior:** After newline, extra indent when the previous line matches an
  indent-after-open pattern; dedent-on-close removes one indent unit on
  indent-only lines when typing a configured close character. Disabled →
  legacy leading-whitespace behavior only.

### Auto-detect indent on open

- **Settings:** `editor.indent.detect_on_open` (user-scoped, default on).
- **Behavior:** After preferences apply on disk-backed loads,
  `ApplyDetectedIndentAfterPreferences` runs `editor::DetectIndent` on the
  buffer (up to **256 non-blank lines** per `IndentDetect.h`) and sets only
  `TextViewport` tab/indent fields — **no** buffer mutation, **no** persisted
  detection result.

## Occurrences highlight

- **Settings:** `editor.occurrences.enabled` (default on),
  `editor.search.case_sensitive` (default off) for seed word and add-cursor scans.
- **Data:** `RenderViewModelBuilder::BuildEditorViewModel` fills
  `EditorViewModel::occurrence_ranges` for the visible viewport (cached per
  viewport + layout + caret + case mode + word seed).
- **Paint:** Row background fills: seed occurrence uses active search-match
  color, others use standard search-match color (`EditorViewRenderer.cpp`).
- **Menu / toggle:** **View → Occurrences Highlight**; **Selection** also exposes
  case-sensitive seeds and related toggles.

## Shaping actions

Shaping helpers live in `src/editor/ShapingActions.{h,cpp}`. Capability toggles
(no-op the action when off, and availability follows via
`WorkspaceActionAvailability` / executor guards):

| Toggle setting | Actions gated |
| --- | --- |
| `editor.shaping.toggle_comment.enabled` | Toggle line/block comment |
| `editor.shaping.line_ops.enabled` | Move line up/down, duplicate line, delete line, Tab / Shift+Tab block indent |
| `editor.shaping.sort_lines.enabled` | Sort lines ascending/descending |
| `editor.multicursor.add_at_match.enabled` | Add cursor at next/all matches |

Default **Edit** menu shortcuts (editor context):

- Toggle line comment — `Ctrl+/`
- Toggle block comment — `Shift+Alt+A`
- Move line up/down — `Alt+Up` / `Alt+Down`
- Duplicate line — `Shift+Alt+Down`
- Delete line — `Ctrl+Shift+K`
- Indent / outdent lines — `Tab` / `Shift+Tab` (multi-line selections route to
  shaping; single-line `Tab` still inserts a tab / soft-tab per existing rules)
- Sort lines — no default accelerator in the built-in registry
- Add cursor next match — `Ctrl+D`
- Add cursor all matches — **`Ctrl+Shift+L`** by default (matches VS Code's
  "Select all occurrences")

### Multi-cursor mouse gestures

- **Alt+click** — add a secondary caret at the previous primary position and move
  the primary caret to the click target.
- **Shift+Alt+click** on the same column as the primary caret — add zero-width
  carets on every line between the primary line and the clicked line (inclusive),
  all at that column. Shift+Alt+click on a different column falls back to
  Alt+click behavior.

**Note:** `WorkspaceEditActionExecutor` currently invokes
`ToggleLineComment` / `ToggleBlockComment` with fixed `//` and `/*` `*/`
markers, not the per-language `LanguageContract` line/block fields. Contract
comment metadata is still used where insertion and pair logic consult it.

## Snippets

- **Settings:** `editor.snippets.enabled` (default on).
- **Plugins:** `ctx.snippets.add{ … }` merges `prefix` / `body` / `label` / ids into
  the contract (see plugins guideline).
- **Engine:** `editor::SnippetEngine` (`src/editor/SnippetEngine.{h,cpp}`) drives
  placeholder sessions; the `Insert Snippet…` overlay routes through
  `ActionId::InsertSnippet` / `ShowInsertSnippetOverlay`, and completion routing
  delivers contract snippets alongside language items.

## Save-time normalization

- **Settings:** `editor.save.trim_trailing_whitespace` (default off,
  project-scoped), `editor.save.ensure_final_newline` (default off,
  project-scoped).
- **Implementation:** `WorkspaceSaveNormalization::{TrimTrailingWhitespace,EnsureSingleFinalNewline}`
  mutates a line vector during the save pipeline; wired to viewport flags from
  workspace editor preferences (`WorkspaceShellEditor.cpp`).

## Command palette names

Toggle commands use stable ids such as `toggle-editor-folding`,
`toggle-editor-sticky-scroll`, `toggle-editor-indent-guides`,
`toggle-editor-render-whitespace`, `toggle-editor-outline`,
`toggle-editor-bracket-match-highlight`, `toggle-editor-auto-close-pairs`,
`toggle-editor-surround`, `toggle-editor-smart-indent`,
`toggle-editor-toggle-comment`, `toggle-editor-line-ops`,
`toggle-editor-sort-lines`, `toggle-editor-add-cursor-at-match`,
`toggle-editor-occurrences-highlight`, `toggle-editor-search-case-sensitive`,
`toggle-editor-snippets`, `toggle-editor-save-trim`,
`toggle-editor-save-ensure-newline`, `toggle-editor-auto-detect-indent`
(`WorkspaceCommandRegistry.cpp`).

## Menu map (built-in)

- **View:** Word wrap, code folding, sticky scroll, indent guides, render
  whitespace, symbol outline, bracket match highlight, occurrences highlight,
  zoom.
- **Selection:** Add-cursor actions, toggles for add-cursor, occurrences, and
  case-sensitive search seeds.
- **Preferences → Editor:** Auto-close, surround, smart indent, auto-detect
  indent, shaping toggles, snippets toggle, save normalization toggles.

---

For performance and allocation discipline on these paths, see
`openspec/changes/editor-essential-capabilities/tasks.md` sections 13–14 (many items
  still open).
