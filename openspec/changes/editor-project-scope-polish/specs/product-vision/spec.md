## MODIFIED Requirements

### Requirement: Durable Non-Goals

MicroIDE SHALL treat the following as out of scope unless deliberately promoted into a dedicated phase: full debugger UI beyond first-pass start/stop plus output-channel plumbing, plugin marketplaces and remote install flows, Micro-plugin compatibility, cloud or collaboration features, account systems and sync, recent-project and recent-file surfaces, and native OS menu integration.

#### Scenario: Feature request falls inside a non-goal
- **WHEN** a proposal requests a feature whose primary capability falls inside the non-goal list
- **THEN** the proposal SHALL be rejected or SHALL explicitly declare itself as promoting a non-goal into its own phase, with an updated product-vision delta in the same change

### Requirement: AI Is In Scope

The MicroIDE product SHALL treat AI workflows (chat, inline completion, MCP-backed tool use, provider-runtime-driven model access) as in-scope, host-owned first-class surfaces. The implementation guide and any durable docs SHALL NOT list AI or chat as a non-goal.

#### Scenario: Documentation lists AI as a non-goal
- **WHEN** any durable doc (implementation guide, roadmap, guidelines) is reviewed
- **THEN** it SHALL NOT list AI, chat, or inline completion as a non-goal, and any such entry SHALL be corrected in the same review
