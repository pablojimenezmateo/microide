## ADDED Requirements

### Requirement: Banner actions are reachable from the control channel
`banner-action <label>` SHALL press the button with that label (case-insensitive) on the active editor tab's banner exactly as a click does — Compare, Reload, Overwrite and Keep on the changed-on-disk banner, Retry on a failed open — and SHALL reject naming what is missing when the tab shows no banner or the banner has no such button. A refused `save` SHALL keep naming the banner, so the two together let an agent resolve the conflict it was told about.

#### Scenario: An agent resolves a changed-on-disk refusal
- **WHEN** `save` is refused because the file changed on disk and the agent runs `banner-action Reload`
- **THEN** the buffer holds the on-disk version, the banner is gone, and the next `save` succeeds

#### Scenario: Nothing to press
- **WHEN** `banner-action Overwrite` runs with no banner on the active tab
- **THEN** it is rejected with "the active tab shows no banner"
