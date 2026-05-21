## MODIFIED Requirements

### Requirement: Durable Non-Goals

MicroIDE SHALL treat the following as out of scope unless deliberately promoted into a dedicated phase: debugger/DAP support, plugin marketplaces and remote install flows, Micro-plugin compatibility, broad plugin security-system hardening work (including plugin sandboxing, per-plugin capability prompts, plugin signing, and plugin marketplace trust), cloud or collaboration features, account systems and sync, recent-project and recent-file surfaces, and native OS menu integration.

Minimal startup recovery and trust controls for the Git Workstation Preview, specifically `--disable-plugins`, `--safe-mode`, visible plugins-disabled state, and documentation that repo-local plugin code is not loaded by default, SHALL be considered in scope for that preview and SHALL NOT imply a plugin sandbox or marketplace trust model.

#### Scenario: Feature request falls inside a non-goal
- **WHEN** a proposal requests a feature whose primary capability falls inside the non-goal list
- **THEN** the proposal SHALL be rejected or SHALL explicitly declare itself as promoting a non-goal into its own phase, with an updated product-vision delta in the same change

#### Scenario: Safe-mode request is scoped to preview trust
- **WHEN** a proposal adds startup flags that disable plugins for recovery or untrusted-repository inspection
- **THEN** the proposal MAY proceed as part of the Git Workstation Preview scope, provided it does not add plugin sandboxing, capability prompts, marketplace, remote install, or project-local plugin loading
