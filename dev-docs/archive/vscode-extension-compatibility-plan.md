# VS Code Extension Compatibility — archived decision

Reviewed on 2026-05-23. Amended 2026-06-25 (see "Amended direction" below).

## Status

**Out of scope.** This plan was audited during the 2026-04 vision-alignment pass
(`openspec/changes/archive/2026-04-24-establish-vision-and-diff-merge-excellence/tasks.md`
task 5.2). No implementation file ever lived in the tree.

## Amended direction (2026-06-25)

The original non-goal — **running VS Code extensions as-is** — still holds: there
is no `vscode` API shim, no Node runtime, and no marketplace. What changed is the
adjacent goal of **portability**: making it possible to *rewrite* a top extension
against microide's narrow Lua seams. The `feat/plugin-rendering` branch closed the
largest portability gaps so the language-server, theme, decoration, linter/
formatter, status/sidebar, reactive-decoration, and editing-extension classes can
be ported. Specifically added: host-owned buffer edits (`ctx.editor.apply_edits`),
debounced reactive editor events (`on_buffer_change`/`on_cursor_move`/
`on_selection_change`/`on_buffer_close`), and snippet prefix tab-trigger expansion.

Still deliberately out of scope (genuine exceptions to "any top extension is
portable"): notebooks (Jupyter), remote / Live Share, AI inline + chat, and full
interactive webviews. Mid-line virtual text rendering (inline color swatches /
mid-line annotations) remains **deferred** — its phantom-column-shift threads
through the editor's hot-path glyph/caret/selection geometry and the cross-layer
click hit-test, a risky rework reserved for a dedicated follow-up.

## Decision

MicroIDE does **not** target VS Code extension API compatibility. The durable product contract is:

- Built-in editor, diff, merge, git, search, and terminal workflows stay **host-owned**.
- Extension is through **Lua 5.4 plugins** and narrow host registries (see
  [`guidelines/plugins.md`](../../guidelines/plugins.md) and
  [`openspec/specs/product-vision/spec.md`](../../openspec/specs/product-vision/spec.md)).

Explicit non-goals include plugin marketplaces, remote install flows, and
Micro-plugin / VS Code extension compatibility (`ROADMAP.md` → "Not On This Roadmap").

## If This Ever Returns

Promote it as its own OpenSpec change with:

1. A concrete user workflow that cannot be met by the Lua plugin model.
2. A security and trust model beyond the current user-scope plugin stance
   ([`guidelines/plugin-trust-model.md`](../../guidelines/plugin-trust-model.md)).
3. Explicit rejection or amendment of the product-vision non-goals.

Until then, do not add VS Code compatibility references to the active roadmap.
