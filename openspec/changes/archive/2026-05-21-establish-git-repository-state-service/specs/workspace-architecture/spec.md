## ADDED Requirements

### Requirement: Git Repository State Is Service-Owned
Workspace repository state and Git operations SHALL be owned by a `GitRepositoryService` or equivalently narrow host-owned service. Workspace coordinators and render code SHALL NOT own independent Git status maps, call Git directly, or mutate repository snapshot state outside the service API.

#### Scenario: Coordinator needs repository status
- **WHEN** a workspace coordinator needs branch, status, conflict, staged, unstaged, untracked, or outgoing repository state
- **THEN** it SHALL request a `GitRepositoryState` snapshot or operation through the Git repository service and SHALL NOT run a subprocess directly

#### Scenario: Render surface displays Git status
- **WHEN** a render translation unit displays branch, cleanliness, stale, refresh, or error state
- **THEN** it SHALL consume prebuilt view-model data derived from the Git repository service and SHALL NOT read the service or project state directly during paint

### Requirement: Git Operations Stay Off Workspace Hot Paths
Workspace code SHALL NOT synchronously wait for Git subprocess output on render, input, activation, or per-frame preparation paths. Git operation results SHALL be delivered asynchronously and ignored when their snapshot generation is stale.

#### Scenario: User opens the Git sidebar
- **WHEN** the user opens or refreshes the Git sidebar
- **THEN** the workspace SHALL render immediately with the current snapshot or a loading state, while any required Git subprocess work runs in the background
