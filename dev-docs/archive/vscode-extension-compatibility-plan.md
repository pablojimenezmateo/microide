# VS Code Extension Compatibility — archived decision

Reviewed on 2026-05-23.

## Status

**Out of scope.** This plan was audited during the 2026-04 vision-alignment pass
(`openspec/changes/archive/2026-04-24-establish-vision-and-diff-merge-excellence/tasks.md`
task 5.2). No implementation file ever lived in the tree.

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
