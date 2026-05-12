# UI Shell Guide

Purpose: define the durable rules for shell composition, event routing, redraw, and host-owned presentation in `microide`.

## Quick Scan

- The shell is host-owned and desktop-native.
- State changes, action routing, and render code should be separable and testable.
- Coordinators should stay thin and explicit about which state they mutate.
- Paint code should render already-decided state; it should not become the place where product logic lives.
- Input latency, redraw cost, and resize behavior are core product qualities.

## Shell Ownership

The shell owns:

- window-level composition
- menu, sidebar, overlay, prompt, and panel layout
- redraw invalidation and retained repaint behavior
- host-owned rendering primitives and visual semantics
- routing from SDL events into explicit coordinators and state updates

Plugins may request contributions, but they do not own frame composition, paint order, or raw surface internals.

## Structure Guidelines

- Keep state models in focused headers or modules instead of growing one giant shell type.
- Keep coordinators responsible for translating user intent into state changes and service calls.
- Keep root views and render helpers responsible for drawing current state, not inventing new behavior.
- If a feature needs new shell behavior, prefer adding a focused state model or coordinator instead of widening unrelated ones.

## Render-Path Rules

- Rendering stays host-owned.
- Render functions consume typed POD-like view-model structs built by `RenderViewModelBuilder`. Do not call `WorkspaceShell` member functions, query coordinators, or read mutable shell state from inside a draw pass. The architectural-lint test rejects render TUs that read `context_.current_project_state` or call `CurrentTextInputSurface(...)`.
- View models do not hold pointers or references to the shell, coordinators, or services. They contain exactly the fields the surface needs.
- Keep layout math, hover hit-testing, dirty-region invalidation, and interaction mapping explicit and reviewable.
- Avoid hidden redraw side effects. Invalidation should happen through clear pathways.
- Reuse host render primitives so the shell does not drift into many one-off drawing conventions.
- Protect typing, scrolling, and resize responsiveness when adding new UI behavior.

## Editor text surface: decorated-row layer order

The normal-editor path builds `editor::EditorViewModel` in
`RenderViewModelBuilder::BuildEditorViewModel` (today: `fold_gutter_marks`,
`occurrence_ranges`) and renders through `EditorViewRenderer::Render`. Within each
visible row, **decoration fills** are appended to a `DecoratedTextRow` in this
order before the row is rasterized once via `DecoratedTextGridRenderer`:

1. Active-line row background (caret line highlight).
2. Buffer-search match highlights (when the buffer search overlay is active).
3. **Occurrence underlay** — viewport-bounded word matches from
   `EditorViewModel::occurrence_ranges` (seed match uses the active highlight
   color).
4. Text selection fills.
5. **Bracket pair match** — single-cell emphasis for opener and closer when the
   bracket-match toggle is on and a pair is resolved.
6. **Indent guides** — 1px vertical segments at indent columns (muted vs border
   color; “active” guide emphasized).
7. **Render-whitespace** glyphs — low-contrast dot / tab rule fills in the same
   decoration list (not a separate view-model vector).

Then the row draws **syntax-colored text runs**, **diagnostic underlines**, and
the combined decorations through the shared decorated-grid primitive.

After the text pass for that row, the gutter paints host-owned chrome (diagnostic
severity marker, **fold affordances** from `FoldGutterMark` / folding model state,
line numbers, secondary carets, primary caret). **Sticky scroll** renders a
host-owned band above the editor when fold state and the `editor.fold.sticky_scroll.enabled`
setting agree; `RenderViewModelBuilder::BuildEditorSurface` populates
`EditorViewModel::sticky_lines` and the renderer reserves height accordingly.

**Snippet overlay:** the in-tree `editor::SnippetEngine` plus the `Insert Snippet…`
overlay (`ActionId::InsertSnippet`, `ShowInsertSnippetOverlay`) drive snippet
sessions. New placeholder visuals should follow the decorated-row discipline and
sit relative to selection/caret per the editor-essential-capabilities spec.

## Interaction Rules

- Separate event routing from durable state models when possible.
- Keep mouse, keyboard, prompt, and text-input handling testable without requiring deep render inspection.
- Prefer deterministic helpers for hit-testing, geometry, and selection math.
- `LayoutMode` is resolved by the layout service from user override, compact breakpoint, and
  hysteresis. Compact mode should remove nonessential chrome, not hide durable commands; menu
  overflow/compact chrome must keep every top-level menu reachable.
- Small visual controls keep explicit hit pads: resize handles, scrollbars, tab closes, terminal
  new-tab controls, and menu overflow affordances must be easier to hit than their visual size.
- The status bar and settings-style overlays are host-owned surfaces. Status-bar rows route to
  actions through segment ids, while Settings and Help/About use the shared
  settings-overlay service and view model rather than ad-hoc render or plugin-owned surfaces.
- Status surfaces split by **time scale**, not by ownership: the footer status bar
  (`workspace-status-bar` spec, `WorkspaceShellRenderStatusBar.cpp`) is **static identity** —
  host-owned, closed enum, glanceable. It answers "where am I, what mode is this in, is a
  language server attached?" Its segments are populated by `RefreshStatusBar` and the LSP
  segment reads readiness only (`ensure_started=false`). Plugins SHALL NOT add footer
  segments. The **breadcrumb status ribbon** (`WorkspaceStatusRegistry`,
  `WorkspaceShell::ComputeVisibleStatusItems`) is **transient signal** — plugin-extensible,
  priority-sorted, ephemeral. It answers "what is happening right now?" — in-flight LSP
  requests, plugin build/format/lint pings, similar dynamic state. Plugin authors targeting
  status SHALL contribute to the breadcrumb; do not propose adding plugin extensibility to
  the footer.
- Avoid embedding service logic directly in render or input handlers when a coordinator can own it.
- Single-line shell input surfaces (prompts, command input, overlay query, sidebar search) consume `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`. Do not reimplement insert / backspace / delete / movement / selection / clipboard / select-all per surface.
- The active editor viewport is owned by the active editor tab. Resolve it through `EditorTabService::ActiveViewport()` (or equivalent typed accessor); do not reintroduce a shell-level or project-level fallback under any name.

## Testing Expectations

- Add coverage for interaction behavior that crosses state and UI boundaries.
- Prefer tests that assert observable shell behavior, not private implementation structure.
- Use dummy-video SDL tests carefully and keep redraw-sensitive cases serial when global SDL state is involved.
- If UI logic is difficult to test, move the decision-making into a helper or coordinator with clearer inputs and outputs.
