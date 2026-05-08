## REMOVED Requirements

### Requirement: Host-Owned Chat Pane
**Reason**: Built-in AI/LLM chat workflows are removed from product scope.
**Migration**: Remove chat sidebar/composer/transcript surfaces and related command/action/render/input paths.

### Requirement: Host-Owned Inline Completion
**Reason**: Built-in AI/LLM inline completion is removed from product scope.
**Migration**: Remove inline completion registries, request/accept/dismiss flows, and ghost-text rendering branches.

### Requirement: AI Provider Runtime Contract
**Reason**: Provider runtime and bridge-backed model integration are removed.
**Migration**: Remove provider runtime services, provider bridge target/protocol, and associated auth/model refresh UX.

### Requirement: Bounded AI Context Collection
**Reason**: No host-owned AI request pipeline remains.
**Migration**: Remove AI context collection managers and request-context assembly plumbing.

### Requirement: MCP Tool Permissions
**Reason**: AI-workflow-owned MCP invocation and approval surfaces are removed.
**Migration**: Remove MCP tool invocation/approval wiring and associated plugin contribution paths.

### Requirement: AI Surfaces Do Not Stall The Shell
**Reason**: AI surfaces are removed and no longer define shell-thread behavior contracts.
**Migration**: Remove AI-specific runtime event handling while preserving existing non-AI responsiveness invariants.
