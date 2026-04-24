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
- Keep layout math, hover hit-testing, dirty-region invalidation, and interaction mapping explicit and reviewable.
- Avoid hidden redraw side effects. Invalidation should happen through clear pathways.
- Reuse host render primitives so the shell does not drift into many one-off drawing conventions.
- Protect typing, scrolling, and resize responsiveness when adding new UI behavior.

## Interaction Rules

- Separate event routing from durable state models when possible.
- Keep mouse, keyboard, prompt, and text-input handling testable without requiring deep render inspection.
- Prefer deterministic helpers for hit-testing, geometry, and selection math.
- Avoid embedding service logic directly in render or input handlers when a coordinator can own it.

## Testing Expectations

- Add coverage for interaction behavior that crosses state and UI boundaries.
- Prefer tests that assert observable shell behavior, not private implementation structure.
- Use dummy-video SDL tests carefully and keep redraw-sensitive cases serial when global SDL state is involved.
- If UI logic is difficult to test, move the decision-making into a helper or coordinator with clearer inputs and outputs.
