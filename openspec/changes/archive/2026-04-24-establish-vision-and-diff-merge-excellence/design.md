## Context

MicroIDE's shipped surface is already large: SDL3 shell, multi-project workspace, editor / compare / merge tabs, shared-buffer splits, PTY terminal, Lua plugin runtime, LSP/DAP managers, formatter / save-participant / completion / code-action / task / tool / test / output-channel registries, SCM / annotation / auth / review / virtual-document registries, AI providers, conversation model, external-agent runtime, MCP tools, bounded AI context, a host-owned chat pane, ghost-text inline completion, a native `microide_provider_bridge` binary, and repo-owned dogfood plugins for ESLint and LLM workflows.

The constraints this design operates under:

- `docs/implementation-guide.md` still lists "AI/chat surfaces" as a non-goal, which contradicts the shipped chat pane, provider bridge, and MCP permission flow.
- `ROADMAP.md` is a validation pass for the plugin phase; it does not declare the product's durable shape.
- Compare and merge are shipped but render through overlapping code paths rather than one unified decorated text-grid pipeline. `docs/diff-editor-merge-rewrite-plan.md` captures the intended unification target.
- Performance findings (`docs/performance-findings.md`, `docs/known-tech-debt.md` items 8–12) are tracked as debt rather than as durable budgets.
- The new `guidelines/` handbook and `CLAUDE.md` were just merged; they define source-of-truth order (`AGENTS.md` > `docs/active-work.md` > `docs/implementation-guide.md` > focused docs > handbook) but do not yet include a product-vision spec.

Stakeholder: this is a single-maintainer project; the audience for these specs is the maintainer plus any AI coding agent working in the repo under `CLAUDE.md`.

## Goals / Non-Goals

**Goals:**
- Establish `openspec/specs/product-vision` as the durable, machine-readable product thesis that other changes must align with.
- Establish `openspec/specs/diff-merge-editor` and `openspec/specs/performance-budgets` as the durable correctness and latency contracts for the product's main differentiator.
- Establish `openspec/specs/ai-workflows` as the durable contract for the shipped AI surfaces, so the non-goal entry in the implementation guide can be retired in the same slice.
- Align the durable docs that currently touch product scope (`docs/implementation-guide.md`, `ROADMAP.md`, `docs/active-work.md` where it duplicates vision, `AGENTS.md`, `CLAUDE.md`, `guidelines/architecture.md`, `guidelines/performance.md`) so they no longer contradict the new specs. Rewrite `README.md` as a product-first document. Archive shipped plan docs under `docs/archive/`.
- Keep the diff-merge unification as a directional commitment backed by `microide_diff_bench` rather than a full implementation inside this change.

**Non-Goals:**
- Implementing the unified decorated text-grid pipeline. That work is downstream and will be proposed as its own change (or series of changes) that cite this spec.
- Changing plugin API surfaces. Plugin contribution seams (`ctx.ai_providers`, `ctx.mcp_tools`, etc.) stay as shipped.
- Adding new external dependencies. No new crates, libraries, or runtimes.
- Writing a performance test harness beyond the existing `microide_diff_bench`, `microide_search_bench`, and `MICROIDE_PERF_TRACE` tooling. Budgets tie to those tools as they exist today.
- Retro-blessing every shipped behavior. Specs describe the durable contract, not the current implementation's quirks.

## Decisions

### D1: Product vision lives in `openspec/specs/`, not only in prose docs

OpenSpec specs are machine-readable and diff-reviewable; prose docs drift. `docs/implementation-guide.md` still lists AI as a non-goal months after AI shipped, which is exactly the failure mode we need to prevent.

Alternative considered: keep vision in `docs/implementation-guide.md` and treat OpenSpec as implementation-only. Rejected because the implementation guide has already drifted, and there is no mechanical way to flag a future proposal that contradicts it.

Decision: the durable product thesis is the `product-vision` spec. Prose docs reference it and are updated in the same slice when it changes.

### D2: Diff and merge are first-class, not "editor tabs with extras"

The product pitch is "best-in-class diff and merge." That requires a durable spec separate from "editor," because compare and merge have tab-level semantics (hunk navigation, per-hunk apply, whole-side apply, change-overview lane, state preservation across rename / delete / reopen) that the editor spec does not need.

Alternative considered: fold compare and merge into a single `editor` spec. Rejected because the compare and merge state machines are independently testable and have their own failure modes (orphaned commit-side paths, hunk navigation wrap rules, conflict resolution state).

Decision: `diff-merge-editor` is its own capability with its own spec. The unified render pipeline is a stated requirement; the implementation is deferred to a follow-up change.

### D3: AI is in scope, but the surface contract is host-owned

The shipped AI platform is already host-owned: chat pane, inline completion, MCP permissions, provider bridge. The spec codifies that shape rather than re-opening the host-versus-plugin question.

