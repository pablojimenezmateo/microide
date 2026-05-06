## 1. Runtime Contract

- [x] 1.1 Define the host-owned `AiProviderRuntime` and `AiProviderRuntimeService` interfaces, including typed request, event, capability, auth, model-discovery, tool-call, and cancellation surfaces
- [x] 1.2 Refactor provider selection and request bookkeeping so chat and inline completion stop depending on bridge-specific ids, bridge caches, or `ExternalAgentSpec` execution details
- [x] 1.3 Add asynchronous runtime orchestration, output-channel logging, cancellation, and SDL wake-event delivery that work uniformly for both direct and sidecar-backed providers

## 2. Plugin And Transport Surfaces

- [x] 2.1 Extend plugin AI provider registration so plugins can contribute provider-owned auth semantics, request shaping, model enumeration, streaming parsing, and tool-call behavior through the runtime contract
- [x] 2.2 Add host-owned AI transport and secret-access helpers for HTTP, SSE or chunked streaming, and provider session material without exposing `WorkspaceShell` or raw networking ownership to callers
- [x] 2.3 Implement an optional sidecar-backed runtime adapter that preserves existing subprocess-based providers behind the same `AiProviderRuntime` contract

## 3. Initial Provider Adapters

- [x] 3.1 Implement an OpenAI Responses runtime adapter with streaming chat, inline completion, cancellation, and tool-call mapping
- [x] 3.2 Implement an Anthropic Messages runtime adapter with streaming response handling and Claude tool-use mapping
- [x] 3.3 Implement DeepSeek support through the OpenAI-compatible runtime path and provider capability metadata

## 4. Workspace AI Migration

- [x] 4.1 Migrate chat request startup, streaming transcript updates, provider error handling, and cancellation to `AiProviderRuntimeService`
- [x] 4.2 Migrate inline completion request execution, cancellation, and silent provider-failure handling to `AiProviderRuntimeService`
- [x] 4.3 Migrate auth status UI, model-picker population, provider selection, and tool-approval plumbing off `WorkspaceProviderBridgeManager` and bridge-specific selection logic

## 5. Cleanup, Tests, And Docs

- [x] 5.1 Remove bridge-first assumptions and dead bridge-only plumbing from workspace AI call sites while preserving optional sidecar support behind the runtime adapter
- [x] 5.2 Add regression coverage for direct-provider flows, sidecar fallback, model discovery, cancellation, and non-blocking SDL wake-event delivery
- [x] 5.3 Update durable docs and implementation guides to describe the runtime-first AI architecture, direct OpenAI or Claude or DeepSeek support, and optional sidecar providers
