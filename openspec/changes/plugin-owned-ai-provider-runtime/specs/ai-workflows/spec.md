## ADDED Requirements

### Requirement: AI Provider Runtime Contract
MicroIDE SHALL route chat, inline completion, and provider-facing tool-call round-trips through a host-owned `AiProviderRuntime` contract. The default provider path SHALL support direct HTTP request execution plus streaming delivery, and sidecar execution SHALL be treated as an optional adapter behind the same contract rather than a caller-visible architecture.

#### Scenario: Adding a direct HTTP provider
- **WHEN** a new OpenAI-compatible or Anthropic-style provider is added
- **THEN** it SHALL implement the AI provider runtime contract without requiring a stdio bridge binary, and chat and inline-completion callers SHALL consume it without transport-specific branching

#### Scenario: Optional sidecar provider
- **WHEN** a provider is implemented through a sidecar for isolation or legacy reasons
- **THEN** the sidecar adapter SHALL expose the same runtime lifecycle, cancellation, streaming, and error semantics as a direct provider

## MODIFIED Requirements

### Requirement: Host-Owned Inline Completion

MicroIDE SHALL provide host-owned ghost-text inline completion in the editor, with explicit accept and dismiss actions, driven through the host-owned AI provider runtime. Inline completion SHALL NOT block typing, SHALL cancel in-flight requests when the caret moves or the buffer changes, and SHALL degrade silently when a provider is unavailable.

#### Scenario: Typing during a pending inline request
- **WHEN** an inline completion request is in flight and the user types a new character
- **THEN** the in-flight request SHALL be cancelled, the ghost text SHALL be cleared, and a new request MAY be issued without blocking the editor frame

#### Scenario: Provider unavailable
- **WHEN** the configured inline-completion provider is unreachable or unauthenticated
- **THEN** the editor SHALL continue to accept input normally, SHALL NOT display a modal error, and SHALL route the failure to the host-owned output channel for the provider

### Requirement: AI Surfaces Do Not Stall The Shell

Chat, inline completion, MCP tool execution, provider-runtime startup, and provider streaming or cancellation work SHALL NOT stall render, typing, scrolling, or input handling. All AI work SHALL be delivered back to the shell through existing SDL wake-event routing.

#### Scenario: Slow model response
- **WHEN** a chat provider takes several seconds to stream a response
- **THEN** MicroIDE SHALL continue to accept input, repaint on demand, and remain responsive to unrelated sidebar, editor, or terminal interactions, with the pending response delivered as it arrives

#### Scenario: Provider metadata refresh is slow
- **WHEN** a provider runtime takes several seconds to refresh auth state or enumerate models
- **THEN** the shell SHALL remain responsive, and any status or model-list updates SHALL arrive asynchronously through the same wake-event routing used by other AI work

## REMOVED Requirements

### Requirement: Provider Bridge Contract
**Reason**: The durable contract is no longer a bridge-specific interface. OpenAI, Claude, and DeepSeek are first-class direct HTTP and streaming providers, and sidecar execution is an optional adapter instead of the baseline architecture.

**Migration**: Route chat, inline completion, auth status, model discovery, streaming events, tool-call round-trips, and cancellation through `AiProviderRuntime` and `AiProviderRuntimeService`. If a provider still requires a subprocess, implement it as a sidecar-backed runtime adapter rather than a caller-visible bridge contract.
