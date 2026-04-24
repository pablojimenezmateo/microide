# Chat Pane Plan

## Goal

Replace the current first-pass chat sidebar with a durable, provider-agnostic chat pane that owns:

- conversation history, navigation, and drafts
- project-scoped independent execution across multiple open projects
- project-tab level compact agent status signaling
- request lifecycle, cancellation, and restore rules
- basic markdown rendering and safe link activation
- multiline message composition
- model and provider selection
- secure API-key-backed provider setup for supported cloud providers
- tool permission policy and approval UX
- per-request timing, status, and error reporting
- host-owned context-window management

The pane must stay host-owned. Providers should plug into it through the existing external-agent,
AI runtime, AI context, and tool registries instead of forcing the host to adopt a Codex-specific
UI or protocol.

## Product Requirements

The chat pane should support all of the following inside the pane itself:

1. browse existing conversations
2. create a new conversation
3. delete a conversation
4. switch the active conversation
5. select the provider and model for the active conversation
6. compose multiline prompts with per-conversation draft retention
7. render assistant replies with basic markdown
8. open safe local file links and remote links from the transcript
9. show request state, elapsed time, and terminal errors
10. cancel and retry requests, with regenerate defined before it is exposed
11. control whether the agent may run tools, with a workflow similar to Codex CLI
12. remain usable when providers, models, or restored session data are missing or degraded
13. allow multiple open projects to use chat concurrently without one project blocking another
14. surface per-project agent activity in the project tab chrome without opening the pane
15. configure and validate API-key-based access for the supported cloud providers

## Design Constraints

- The host owns chat state, rendering, persistence, permissions, navigation, request lifecycle, and
  context budgeting.
- Project tabs own independent chat state, drafts, approvals, and request execution.
- Project-tab chrome should show a host-derived compact agent state summary for that project.
- Providers remain external-agent registrations. The pane must not hardcode Codex semantics.
- The first supported direct cloud providers are OpenAI and Anthropic.
- Authentication for those providers is API-key-only in this phase. OAuth, device-code login, and
  CLI-auth scraping are out of scope.
- Core provider transport must not depend on Lua-embedded Python, shell wrappers, or vendor CLIs.
- Tool permissions must be capability-scoped and host-enforced.
- Conversation state must survive plugin reloads and normal session restore.
- Non-terminal requests must not silently survive reload or shutdown as if the transport were still
  alive.
- The pane should remain usable even when no provider is configured.
- Markdown, links, tool arguments, and tool outputs are untrusted input.

## High-Level UI

The chat pane should be split into three vertical regions:

1. Conversation rail
   - shows saved conversations
   - includes `New`, `Delete`, and simple metadata such as title, status, and last-updated time
   - keeps the current conversation selected
   - shows background running or failed state for non-active conversations

2. Conversation surface
   - header with provider, model, tool mode, and current status
   - transcript with markdown-aware rendering
   - request timing summary for the active reply
   - inline tool activity and approval results

3. Composer
   - multiline text area
   - explicit send and cancel affordances
   - compact hint row describing enter behavior, tool mode, and provider or model state

This stays inside the existing chat sidebar mode rather than reopening a bottom-panel path.

### Project tab chrome

The project tab itself should expose compact agent state so developers can see background work
without switching projects.

- show a small colored status mark next to the project name inside the project tab
- a triangular glyph is a reasonable default shape because it is compact and visually distinct from
  dirty markers or accent-color treatments
- the marker is derived from project-owned chat state, not plugin-owned rendering
- hover should show a compact full-status summary for that project's active or most recent
  chat activity

The compact marker should summarize project-level agent state such as:

- no marker when no request is active and no recent terminal issue needs attention
- running or streaming request: active-color marker
- tool approval pending: warning-color marker
- failed or interrupted latest request: error-color marker until the failure is acknowledged or a
  later request supersedes it

The hover summary should include, when available:

- conversation title
- provider or model label
- current lifecycle state
- elapsed time
- whether a tool approval is pending

## Provider Model

The pane should be provider-agnostic.

- `ExternalAgentSpec` remains the transport-level registration point.
- The chat pane selects from registered chat-capable agents.
- Model selection is stored in host state, not buried in provider-specific plugin config.
- Provider capability metadata should be explicit instead of inferred from one happy-path transport.
- The first mature cloud-provider path should target OpenAI and Anthropic directly rather than
  shelling out to Codex or ad hoc scripts.

The pane should depend on the following provider-facing data:

