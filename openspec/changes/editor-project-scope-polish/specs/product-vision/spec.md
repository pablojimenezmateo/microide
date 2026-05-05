## MODIFIED Requirements

### Requirement: Durable Non-Goals

MicroIDE SHALL treat the following as out of scope unless deliberately promoted into a dedicated phase: full debugger UI beyond first-pass start/stop plus output-channel plumbing, plugin marketplaces and remote install flows, Micro-plugin compatibility, cloud or collaboration features, account systems and sync, recent-project and recent-file surfaces, and native OS menu integration.

#### Scenario: Feature request falls inside a non-goal
- **WHEN** a proposal requests a feature whose primary capability falls inside the non-goal list
- **THEN** the proposal SHALL be rejected or SHALL explicitly declare itself as promoting a non-goal into its own phase, with an updated product-vision delta in the same change
