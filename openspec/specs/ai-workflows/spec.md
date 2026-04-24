### Requirement: Host-Owned Chat Pane

MicroIDE SHALL provide a host-owned chat pane with a conversation rail, provider and model selector, per-conversation multiline draft composer with retention, markdown transcript rendering with safe local-link opening and confirmed remote-link opening, and project-tab chat status summaries. The chat pane SHALL NOT be replaceable by a plugin.

#### Scenario: Switching conversations preserves drafts
- **WHEN** the user types a draft in conversation A, switches to conversation B, and switches back to A
- **THEN** the draft text for conversation A SHALL be preserved exactly as typed

#### Scenario: Markdown transcript renders a remote link
- **WHEN** a transcript message contains an `https://` link and the user activates it
- **THEN** MicroIDE SHALL prompt for confirmation before opening the link, and SHALL open local-path links without a remote-confirmation prompt

### Requirement: Host-Owned Inline Completion

MicroIDE SHALL provide host-owned ghost-text inline completion in the editor, with explicit accept and dismiss actions, driven through the AI provider bridge. Inline completion SHALL NOT block typing, SHALL cancel in-flight requests when the caret moves or the buffer changes, and SHALL degrade silently when a provider is unavailable.

#### Scenario: Typing during a pending inline request
- **WHEN** an inline completion request is in flight and the user types a new character
- **THEN** the in-flight request SHALL be cancelled, the ghost text SHALL be cleared, and a new request MAY be issued without blocking the editor frame

#### Scenario: Provider unavailable
- **WHEN** the configured inline-completion provider is unreachable or unauthenticated
- **THEN** the editor SHALL continue to accept input normally, SHALL NOT display a modal error, and SHALL route the failure to the host-owned output channel for the provider

### Requirement: Provider Bridge Contract

AI providers SHALL be reachable through the host-owned `WorkspaceProviderBridge` contract, which SHALL support stdio-backed long-lived provider bridges and SHALL accept HTTP and ACP-compatible transports as additive options without changing the contract seen by chat and inline-completion callers.

#### Scenario: Adding an HTTP provider
- **WHEN** a new HTTP-backed provider is added
- **THEN** it SHALL implement the same provider-bridge contract used by stdio providers, and chat and inline-completion code paths SHALL consume it without branching on transport

#### Scenario: Provider bridge crashes
- **WHEN** a long-lived stdio provider bridge exits unexpectedly during a chat or inline-completion request
- **THEN** the host SHALL surface the failure to the provider's output channel, mark pending requests as failed, and allow the next request to relaunch the bridge without restarting MicroIDE

### Requirement: Bounded AI Context Collection

`WorkspaceAiContext` SHALL collect chat and inline-completion context with enforced limits on total size and file count, and SHALL prioritize the current file, current selection, and current diagnostics above broader project context. Context collection SHALL NOT read files excluded by `.gitignore` unless the user has explicitly opted in.

#### Scenario: Context exceeds size budget
- **WHEN** the raw context payload for a chat request would exceed the configured size limit
- **THEN** `WorkspaceAiContext` SHALL trim lower-priority sources first (project-wide snippets before diagnostics, diagnostics before selection, selection before current file) and SHALL record the trim decision for traceability

#### Scenario: Gitignored file is in the viewport
- **WHEN** the current file is listed in `.gitignore` and the user has not enabled "include ignored files in AI context"
- **THEN** `WorkspaceAiContext` SHALL NOT include the file's contents in the outgoing request

### Requirement: MCP Tool Permissions

MCP tool invocations SHALL pass through host-owned permission checks with per-agent permission levels (denied, prompt-required, allowed-within-context, allowed), and the chat pane SHALL surface prompt-required invocations as a project-scoped approval flow with session-scoped remembered approvals. Tool events SHALL be persisted to the conversation transcript.

#### Scenario: Prompt-required tool
- **WHEN** an agent invokes an MCP tool whose permission level is `prompt-required`
- **THEN** the chat pane SHALL prompt the user, and a session-scoped "remember this choice" option SHALL apply the approval to further invocations within the same session only

#### Scenario: Denied tool
- **WHEN** an agent invokes an MCP tool whose permission level is `denied`
- **THEN** the invocation SHALL fail immediately without prompting and the failure SHALL be recorded in the conversation transcript and the tool's output channel

### Requirement: AI Surfaces Do Not Stall The Shell

Chat, inline completion, MCP tool execution, and provider-bridge startup SHALL NOT stall render, typing, scrolling, or input handling. All AI work SHALL be delivered back to the shell through existing SDL wake-event routing.

#### Scenario: Slow model response
- **WHEN** a chat provider takes several seconds to stream a response
- **THEN** MicroIDE SHALL continue to accept input, repaint on demand, and remain responsive to unrelated sidebar, editor, or terminal interactions, with the pending response delivered as it arrives