- provider id
- provider label
- optional model list
- chat capability
- streaming support
- tool-call support
- system-prompt support
- structured-output support
- attachment or image support
- model-enumeration support

When a capability is absent, the pane should degrade by disabling or hiding that behavior instead
of pretending every provider is "chat plus maybe tools."

Codex can remain the default repo plugin, but the pane must work equally for other providers later.

### Provider integration strategy

The correct implementation split is:

- host owns UI, conversation state, approvals, persistence, and secret storage
- plugin owns provider registration and packages the transport implementation
- a plugin-packaged native bridge process owns HTTP, retries, streaming decode, and provider-specific
  request formatting

This phase should support OpenAI and Anthropic only.

For those providers:

- authentication is API-key-only
- API keys are user-scoped secrets owned by the host and never persisted in project session data
- the pane should provide provider setup or status UI, but raw secret handling stays out of
  transcript state and logs
- provider plugins should not read or write vendor-specific auth files such as `~/.codex/auth.json`
  or invent their own unmanaged token stores

The bridge contract should be structured and long-lived:

- launch via structured argv, not shell-joined command strings
- keep the provider bridge process alive across multiple requests instead of spawning one command
  per turn
- speak a structured stdio protocol or ACP-shaped protocol that supports initialize, auth status,
  model enumeration, start request, stream events, cancel request, and capability reporting
- allow the host to pass credentials and per-request metadata in structured form rather than by
  scraping environment-specific files

This keeps vendor networking out of the shell core without forcing mature provider integrations to
be written as Lua scripts plus subprocess glue.

### Credentials

The host should store provider credentials in host-managed secret storage keyed by provider.

- OpenAI and Anthropic keys should be treated as user-level credentials, not project settings
- project state may choose provider and model, but it must not contain raw API keys
- provider setup UI should support set, replace, clear, and basic validation flows
- the current non-keychain secret store is acceptable as an interim step, but the durable direction
  is an OS-backed credential store

## Conversation Model

The current `Conversation` and `Message` structs are too narrow for the intended pane. The durable
shape should include:

### Conversation

- `id`
- `title`
- `schema_version`
- `provider_id`
- `model_id`
- `status`
- `tool_mode`
- `draft`
- `system_prompt`
- `created_at`
- `updated_at`
- `last_request_duration_ms`
- `messages`

### Message

- `id`
- `role`
- `content`
- `status`
- `timestamp`
- `provider_id`
- `model_id`
- `request_duration_ms`
- `error`
- `tool_events`
- `attachments` or `references` placeholder
- `stream_id`
- cached parsed markdown layout keyed by content hash if needed

### Tool Event

- `tool_id`
- `display_name`
- `arguments_summary`
- `status`
- `permission_decision`
- `capability_scope`
- `started_at`
- `finished_at`
- `duration_ms`
- `error`
- `output_summary`

The host should stop treating the assistant reply as only one collapsed text line.

## Persistence And Migration

Chat history must move into persisted project session state.

Each project's conversations, drafts, request state, and approval state are independent from every
other open project.

The session file should store:

- active conversation id
- all saved conversations
- per-conversation provider and model selection
- per-conversation tool mode
- per-conversation draft text
- per-conversation system prompt or host instruction slot
- timing, status, and terminal error metadata

Persisted chat data must carry an explicit `schema_version`.

Migration rules must be defined up front:

- old chat payloads migrate before they become live host state
- added fields should get deterministic defaults instead of ad hoc null handling
- removed providers do not delete conversations; the conversation stays readable and is marked
  unavailable
- removed models do not get silently swapped; the stored `model_id` is preserved and the next send
  requires reselection or explicit fallback
- corrupted chat payloads should fail closed to an empty chat state with a visible warning instead
  of crashing restore or corrupting unrelated workspace state

Plugin reload must no longer clear conversations.
Plugin reload must also not discard host-owned provider credentials.

## Operational Semantics

The chat pane needs explicit request and restore rules, not just UI layout.

### Request lifecycle

The host-owned state machine should be:

- `idle`
- `queued`
- `running`
- `streaming`
- `succeeded`
- `failed`
- `cancelled`

`Conversation.status` reflects the latest request state for the thread. `Message.status` reflects
the lifecycle of that message's contribution.

### Concurrency

- Each conversation allows at most one in-flight request.
- Each project may run chat requests independently from other open projects.
- The runtime must not use one workspace-global active-request slot for chat. It needs request
  tracking keyed by project plus conversation plus request id so two project tabs can talk to the
  same or different agents at the same time.
