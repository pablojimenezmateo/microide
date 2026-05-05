## ADDED Requirements

### Requirement: Low-Contrast Diff Decorations Preserve Text Legibility

Compare and merge surfaces SHALL use low-contrast fill colors for added, removed, and conflicted rows. Foreground text color SHALL remain neutral and SHALL NOT inherit a red, green, or orange tint from the row decoration.

#### Scenario: Added row keeps neutral text
- **WHEN** a compare or merge row is rendered with an added-line decoration
- **THEN** the row fill SHALL remain visually distinguishable while the text itself stays neutral and readable

#### Scenario: Conflict row stays distinguishable without overpowering the text
- **WHEN** a merge conflict row is rendered
- **THEN** the conflict state SHALL remain visible through the low-contrast palette without overwhelming the foreground glyphs
