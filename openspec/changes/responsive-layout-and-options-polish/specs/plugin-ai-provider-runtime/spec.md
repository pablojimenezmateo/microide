## ADDED Requirements

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