- A send snapshots provider, model, tool mode, system prompt, and context policy into the request.
  Mid-flight header edits affect only the next request.

### Project-level status summary

Project tab chrome needs a derived status summary for compact display.

- The project-level summary is derived from that project's conversations and pending approvals.
- If multiple conversations are active in one project, the tab should show the highest-priority
  state instead of trying to render multiple badges.
- Suggested priority order: approval pending, failed or interrupted attention state, running or
  streaming, queued, idle.
- Hover text may mention more than one active conversation when needed, but the tab glyph stays
  singular and compact.

### User actions during a request

- `Cancel` is required while a request is non-terminal.
- `Retry` is required for failed or cancelled requests and should resubmit the same user turn as a
  new assistant attempt without rewriting history.
- `Regenerate` is optional in phase 1. If exposed before branching lands, it should mean "retry the
  latest user turn while preserving the previous assistant reply."
- Switching conversations during a running request does not cancel it. The request remains attached
  to its source conversation, and the rail shows that conversation as running.
- Switching project tabs during a running request does not cancel it. The request remains attached
  to its source project and conversation, and that project tab should surface running state when
  revisited.
- Deleting the active conversation while it is running must cancel the request first, record the
  cancellation, then remove the conversation.
- Changing provider or model mid-flight never mutates the active request.

### Restore behavior

- Reload or shutdown during `queued`, `running`, or `streaming` does not attempt transparent
  resumption.
- On restore, any non-terminal request is converted into a terminal failed state with an explicit
  interruption error such as "Interrupted by reload or shutdown."
- Provider reload that invalidates the active agent follows the same rule: terminate the request,
  mark it failed, and leave the transcript intact.
- Those restore rules apply per project. Interruption in one project must not corrupt or erase chat
  state for another project.

## Context Management

The host must own context-window management instead of leaving it implicit in provider plugins.

- Request construction should flow through the host AI context service, not through ad hoc prompt
  concatenation in the pane.
- Context assembly order should be explicit: system prompt, durable conversation summary if any,
  retained recent turns, current user message, and then opt-in workspace context such as selection,
  current file, diagnostics, or SCM data.
- Provider or model-specific context limits should be stored in host metadata when known.
- When limits are unknown, the host should use a conservative default budget rather than sending the
  whole transcript.
- First pass may use deterministic truncation with a visible "older context omitted" marker.
- Later summarization is acceptable, but it must stay host-owned and explicit in conversation state
  rather than happening invisibly inside a provider adapter.
- Context trimming changes execution input only. It must not rewrite or delete persisted transcript
  history.
- Request payload assembly for OpenAI and Anthropic should be done inside the native provider bridge,
  not by shelling out to generic CLI wrappers.

## Markdown Support

The first implementation only needs basic markdown, but it should be structured so richer support
can be added later.

### Required first-pass markdown

- paragraphs
- line breaks
- headings
- unordered and ordered lists
- block quotes
- fenced code blocks
- inline code
- emphasis and strong emphasis
- markdown links

### Rendering approach

The host should parse messages into lightweight blocks and inline spans, then render those spans
with existing text primitives. Do not treat markdown as plain wrapped strings.

Code blocks should render with:

- distinct background
- monospace text path already used elsewhere
- optional syntax highlighting later

Malformed markdown should fall back to literal text rendering instead of breaking layout or
crashing the pane. Arbitrary HTML remains out of scope.

## Link And Content Security

Links in chat replies should be clickable, but the host must treat them as untrusted content.

### Supported link classes

- remote `https://...` and `http://...` URLs
- `file://...` URIs
- markdown links pointing at relative project paths
- inline local references such as `src/foo.cpp:12`

### Activation rules

- only `http`, `https`, and `file` schemes are allowed
- dangerous schemes such as `javascript:`, `data:`, or shell-like pseudo-links are rejected
- local links resolve against the project root when relative
- normalized relative paths that escape the project root are rejected
- validated local file links open directly and move the cursor to the requested line and column if
  present
- remote links should route through the host URL opener only after explicit confirmation

Tool arguments and tool outputs may contain secrets, tokens, or hostile text. The pane should
store and render redacted summaries by default, with raw payload access left to dedicated tooling
or logs if needed later.

Provider credentials must never appear in transcript state, approval prompts, output channels, or
bridge logs. Validation and transport errors should be redacted before rendering.

