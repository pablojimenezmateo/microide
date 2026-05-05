## Why

MicroIDE's current AI integration is centered on `WorkspaceProviderBridgeManager` and stdio-backed bridge processes, even though the near-term provider set (OpenAI, Claude, DeepSeek) is served more naturally through direct HTTP and SSE APIs. That makes provider integration heavier than necessary, duplicates transport logic outside the host, and leaves plugin-owned provider behavior split awkwardly between metadata registration in-process and request execution out-of-process.

## What Changes

- Replace the bridge-first AI architecture with a host-owned generic AI runtime interface consumed by chat, inline completion, and tool-approval flows.
- Make provider-specific request shaping, model discovery, auth semantics, and tool-call behavior plugin-owned runtime responsibilities behind the generic interface.
- Add direct runtime adapters for the initial provider set:
  - OpenAI via the Responses API
  - Anthropic Claude via the Messages API
  - DeepSeek via its OpenAI-compatible HTTP API
- Keep sidecar or stdio-backed providers as an optional adapter path, not the default architecture or required contract.
- Remove bridge-specific assumptions from provider selection, auth status checks, model enumeration, request startup, streaming update handling, and cancellation flows.
- **BREAKING**: provider integrations that currently rely on `ExternalAgentSpec` plus bridge-only semantics will migrate to the new AI runtime contract.

## Capabilities

### New Capabilities
- `plugin-ai-provider-runtime`: plugin-owned AI provider runtimes that plug into a host-owned generic interface for auth, model discovery, streaming responses, tool calls, and cancellation without requiring an extra bridge binary.

### Modified Capabilities
- `ai-workflows`: replace the durable provider-bridge contract with a runtime-first contract whose default path is direct HTTP/SSE and whose provider-specific flow is owned by plugins.
- `workspace-architecture`: require AI provider execution to route through a narrow runtime/service seam instead of shell- or bridge-manager-specific helpers.

## Impact

- Affected code: `src/workspace/WorkspaceProviderBridge*`, chat and inline completion request paths, provider auth flows, model picker plumbing, external-agent/provider registration surfaces, and plugin provider registration/runtime APIs.
- Affected systems: host HTTP/SSE transport, cancellation and wake-event delivery, tool permission mediation, plugin runtime surfaces, and provider configuration/state.
- Dependencies: OpenAI Responses streaming, Anthropic Messages streaming and tool-use semantics, and DeepSeek OpenAI-compatible chat/function-calling endpoints.
