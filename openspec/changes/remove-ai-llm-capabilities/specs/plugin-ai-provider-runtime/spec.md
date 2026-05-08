## REMOVED Requirements

### Requirement: Host-Owned AI Runtime Interface
**Reason**: Host no longer ships AI provider runtime capabilities.
**Migration**: Remove AI runtime interfaces and plugin wiring that target provider-backed AI workflows.

### Requirement: Provider-Specific Behavior Is Plugin-Owned
**Reason**: AI provider extension surface is removed.
**Migration**: Remove plugin provider contribution contracts and reject AI provider registration in host/plugin runtime.

### Requirement: Direct HTTP And SSE Are The Default Provider Path
**Reason**: Provider HTTP runtime behavior is removed with AI runtime retirement.
**Migration**: Remove direct provider HTTP/SSE execution paths and their dependency chain.

### Requirement: Sidecar Execution Is Optional
**Reason**: Sidecar provider execution is no longer a shipped workflow.
**Migration**: Remove provider sidecar bridge executable and bridge-management lifecycle in host.

### Requirement: Provider Spec Declares Picker Metadata
**Reason**: AI provider picker is removed from settings/status workflows.
**Migration**: Remove provider picker metadata from plugin/provider schemas.

### Requirement: Cycle-Provider Becomes A Keyboard Fallback Only
**Reason**: Provider cycling behavior is no longer applicable.
**Migration**: Remove provider-cycling command/action pathways.

### Requirement: Picker Surface Is Host-Owned
**Reason**: AI provider picker surface is removed with AI workflows.
**Migration**: Remove picker render/input/state integration and associated test coverage.
