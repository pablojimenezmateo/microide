## Context

MicroIDE's current AI execution model is split across two seams. Provider metadata and auth integrations are registered in-process through the plugin host, but actual chat and inline-completion execution is routed through `ExternalAgentSpec` plus `WorkspaceProviderBridgeManager`, which requires a stdio bridge process and a bridge-specific NDJSON protocol. That split adds transport-specific complexity to the shell, duplicates execution logic outside the host, and makes direct providers such as OpenAI, Claude, and DeepSeek look more exotic than they are.

The near-term provider target set does not justify a bridge-first architecture. OpenAI, Anthropic, and DeepSeek all support direct HTTP request/response APIs plus streaming over server-sent events or OpenAI-compatible chunked streams. The better durable boundary is a host-owned runtime contract with plugin-owned provider logic behind it, while preserving sidecars only for the minority of providers that genuinely need process isolation or already ship as standalone CLIs.

## Goals / Non-Goals

**Goals:**
- Define one host-owned AI runtime interface that chat, inline completion, auth status UI, model pickers, and tool approval flows consume without transport branching.
- Move provider-specific auth semantics, request shaping, model enumeration, streaming parsing, and tool-call handling behind plugin-owned runtime implementations.
- Make direct HTTP/SSE provider execution the default path for OpenAI, Claude, and DeepSeek.
- Keep the host responsible for generic platform concerns: secret access, HTTP transport, cancellation, wake-event delivery, and permission mediation.
- Preserve an optional sidecar/stdio adapter for providers that truly benefit from process separation.

**Non-Goals:**
- Replacing host-owned chat or inline-completion UI with plugin-defined UI.
- Designing a plugin marketplace or remote provider installation system.
- Supporting every possible provider in the first slice; the initial contract is optimized for OpenAI, Claude, and DeepSeek.
- Eliminating sidecars entirely; they remain a supported adapter, just not the baseline architecture.

## Decisions

### D1. The durable seam is `AiProviderRuntime`, not `WorkspaceProviderBridgeManager`

The host will expose a narrow runtime contract for AI provider execution. Callers will target runtime operations such as auth status, model discovery, request start, streaming updates, tool-call round-trips, and cancellation. The shell and coordinators will stop depending on bridge-specific state such as bridge agent ids, bridge request ids, or bridge startup lifecycle.

**Why this shape:** the current bridge manager is both a transport implementation and the public contract. That makes stdio an architectural requirement instead of one adapter choice.

**Alternatives considered:**
- Keep `WorkspaceProviderBridgeManager` as the contract and add direct providers under the same bridge API. Rejected because it preserves the wrong abstraction and leaks bridge lifecycle semantics into callers.
- Let chat and inline completion each define their own provider callback surface. Rejected because it would duplicate execution, cancellation, and streaming logic.

### D2. Plugins own provider behavior; the host owns generic transport primitives

Provider-specific behavior belongs to plugin-owned runtimes: auth semantics, request/response mapping, model enumeration rules, and tool-call semantics. The host remains responsible for generic services that should not be reimplemented per plugin: secret lookup, HTTP/SSE streaming transport, cancellation tokens, SDL wake-event delivery, output-channel logging, and MCP tool permission mediation.

**Why this shape:** it gives plugins real control over provider behavior without forcing every plugin to solve networking, threading, or shell wake-up concerns independently.

**Alternatives considered:**
- Host owns all provider adapters directly. Rejected as the durable design because it weakens the plugin extension story and forces provider-specific logic into the workspace layer.
- Plugins own both provider behavior and raw networking. Rejected because it would duplicate infrastructure and complicate threading, cancellation, and observability.

### D3. The initial runtime surface targets direct HTTP/SSE providers first

The first concrete adapters will be:
- an OpenAI-compatible runtime path for OpenAI and DeepSeek
- an Anthropic Messages runtime path for Claude

These adapters may share host-owned HTTP and streaming helpers, but they remain separate runtime implementations where API semantics differ materially, especially for tool use and response event formats.