The chat pane should track hit regions per rendered span so clicks are precise rather than relying
on raw text matching after render.

## Multiline Composer

The current shared single-line text state is not sufficient.

The chat composer should become a true multiline surface with:

- newline insertion
- multiline cursor movement
- selection
- paste
- IME-friendly text entry
- explicit send behavior

Recommended behavior:

- `Enter` inserts a newline
- `Ctrl+Enter` sends
- `Escape` returns focus to the editor when appropriate

If a future platform-specific convention is needed, it can be layered later, but the internal
model should already be multiline.

## Tool Permission Model

The chat pane needs a host-owned tool policy similar in spirit to Codex CLI without binding the
implementation to Codex.

### Required modes

1. `No Tools`
   - the agent is not allowed to invoke tools

2. `Ask`
   - each tool request requires an explicit host confirmation

3. `Auto`
   - the agent may use tools allowed by current host policy without prompting each time

The active mode should be visible and changeable in the pane header.

### Enforcement

- The host checks tool calls against `McpToolRegistry`
- The pane-level mode further constrains those calls
- Provider responses that request tools must flow through host permission checks

This means:

- `Denied` stays denied
- `PromptRequired` prompts in `Ask`, rejects in `No Tools`, and may still prompt or reject in
  `Auto` depending on host policy
- `AllowedWithinContext` and `Allowed` can proceed in `Auto`

### Approval UX

Tool approval prompts should be surfaced by the host, but the resulting decision and tool activity
should remain visible in the conversation transcript.

The UX should define the following from day one:

- prompts show tool name, capability scope, and a redacted arguments summary
- approvals may be remembered for the current session within the approved capability scope
- remembered approvals are scoped to the current project and capability scope, not shared across
  all open projects by default
- remembered approvals are never persisted with conversation history
- pending approvals expire when the request is cancelled or after a host-defined timeout
- denied or expired approvals render as explicit tool events, not silent failures
- cancelling a chat request also cancels the active tool request when the runtime can stop it;
  otherwise late tool output is ignored and the transcript records cancellation

## Timing And Status

Each chat request should record:

- start time
- finish time
- elapsed duration
- final status

The pane should show:

- current in-flight status
- elapsed time for the latest reply
- per-message timing when useful
- terminal error summaries for failed or interrupted requests
- project-tab compact state consistent with the same underlying lifecycle data

This should be stored in conversation state instead of being only transient UI text.

## Error And Empty States

The host behavior should be explicit for the common failure cases:

- no providers configured: show an empty state with setup guidance and keep transcript browsing
  available
- provider exists but no API key is configured: keep the conversation readable and block send with
  setup guidance
- provider key is invalid or revoked: record a clear auth failure state without exposing the secret
- model enumeration fails: preserve the last-known model selection and surface retry guidance
- restored conversation whose provider no longer exists: keep the conversation readable and mark it
  unavailable
- restored `model_id` no longer offered: preserve it as stale and require user action before the
  next send
- provider transport failure: record a failed assistant message with retry affordance
- malformed markdown: render as literal text rather than failing the pane
- corrupted session data: discard only the invalid chat payload, log a warning, and keep the rest
  of workspace restore working

## Accessibility And Keyboard Behavior

The pane should define focus and keyboard rules explicitly:

- focus order: conversation rail, header controls, transcript, composer, then approval prompt
- `Tab` and `Shift+Tab` move predictably across those regions
- transcript items, tool approvals, and header controls need visible focus states and text labels
- approval prompts should take modal focus until resolved or cancelled
- high-contrast themes must preserve selection, link, error, and running-state visibility
- project-tab hover status also needs a keyboard-reachable or screen-reader-visible equivalent; the
  information must not exist only on pointer hover

## Performance

The pane needs performance rules before transcripts get large:

- large transcripts should use virtualization instead of laying out every message every frame
- parsed markdown should be cached by content hash instead of ad hoc mutable state
- link hit testing should be derived from rendered spans, not re-parsed raw strings
- scroll, typing, resize, and request-status updates must stay responsive with long histories
- profile long-transcript redraw and interaction paths with `docs/runtime-profiling.md` instead of
  guessing

## Runtime Direction

The current AI runtime only returns one final stdout chunk. That is acceptable for a first pass of
the pane work, but the design should anticipate streamed updates and structured tool events.

The durable direction is:

