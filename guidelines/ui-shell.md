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
  actions through segment ids, while Settings, AI Provider, and Help/About use the shared
  settings-overlay service and view model rather than ad-hoc render or plugin-owned surfaces.
- Avoid embedding service logic directly in render or input handlers when a coordinator can own it.
- Single-line shell input surfaces (prompts, command input, overlay query, sidebar search) consume `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`. Do not reimplement insert / backspace / delete / movement / selection / clipboard / select-all per surface. The chat composer is the documented multiline exception.
- The active editor viewport is owned by the active editor tab. Resolve it through `EditorTabService::ActiveViewport()` (or equivalent typed accessor); do not reintroduce a shell-level or project-level fallback under any name.

## Testing Expectations

- Add coverage for interaction behavior that crosses state and UI boundaries.
- Prefer tests that assert observable shell behavior, not private implementation structure.
- Use dummy-video SDL tests carefully and keep redraw-sensitive cases serial when global SDL state is involved.
- If UI logic is difficult to test, move the decision-making into a helper or coordinator with clearer inputs and outputs.
