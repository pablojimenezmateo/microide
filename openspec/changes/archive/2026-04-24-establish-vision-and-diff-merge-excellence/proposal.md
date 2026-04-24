## Why

MicroIDE has shipped a large amount of capability — a multi-project SDL3 shell, compare and merge tabs, a PTY terminal, plugin runtime, LSP/DAP/AI provider bridges — but its durable product identity is under-declared. `docs/implementation-guide.md` still lists "AI/chat surfaces" as a non-goal while the AI platform is shipped, and the current `ROADMAP.md` is a validation-pass for the plugin phase rather than a product thesis. Before the next round of feature work, we need a single place that declares the product shape ("fastest low-footprint AI-focused IDE with best-in-class diff and merge, CPU-rendered, keyboard-first"), and we need the diff and merge editors — the product's main differentiator — promoted from "shipped tabs" to a first-class, measured capability with a shared rendering pipeline and durable performance budgets.

## What Changes

- Declare the durable product thesis: a native C++/SDL3 single-window IDE with no GPU acceleration, a VSCode-shaped surface area with Zed-class responsiveness, and AI as a first-class workflow rather than an opt-in plugin. Correctness > speed > CPU > memory, with low input latency as an explicit budget.
- Refresh `docs/implementation-guide.md` so AI/chat is listed as an in-scope, host-owned workflow, and add explicit budgets (startup under target ms on a warm repo, typing/scroll frame budget, idle CPU near zero).
- Refresh `AGENTS.md` so the "Current Project Reality" and product-pillar statements include AI workflows and call out diff/merge excellence as a product goal; plugin expansion was originally an active phase but is now a shipped capability, not the dominant theme.
- Rewrite `README.md` as a product-first document: a short pitch (fastest low-footprint AI-focused IDE with first-class diff/merge, CPU-rendered, keyboard-first), a feature summary grouped by capability, and a build / run section. Retire the unstructured historical changelog the current README has grown into.
- Audit `docs/` and retire or archive docs that no longer describe current reality: `docs/chat-pane-plan.md` is shipped (move to an archive or delete), and any other plan-style docs whose phases are complete should be archived. Reference docs (plugin runtime research, macOS plan, profiling, performance findings) stay in place.
- Promote diff and merge to a first-class capability: one shared row-decoration and text-grid pipeline across editor / compare / merge, no file-size-threshold degradations of diff semantics, and published correctness and latency budgets for compare open, merge conflict navigation, and hunk application. Follow `docs/diff-editor-merge-rewrite-plan.md` where it exists and fill the gaps where it does not.
- Promote AI workflows to a first-class, host-owned capability: chat, inline completion, and MCP tool flows remain built-in (not plugins), with bounded context collection, session-scoped approvals, and a provider-bridge contract that treats stdio/HTTP/ACP transports uniformly.
- Establish a performance budget spec that every redraw, typing, scrolling, compare, merge, and chat interaction path must fit inside, tied to `MICROIDE_PERF_TRACE` evidence in reviews of perf-sensitive changes.
- **BREAKING (docs-only)**: Replace the "non-goals" list in the implementation guide with a narrower one. `AI/chat surfaces` moves off the non-goals list. `Debugger UX beyond first-pass`, `plugin marketplaces`, `cloud/collaboration/sync`, `recent-project surfaces`, and `soft wrap` remain out of scope.

## Capabilities

### New Capabilities

- `product-vision`: the durable product thesis, priority order, and in-scope / out-of-scope declaration that every future change must align with.
- `diff-merge-editor`: the first-class behavioral contract for compare tabs, three-way merge tabs, and the shared decorated text-grid pipeline that backs them.
- `ai-workflows`: the host-owned contract for chat, inline completion, MCP tools, provider bridges, and bounded context collection.
- `performance-budgets`: the durable latency, CPU, and memory budgets that perf-sensitive changes must measure against.

### Modified Capabilities

<!-- No existing specs in openspec/specs/ — this is the first spec-driven change. -->

## Impact

- Docs: `docs/implementation-guide.md`, `ROADMAP.md`, `docs/active-work.md`, `AGENTS.md`, `CLAUDE.md`, `README.md`, and `guidelines/architecture.md` all reference product scope and will need a targeted alignment pass so they no longer contradict each other on AI scope, plugin-phase framing, or diff/merge priority. Shipped plan docs under `docs/` (notably `docs/chat-pane-plan.md`) will be archived; reference docs remain.
- Code (behavioral, downstream): the diff-merge unification touches `src/compare/*`, `src/editor/TextViewport*`, and the shared row-decoration pipeline used by editor / compare / merge render. The AI capability touches `src/workspace/WorkspaceAiProvider*`, `WorkspaceInlineCompletion*`, `WorkspaceConversation*`, `WorkspaceExternalAgent*`, `WorkspaceMcpTool*`, `WorkspaceAiContext*`, and `WorkspaceProviderBridge*`. Perf budgets touch the tracing entry points in `docs/startup-tracing.md` and `docs/runtime-profiling.md`.
- APIs: no user-facing API breaks. Plugin contribution surfaces (`ctx.ai_providers`, `ctx.external_agents`, `ctx.mcp_tools`) stay; their semantics are tightened rather than replaced.
- Dependencies: none added. PCRE2, SDL3, SDL3_ttf (optional), Lua 5.4 remain the full stack.
- Risk: the vision declaration will retire some deferred items (notably AI surfaces as "non-goal"); downstream docs and ADRs must be updated in the same slice to avoid a stale guidance layer.
