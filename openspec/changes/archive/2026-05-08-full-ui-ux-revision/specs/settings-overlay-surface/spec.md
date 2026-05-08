## ADDED Requirements

### Requirement: Settings Guidance Copy Uses Accurate Semantics
Settings overlay helper text SHALL use copy labels that match message intent. Text that describes baseline behavior SHALL be rendered as neutral description (or `Note` where appropriate) and SHALL NOT be labeled as a `Tip`.

#### Scenario: Behavior text is not mislabeled as tip
- **WHEN** a settings row includes explanatory text that tells users how the control works by default
- **THEN** the overlay SHALL render that text without a `Tip` label (or with a neutral `Note` label) and SHALL reserve `Tip` for optional advice only

#### Scenario: Optional advice remains a tip
- **WHEN** helper text recommends an optional workflow optimization
- **THEN** the overlay SHALL label that text as `Tip`
