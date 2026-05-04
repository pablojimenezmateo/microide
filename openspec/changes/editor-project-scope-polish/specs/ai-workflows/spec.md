## MODIFIED Requirements

### Requirement: Host-Owned Inline Completion

MicroIDE SHALL provide host-owned ghost-text inline completion in the editor, with explicit accept and dismiss actions, driven through the host-owned AI provider runtime. Inline completion SHALL NOT block typing, SHALL cancel in-flight requests when the caret moves or the buffer changes, and SHALL degrade silently when a provider is unavailable.

#### Scenario: Typing during a pending inline request
- **WHEN** an inline completion request is in flight and the user types a new character
- **THEN** the in-flight request SHALL be cancelled, the ghost text SHALL be cleared, and a new request MAY be issued without blocking the editor frame

#### Scenario: Provider unavailable
- **WHEN** the configured inline-completion provider is unreachable or unauthenticated
- **THEN** the editor SHALL continue to accept input normally, SHALL NOT display a modal error, and SHALL route the failure to the host-owned output channel for the provider

### Requirement: Provider Bridge Contract

AI providers SHALL be reachable through a host-owned provider runtime contract. Direct API-backed providers SHALL NOT require a bridge subprocess. Stdio-backed or sidecar-backed provider adapters SHALL remain supported as an implementation option, and HTTP- and ACP-compatible transports SHALL be additive options without changing the contract seen by chat and inline-completion callers.

#### Scenario: Adding a direct provider
- **WHEN** a new direct HTTP-backed provider is added
- **THEN** it SHALL implement the same runtime contract consumed by chat and inline completion, and the host SHALL NOT require a bridge subprocess for that provider

#### Scenario: Sidecar-backed provider crashes
- **WHEN** a sidecar-backed provider exits unexpectedly during a chat or inline-completion request
- **THEN** the host SHALL surface the failure to the provider's output channel, mark pending requests as failed, and allow the next request to relaunch the adapter without restarting MicroIDE

### Requirement: AI Surfaces Do Not Stall The Shell

Chat, inline completion, MCP tool execution, and provider-runtime startup SHALL NOT stall render, typing, scrolling, or input handling. All AI work SHALL be delivered back to the shell through existing SDL wake-event routing.

#### Scenario: Slow model response
- **WHEN** a chat provider takes several seconds to stream a response
- **THEN** MicroIDE SHALL continue to accept input, repaint on demand, and remain responsive to unrelated sidebar, editor, or terminal interactions, with the pending response delivered as it arrives
