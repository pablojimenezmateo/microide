## Context

AI/LLM functionality is currently implemented as a cross-cutting vertical slice in MicroIDE: chat/sidebar UI, inline completion, provider runtime and bridge process, MCP invocation and approval, plugin contribution surfaces, persisted conversation/provider state, perf scenarios, and product/spec documentation. Build and packaging also require AI-specific targets and dependencies (`libcurl`, `microide_provider_bridge`).

The removal must preserve core editor/diff/search/git/terminal correctness while preventing architectural regressions (for example, leaving dead state in `WorkspaceShell`, stale spec guarantees, or orphaned persistence tags). Because this feature set spans host services, plugin APIs, and durable contracts, the work requires a staged design rather than file-by-file deletion.

## Goals / Non-Goals

**Goals:**
- Remove all host-owned AI/LLM behavior and UI surfaces, including chat, inline completion, provider auth/picker, AI status indicators, and AI/MCP command entry points.
- Remove AI runtime infrastructure and external dependencies (`libcurl`, provider bridge target, provider bridge packaging/CI wiring).
- Remove AI-specific plugin contribution surfaces and bundled AI plugins.
- Define and implement deterministic persisted-state migration behavior for AI conversation/provider data.
- Keep OpenSpec, docs, tests, and perf baselines aligned with the non-AI product direction in the same change.

**Non-Goals:**
- Replacing removed AI features with a new assistant workflow in this change.
- Preserving backward compatibility for plugin APIs that only exist to support removed AI runtime/provider/tool surfaces.
- Refactoring unrelated editor/search/git/terminal architecture beyond cleanup required to safely remove AI hooks.

## Decisions

### 1) Remove AI as a full vertical slice, not piecemeal

We will remove all AI/LLM host behavior end-to-end (UI, runtime, commands, plugin seams, persistence, docs/specs) in coordinated slices instead of introducing compatibility shims.

- **Rationale:** Piecemeal disablement leaves dead branches, stale persistence records, and contradictory specs.
- **Alternative considered:** Keep hidden runtime plumbing while removing UI only. Rejected because it keeps `libcurl`, provider bridge, and plugin contracts alive without user value.

### 2) Treat MCP tool execution as part of removed AI workflow

MCP invocation, approval prompts, and plugin MCP contribution paths coupled to chat/provider workflows will be removed in the same program.

- **Rationale:** Current MCP ownership and invocation flow are AI-workflow-centric and rely on the same runtime/approval surfaces.
- **Alternative considered:** Keep MCP as a standalone pillar. Rejected for this change because no independent product contract exists today and keeping it would materially increase scope/risk.

### 3) Remove AI dependency chain from build + packaging

Eliminate `libcurl` requirements and `microide_provider_bridge` target and remove corresponding CI/package wiring once runtime users are removed.

- **Rationale:** Dependency removal is a first-order project objective and must be enforced by build graph cleanup.
- **Alternative considered:** Keep `libcurl` optional. Rejected because optionality still preserves dead complexity and unclear ownership.

### 4) Migrate persisted AI state via tolerant decode + write omission

Session/project restore will tolerate legacy AI records during decode but stop writing AI records on save. Migration is one-way: loaded AI artifacts are ignored/discarded in runtime state.

- **Rationale:** This prevents crashes/corruption on existing workspaces while guaranteeing AI state naturally drains from persisted files over subsequent saves.
- **Alternative considered:** Hard-fail on unknown AI records. Rejected because it breaks existing user state.

### 5) Remove AI plugin APIs instead of deprecating long-term

Delete AI-only plugin contribution interfaces (`ai_providers`, `external_agents`, MCP hooks) and bundled AI plugins in-tree.

- **Rationale:** The product no longer supports these capabilities; durable dead extension points conflict with architecture direction.
- **Alternative considered:** Keep APIs with no-op host behavior. Rejected because silent no-ops create poor plugin ergonomics and maintenance cost.

### 6) Align specs/docs in the same change set

Update or retire all OpenSpec capabilities and docs that currently guarantee AI behavior, including product vision and performance harness scenario contracts.

- **Rationale:** Repository policy requires durable contracts to match shipped behavior.
- **Alternative considered:** Delay docs/spec updates until after code removal. Rejected because it creates split-brain guidance.

## Risks / Trade-offs

- **[Risk] Residual AI references survive in coordinator/render/input code paths** -> **Mitigation:** perform targeted symbol/path sweeps and compile/test each subsystem slice after removal.
- **[Risk] Persistence decode regressions with legacy AI-tagged sessions** -> **Mitigation:** add regression tests for loading old records and verify save output no longer emits AI records.
- **[Risk] Plugin host breakage from API surface deletion** -> **Mitigation:** remove in-tree plugin usage first, then tighten plugin host interfaces and corresponding tests in the same slice.
- **[Risk] Performance harness contract drift after chat scenario removal** -> **Mitigation:** update `performance-harness` spec and replace with a non-AI scenario + baseline in the same change.
- **[Trade-off] Intentional breaking change for AI plugin/runtime compatibility** -> **Mitigation:** document break clearly in proposal/spec/task artifacts and migration notes.

## Migration Plan

1. **Contract-first updates**: land proposal/spec deltas defining non-AI scope and retirement/removal semantics.
2. **Core runtime/UI removal**: remove chat/inline/provider/MCP UI/actions/render/input/runtime code and compile.
3. **Plugin/runtime/dependency cleanup**: remove AI plugin APIs and bundled plugins, then remove bridge target and `libcurl` wiring from build/CI/package scripts.
4. **Persistence migration**: keep decode compatibility for legacy AI records, stop writing AI records, add regression coverage.
5. **Perf/test/docs closure**: replace/remove AI perf scenarios and baselines, update tests, and align docs (`AGENTS.md`, implementation and active-work docs, roadmap).
6. **Validation gate**: run targeted unit/integration/perf checks covering touched subsystems before merge.

Rollback strategy is commit-level rollback of this change set; there is no in-product runtime toggle for AI once removed.

## Open Questions

- Should any non-AI subset of MCP survive as a future standalone capability, or is it fully removed for now?
- What is the canonical replacement for `chat_pane_long_transcript` in the required perf harness scenario list?
- Do we preserve read-only visibility of legacy chat data anywhere, or fully ignore/remove it at load time?
