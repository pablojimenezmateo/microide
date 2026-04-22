# Text Surface Audit

Reviewed on 2026-04-22.

This records the remaining places where text surfaces still diverge after the first
unification pass in `docs/text-surface-unification.md`.

## Fixed in the current slice

- chat-composer keydown no longer falls through into the underlying editor
- `Ctrl+A` and `Ctrl+C` now follow the shared navigable-viewport path for read-only
  virtual documents and read-only compare panes, not just the Edit menu actions

## Remaining gaps

### 1. Single-line inputs still do not share one key editor

Prompt input, command input, chat composer, overlay query fields, and sidebar search edit mode
now share text insertion, caret rendering, IME composition, and paste routing, but they do not
yet share one keyboard-edit model.

Current consequences:

- backspace, escape, submit, and a few mode-local keys are implemented separately
- left or right movement, home or end, delete-forward, and text selection are not unified
- Edit actions such as `Select All`, `Copy`, and `Cut` still target viewport-backed surfaces, not
  these single-line buffers

The next architectural step here is a host-owned single-line text editor state that carries:

- buffer text
- caret position
- optional selection range
- shared keydown helpers for backspace, delete, left or right, home or end, and select-all

### 2. Surface-specific command keys still live in per-surface handlers

The shell still spreads non-text key handling across:

- prompt modal handlers
- command prompt coordinator
- overlay key handlers
- sidebar search edit handlers
- panel chat handling

That split is acceptable for surface-specific actions like submit, history, or replace-all, but
basic line-editing behavior should move behind one helper.

### 3. Menu and shortcut parity should stay under regression coverage

The app already had cases where the Edit menu and keyboard shortcuts diverged. Any future text
surface work should add both:

- action-level tests
- shortcut-level tests

That is especially important for read-only compare panes, virtual documents, prompts, and panel
surfaces.