**Why this shape:** OpenAI and DeepSeek are close enough to share a compatibility-oriented path, while Anthropic's message and tool-use shape is different enough to deserve an explicit adapter.

**Alternatives considered:**
- One universal JSON adapter for all providers. Rejected because it either collapses to the lowest common denominator or grows provider-specific branching in one place.
- Three fully independent vertical integrations. Rejected because OpenAI and DeepSeek share enough wire behavior that a reusable compatibility layer is worthwhile.

### D4. Sidecars become an adapter, not the default transport

If a provider runtime wants process isolation or already exists as a CLI/stdio tool, it can implement the same host-owned runtime contract through a sidecar adapter. The host will treat this as one transport strategy alongside direct HTTP/SSE execution. User-visible behavior, state ownership, and request lifecycle semantics remain identical across both paths.

**Why this shape:** it preserves flexibility for future providers without forcing the primary providers through an extra binary.

**Alternatives considered:**
- Remove sidecar support entirely. Rejected because it would make some future providers or enterprise gateways harder to integrate.
- Continue launching one long-lived sidecar by default for every provider. Rejected because it adds avoidable complexity and startup/runtime overhead.

### D5. AI state remains host-owned and project-scoped where appropriate

Conversation state, inline-completion state, tool-approval state, and provider selections remain host-owned state. Plugins contribute runtime behavior, not UI state ownership. Per-project provider/session selections continue to live in project-owned persistence and services rather than inside plugin globals or bridge-manager caches.

**Why this shape:** the product thesis already treats chat, inline completion, and tool approval as host-owned workflows. That should not change while the execution seam changes.

**Alternatives considered:**
- Let plugins own conversation and completion session state. Rejected because it would fragment persistence and weaken the host-owned workflow model.

## Risks / Trade-offs

- **[Risk] The plugin/runtime contract grows too broad** → Mitigation: keep provider behavior focused on auth, model discovery, request execution, tool-call semantics, and cancellation, while generic HTTP/secret/wake-event services stay host-owned.
- **[Risk] OpenAI-compatible and Anthropic adapters diverge in subtle ways** → Mitigation: define typed runtime events at the host boundary instead of leaking raw provider payloads.
- **[Risk] Removing bridge-first assumptions touches many AI call sites** → Mitigation: introduce the runtime seam first, migrate chat and inline completion onto it, then delete bridge-specific members and helpers.
- **[Risk] Plugin-owned runtime callbacks could stall the shell if invoked synchronously** → Mitigation: require runtime execution to remain asynchronous and deliver results exclusively through existing wake-event routing.
- **[Risk] Auth behavior becomes split between auth-provider plugins and AI-provider plugins** → Mitigation: let AI runtimes depend on host auth/session services rather than reintroducing ad hoc secret handling inside request callers.

## Migration Plan

1. Introduce the `AiProviderRuntime` contract and host-owned runtime orchestration service without removing the existing bridge manager.
2. Add direct runtime adapters for OpenAI-compatible and Anthropic providers, plus provider registration/runtime plumbing for plugin-owned execution.
3. Migrate chat, inline completion, auth status checks, model enumeration, and tool approval flows to the runtime service.
4. Keep stdio/sidecar support behind a bridge-backed runtime adapter for any providers not yet migrated.
5. Remove bridge-specific shell members, request bookkeeping, and selection logic once all primary call sites route through the runtime service.
6. Update durable docs/specs and add regressions for direct-provider flow, sidecar fallback, cancellation, and non-blocking delivery.

## Open Questions

- Should the first implementation make OpenAI, Claude, and DeepSeek built-in runtimes, or should even those ship through the plugin registration/runtime surface from day one?
- Do we want one generic host HTTP client service exposed to plugins, or a narrower AI-runtime-only transport helper that avoids accidental general-purpose network usage?
- How much of model discovery should be cached per provider versus queried on demand?
- Should DeepSeek be modeled strictly as OpenAI-compatible in the first slice, or do we want an explicit DeepSeek runtime type to make future capability drift easier to handle?