Alternative considered: leave AI as a plugin-contributed capability without a host spec. Rejected because chat, inline completion, and MCP permission prompts have product-critical UX (draft retention, streaming without stalling the shell, session-scoped approvals) that cannot be delegated to plugins without regressing the experience.

Decision: `ai-workflows` spec pins the host-owned surfaces. Providers remain plugin-contributable through the existing registries, but the UI and permission flow stay built in.

### D4: Performance budgets are a capability, not a style guide

Perf is the project's second-highest priority after correctness. Keeping budgets only in `guidelines/performance.md` makes them easy to slide past in review. Treating them as a capability with scenarios lets a future change that regresses typing latency be flagged as a spec violation, not just a style violation.

Alternative considered: keep perf as prose in `guidelines/performance.md`. Rejected because prose guidance has not prevented the regressions already tracked in `docs/known-tech-debt.md`.

Decision: `performance-budgets` spec ties each budget to a specific tool (`MICROIDE_PERF_TRACE`, `microide_diff_bench`, `microide_search_bench`) and requires before/after evidence on hot-path changes.

### D5: This change is specs-first, docs-aligned, code-untouched

The proposal sets direction. Downstream changes will implement the diff-merge unification, retire the implementation-guide non-goal list, and wire perf budgets into CI checks. Bundling implementation into this change would blur what the specs actually require.

Alternative considered: land the implementation-guide doc update in this change. Accepted in scope as a pure alignment task because leaving a contradictory non-goal entry would immediately violate `product-vision` requirement "AI Is In Scope." No implementation code changes.

Decision: this change creates specs and updates doc pointers only. Any code change is a follow-up change that cites these specs.

## Risks / Trade-offs

- [Spec drift from shipped behavior] → Each spec scenario is phrased to match the existing shipped behavior described in `README.md` and `docs/active-work.md`. Where the spec requires more than the shipped behavior (unified render pipeline, no file-size-threshold diff degradation), the scenario uses SHALL and is called out in this design as a deferred implementation target.
- [Budgets without numeric thresholds] → Budgets reference tools (`MICROIDE_PERF_TRACE`, `microide_diff_bench`) rather than hard numbers, because the current benchmarks do not have published per-op targets on a reference host. A follow-up change can add numeric thresholds once a reference host is declared. The scenarios still bite, because they require before/after evidence on hot-path changes.
- [Non-goal retirement breaks expectations] → Removing "AI/chat surfaces" from the non-goal list may surprise readers of older docs. Mitigation: update `docs/implementation-guide.md` in the same slice and leave a short "AI is in scope, see `openspec/specs/ai-workflows`" pointer where the non-goal used to live.
- [OpenSpec becomes a second source of truth that drifts the other way] → `CLAUDE.md` already declares a source-of-truth order. Add one sentence to `CLAUDE.md` pointing at `openspec/specs/` as the authoritative product-contract layer, above focused docs but aligned with the active-work priority stack for in-flight work.

## Migration Plan

This change is docs- and spec-only. There is no deployment step.

Rollout order inside the change:
1. Create the four spec files (done as part of this proposal's `specs/` artifact).
2. Update `docs/implementation-guide.md` to remove "AI/chat surfaces" from the non-goal list and to reference `openspec/specs/product-vision` and `openspec/specs/ai-workflows`.
3. Update `AGENTS.md` so "Current Project Reality" includes AI workflows as a built-in pillar and no longer frames plugin support as the single active phase.
4. Rewrite `README.md` as a product-first document (pitch, capability-grouped highlights, build, controls, commands, benchmarks), replacing the current accreted changelog.
5. Audit `docs/`: archive `docs/chat-pane-plan.md` (shipped) and any other plan docs whose phases are complete under a new `docs/archive/` directory. Keep reference docs in place.
6. Update `ROADMAP.md` so the "next phase" section references the diff-merge unification and AI validation specs rather than carrying its own scope definition.
7. Update `CLAUDE.md` source-of-truth ordering to include `openspec/specs/` as the product-contract layer, directly below `AGENTS.md`.
8. Update `guidelines/architecture.md` and `guidelines/performance.md` to point at `openspec/specs/` for product scope and perf budget questions.

Rollback: revert the commit. No runtime impact.

## Open Questions

- Should numeric frame-time and startup-time thresholds be added to `performance-budgets` now, or in a follow-up change once a reference host is declared? (Current answer: follow-up.)
- Should `diff-merge-editor` also cover the sidebar git view and commit picker, or keep those as separate capabilities? (Current answer: keep them separate; they are discovery surfaces, not compare/merge render surfaces.)
- Does `ai-workflows` need a separate `ai-providers` capability for the plugin-contribution side, or is the single spec sufficient? (Current answer: single spec; provider registration is already covered by existing plugin contribution registries and does not need its own durable scenarios yet.)
