## ADDED Requirements

### Requirement: Patch Application Is A Narrow Service Boundary
Workspace hunk and line patch operations SHALL route through a `PatchApplyService` or equivalent host-owned service. Sidebar, compare, merge, and command handlers SHALL NOT construct shell commands or mutate index/worktree state directly for patch operations.

#### Scenario: Selected-line staging from compare
- **WHEN** a compare interaction requests selected-line staging
- **THEN** the workspace coordinator SHALL call the patch application service with typed diff-selection data and SHALL NOT run a subprocess from the coordinator itself

#### Scenario: Patch operation completes
- **WHEN** a patch operation completes successfully or fails
- **THEN** the owning service SHALL publish a structured result and request repository snapshot refresh through the Git repository service
