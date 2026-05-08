# plugin-ai-provider-runtime Specification

## Purpose
TBD - created by archiving change plugin-owned-ai-provider-runtime. Update Purpose after archive.
## Requirements
### Requirement: Host-Owned AI Runtime Interface
MicroIDE SHALL expose a host-owned AI runtime interface that chat, inline completion, auth status UI, model selection, and tool approval flows consume without branching on provider transport. The runtime interface SHALL cover auth status, model discovery, streaming response events, tool-call round-trips, cancellation, and capability reporting.

#### Scenario: Chat request uses the runtime interface
- **WHEN** the user starts a chat request
- **THEN** the chat workflow SHALL target the host-owned AI runtime interface rather than invoking a bridge-specific or transport-specific API

#### Scenario: Inline completion uses the same runtime interface
- **WHEN** the editor requests an inline completion
- **THEN** the inline-completion workflow SHALL use the same AI runtime interface used by chat, with provider-specific behavior hidden behind the runtime implementation

### Requirement: Provider-Specific Behavior Is Plugin-Owned
Provider-specific auth semantics, request shaping, model enumeration rules, streaming parsing, and tool-call behavior SHALL be owned by plugin-provided AI runtime implementations. The host SHALL continue to own secret access, generic transport helpers, cancellation, wake-event delivery, and permission mediation.

#### Scenario: Plugin registers a direct provider runtime
- **WHEN** a plugin contributes an AI provider runtime for a direct HTTP provider
- **THEN** the runtime SHALL define provider-specific behavior while relying on host-owned services for secret lookup, asynchronous execution, and shell wake-up delivery

#### Scenario: Host rejects provider-owned workflow replacement
- **WHEN** a plugin attempts to replace the host-owned chat or inline-completion UI instead of contributing a provider runtime
- **THEN** the host SHALL reject that replacement and SHALL accept only the provider runtime contribution

### Requirement: Direct HTTP And SSE Are The Default Provider Path
For providers whose APIs are directly reachable from the host, MicroIDE SHALL prefer direct HTTP request execution and streaming response handling over spawning an extra binary. OpenAI, Anthropic Claude, and DeepSeek SHALL be supported through direct runtime adapters in the initial slice.

#### Scenario: OpenAI provider runs without a sidecar
- **WHEN** the user selects an OpenAI-backed provider
- **THEN** the request SHALL execute through a direct runtime adapter without requiring a bridge subprocess

#### Scenario: Claude provider streams directly
- **WHEN** the user selects a Claude-backed provider
- **THEN** the runtime SHALL consume the Anthropic streaming API directly and deliver typed response events back to the host

#### Scenario: DeepSeek uses the direct compatibility path
- **WHEN** the user selects a DeepSeek-backed provider
- **THEN** the runtime SHALL execute through a direct API adapter compatible with the host-owned AI runtime contract and SHALL NOT require a separate bridge binary

### Requirement: Sidecar Execution Is Optional
MicroIDE SHALL support sidecar or stdio-backed AI providers only as an optional runtime adapter strategy. Sidecar-backed providers SHALL implement the same host-owned AI runtime contract and SHALL NOT impose bridge-specific behavior on chat, inline completion, or tool approval callers.

#### Scenario: Sidecar-backed provider is installed
- **WHEN** a provider runtime is implemented through a sidecar adapter
- **THEN** the host SHALL treat it as one runtime implementation strategy behind the same generic AI runtime interface

#### Scenario: Direct and sidecar providers share caller behavior
- **WHEN** chat, inline completion, or tool approval flows interact with either a direct provider or a sidecar-backed provider
- **THEN** the caller-visible request lifecycle, cancellation model, and event-delivery contract SHALL remain the same

### Requirement: Provider Spec Declares Picker Metadata

Every registered AI provider SHALL declare picker metadata in its `AiProviderSpec`: `display_name` (required, non-empty), `default_model` (optional, must be present in `models` when set), `requires_api_key` (bool), and `auth_method` (enum: `none`/`api_key`/`oauth`/`sidecar`). The host picker overlay SHALL render providers using these fields verbatim and SHALL NOT inspect provider-internal state.

#### Scenario: Provider without display_name is rejected
- **WHEN** a plugin registers an AI provider whose `display_name` is empty
- **THEN** `AiProviderRegistry::Register` SHALL reject the registration and SHALL log a structured warning naming the offending plugin

#### Scenario: Picker uses declared metadata
- **WHEN** the picker overlay renders the provider list
- **THEN** each row SHALL render `display_name`, the current `auth_status`, and (when expanded) the model dropdown sourced from `Models()` or declared `model_options` — and SHALL NOT call into transport-specific APIs

### Requirement: Cycle-Provider Becomes A Keyboard Fallback Only

Click-to-cycle on the chat-sidebar provider rail SHALL be removed as a primary picker affordance. Cycling SHALL remain available as a keyboard accelerator (default `Ctrl+Shift+P` next, `Ctrl+Shift+Alt+P` previous), exposed through `WorkspaceCommandRegistry`, and SHALL NOT be the only path to provider selection.

#### Scenario: Click-to-cycle on rail is gone
- **WHEN** the user clicks on the chat-sidebar provider rail
- **THEN** the host SHALL open the AI provider picker overlay and SHALL NOT advance the active provider as a side effect of the click

#### Scenario: Keyboard accelerator still cycles
- **WHEN** the user presses `Ctrl+Shift+P`
- **THEN** the active provider SHALL advance to the next provider in `AiProviderRegistry`, identical to the current cycle behavior

### Requirement: Picker Surface Is Host-Owned

The AI provider picker overlay SHALL be host-owned, MUST live in `src/workspace/WorkspaceShellRenderSettings*.cpp` (shared with the Settings overlay), and SHALL NOT be replaceable by a plugin contribution. Plugins MAY add new provider runtimes through the existing registration API, which the host picker SHALL surface automatically.

#### Scenario: Plugin attempts to replace the picker
- **WHEN** a plugin contributes a UI surface that declares itself as a replacement provider picker
- **THEN** the host SHALL ignore that contribution and SHALL continue to render the host-owned picker

#### Scenario: Plugin-registered provider appears in the picker
- **WHEN** a plugin registers a new AI provider runtime through the existing API
- **THEN** the host picker SHALL include that provider on its next open without any host-side enumeration changes

