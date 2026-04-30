# Text Surface Unification

Reviewed on 2026-04-23.

This document defines the current contract for text interaction across the workspace shell.
New text surfaces should fit one of these two families instead of adding one-off clipboard,
caret, or selection behavior. Open implementation debt should be tracked in
`docs/known-tech-debt.md`, not in a separate audit file.

## 1. Single-line text-input surfaces

These surfaces own a mutable string plus a caret:

- prompt inputs such as rename, create, and delete confirmations
- the command prompt
- the chat composer (single-line-equivalent shortcuts only; multiline storage remains)
- centered overlays such as file finder and commit picker
- buffer-search and buffer-replace fields
- project-search sidebar query fields
- sidebar search query and replace inputs

Rules:

- typed text and pasted text must route through `WorkspaceTextInputCoordinator`
- `Ctrl+V` and menu paste must use the same insertion path
- rendering must use the shared single-line tail-fitting helpers so long input stays visible
- caret drawing and blink timing must use the shared non-editor text-input caret path
- IME composition should render through the same visual model as normal text entry

If a new surface needs custom styling, keep the styling local but keep insertion, caret
measurement, composition, and paste routing on the shared path.

### Chat composer multiline exception

The chat composer is still stored as a multiline `TextViewport`, but behavior that is
single-line-equivalent now routes through the shared `SingleLineKeyHandler` model:

- horizontal caret movement (`Left`/`Right`/`Home`/`End`)
- single-line edit operations (`Backspace`/`Delete`)
- line-scoped `Select All` and `Cut` when focused in chat

Multiline-only behavior stays on the viewport path:

- newline insertion (`Enter`)
- vertical motion (`Up`/`Down`)
- paging (`PageUp`/`PageDown`)

## 2. Navigable text viewports

These surfaces expose text selection and navigation even when they are read-only:

- normal editor panes
- read-only virtual documents
- compare panes, including read-only right-hand panes in branch or commit comparisons
- other viewport-backed text surfaces added later

Rules:

- `Select All`, `Copy`, and context-sensitive copy actions must target the active navigable
  viewport, not only editable editors
- edit actions such as cut, paste, undo, redo, backspace, delete, enter, and tab still require
  an editable viewport
- keyboard navigation, caret invalidation, and selection sync should work for read-only viewports
  the same way they do for editable ones

This split is intentional: read-only text should remain navigable and copyable without becoming
editable.

## 3. Tab path actions

Editor-tab context menus should always expose:

- `Copy Relative Path`
- `Copy Absolute Path`

These actions operate on the active tab path. Right-clicking a tab must retarget the active tab
before the menu runs so the copied path matches the tab the user opened the menu on.