- request lifecycle owned by the host runtime
- provider transport handled by external agents
- context construction owned by the host AI context service
- cloud-provider HTTP handled by a plugin-packaged native bridge rather than shell or Python glue
- request execution isolated by project workspace state rather than one shell-global chat lane
- structured reply updates mapped onto conversation messages
- optional streamed partial assistant output
- optional structured tool-call events

This work should extend the existing host-owned AI seams instead of creating a chat-only side path.

## Implementation Phases

### Phase 1: Durable schema and semantics

- extend conversation, message, and tool-event models
- add request-state, timing, error, draft, and system-prompt fields
- persist conversations in project session state with explicit schema versioning
- define migration behavior and restore behavior for non-terminal requests
- stop clearing chat history on plugin reload
- make chat runtime ownership explicitly project-scoped instead of shell-global
- thread host-owned context budgeting through request construction

### Phase 2: Provider bridge and credentials

- scope the first mature cloud integrations to OpenAI and Anthropic
- add host-owned API key setup and storage for those providers
- replace shell-joined external-agent endpoints with structured bridge launch specs
- add a plugin-packaged native provider bridge with long-lived stdio or ACP-shaped transport
- add auth status, provider capability negotiation, and model enumeration through that bridge
- remove dependence on Lua-embedded Python, shell wrappers, and Codex CLI for the primary provider path

### Phase 3: Pane shell

- add conversation rail and header controls
- add create, delete, switch, cancel, and retry actions
- add provider, model, and tool-mode controls
- add provider setup, missing-key, and invalid-key states
- add multiline composer state, rendering, and per-conversation draft retention
- add project-tab compact agent status indicator plus hover summary
- define keyboard focus order and approval-prompt focus handling

### Phase 4: Transcript rendering and security

- replace line-based transcript rendering with markdown block rendering
- add content-hash-based markdown caching
- add precise hit-testing for remote and local links
- add path normalization, URI validation, and remote-link confirmation
- show timing, state, and tool activity in transcript metadata

### Phase 5: Runtime and tool policy plumbing

Shipped in the current tree:

- thread pane-level tool mode through chat request execution
- add host-side tool-call approval flow with timeout and session-scoped remembered approvals
- record tool activity in conversation state
- snapshot provider, model, and context policy at send time
- make request cancellation explicit in both runtime and UI state
- support concurrent chat requests across multiple open projects

### Phase 6: Provider capability negotiation and degraded states

- make model lists come from provider data where available
- surface provider capability differences explicitly in the pane
- improve empty-state and error-state behavior for missing providers or models
- keep non-streaming providers functional without special-case UI forks

### Phase 7: Streaming and scale polish

- add streamed updates when the runtime transport supports them
- add richer tool-event rendering
- add virtualization tuning for very large transcripts
- add host-owned summarization only if deterministic truncation is no longer sufficient

## Testing Requirements

Implementation should add coverage for:

- session restore of conversations
- schema migration of old conversations
- corrupted chat-session payload handling
- plugin reload preserving conversations
- plugin reload preserving host-owned provider credentials and status
- creating, deleting, and switching conversations
- setting, replacing, and clearing provider API keys
- missing or invalid API key states for OpenAI and Anthropic
- provider and model selection persistence
- model enumeration and capability fetch through the native provider bridge
- missing provider and missing model restore behavior
- request lifecycle transitions, including cancel and retry
- concurrent requests across two open projects without cross-talk or shared cancellation
- project-tab compact indicator and hover summary for running, approval-pending, and failed states
- reload or shutdown turning non-terminal requests into interrupted failures
- tool-mode persistence and enforcement
- approval timeout, denial, and remembered-session approval behavior
- multiline composer editing, draft retention, and send behavior
- markdown paragraph, list, code, and inline formatting layout
- malformed markdown fallback behavior
- clickable remote links with scheme validation and confirmation
- clickable local file links with path normalization and line navigation
- context-budget trimming behavior
- elapsed-time and terminal-status updates

## Non-Goals For The First Pass

- full CommonMark compliance
- image rendering in markdown
- arbitrary HTML rendering
- provider-specific bespoke controls
- hardcoded Codex-only workflows
- transparent request resume after reload or restart

## Summary

The correct shape is a host-owned, persisted, provider-agnostic chat pane with structured
conversation state and explicit operational semantics. Markdown rendering, link activation,
multiline composition, model selection, conversation management, request lifecycle, context-window
policy, and tool permission flow should all live in that pane instead of being spread across
transient shell state or provider-specific plugins.
