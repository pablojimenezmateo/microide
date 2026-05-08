## ADDED Requirements

### Requirement: Hotspot-Prone Workspace Paths Expose Measurable Seams
Workspace subsystems that repeatedly appear in hotspot audits SHALL expose deterministic measurement seams through existing services, builders, or harness hooks rather than embedding opaque work in event-loop glue or render translation units.

#### Scenario: Hotspot found in opaque shell-thread path
- **WHEN** the audit identifies a hotspot that cannot be isolated by current harness instrumentation
- **THEN** the owning subsystem SHALL add a narrow measurable seam so the path can be validated by automated perf scenarios without broad shell coupling

### Requirement: Optimization Refactors Preserve Ownership Boundaries
Performance-motivated workspace refactors SHALL preserve service ownership and coordinator boundaries, and SHALL NOT reintroduce broad `WorkspaceShell` reach-through to reduce short-term code churn.

#### Scenario: Optimization proposes shell reach-through shortcut
- **WHEN** an optimization change attempts to bypass a service boundary by mutating state directly through shell internals
- **THEN** the change SHALL be rejected in favor of a service-level API that keeps the path measurable and testable
