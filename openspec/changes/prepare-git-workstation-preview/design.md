## Context

The product vision currently lists plugin security hardening, including safe-mode startup, as a non-goal. The roadmap argues that a Git/diff/merge tool must be safe to open unfamiliar repositories with, and the README/plugin trust model already state that repo-local plugin directories are ignored. A minimal disable-plugins/safe-mode path is a scoped trust feature, not a full sandbox.

This change is a release-readiness proposal. It should update product scope and docs without expanding into hosted PR review, plugin marketplace, or general IDE breadth.

## Goals / Non-Goals

**Goals:**
- Define the preview promise and unsupported features.
- Add `--disable-plugins` and `--safe-mode` startup behavior.
- Surface plugins-disabled state in the UI.
- Reconcile README, plugin trust model, SECURITY.md, and release docs.
- Define release artifact and validation criteria.

**Non-Goals:**
- No plugin sandbox, capability prompts, signatures, marketplace, or remote install.
- No project-local plugin loading.
- No hosted provider review/auth.
- No signed binary requirement unless promoted later.
- No comparative benchmark or marketing claims.

## Decisions

- `--disable-plugins` disables user-scope plugin loading while leaving built-in workflows available. It is useful for trusted local repos when a user wants deterministic startup.
- `--safe-mode` implies `--disable-plugins` and may also skip session restore or other nonessential startup state if needed to recover from crashes. The first implementation should be intentionally small and documented.
- Plugins-disabled state is visible in status/help surfaces so users can verify the mode after launch.
- Preview release docs list tested workflows and limitations rather than broad IDE claims.

## Risks / Trade-offs

- Changing product-vision non-goals can invite broad security work -> the spec must explicitly keep sandboxing, prompts, marketplace, and signatures out of scope.
- Safe mode can become a dumping ground -> define minimal behavior and add tests so it remains predictable.
- Release artifacts can imply stability -> docs must label the preview experimental and list data-loss reporting instructions.

## Migration Plan

1. Update product scope and preview release spec.
2. Add CLI flags and plugin-runtime startup guards.
3. Add visible disabled-state UI and tests.
4. Reconcile docs and add `SECURITY.md`.
5. Add release checklist and packaging artifacts when the required workflow changes are complete.
