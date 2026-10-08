## ADDED Requirements

### Requirement: Notification rows carry inline action buttons
A notification row SHALL carry zero or more actions, each a label plus an `ActionId` with arguments, rendered as inline buttons on the row in the style of VS Code's notification toasts. Actions SHALL be host-owned data, not closures, so plugins and the control channel can name them and a row survives being re-keyed. Labels and button geometry SHALL be composed in `RenderViewModelBuilder`; the render translation unit SHALL only draw them.

#### Scenario: A formatter failure offers Show Output
- **WHEN** a format-on-save formatter fails
- **THEN** the warning row shows a `Show Output` button that opens the formatter's output channel when clicked

#### Scenario: A sticky row's actions update in place
- **WHEN** a keyed sticky row is reposted with a different action set
- **THEN** the row's buttons change without the row moving or duplicating

### Requirement: Actions are reachable by pointer, keyboard and control channel
Clicking a button SHALL dispatch its action through the normal action executor and, unless the action says otherwise, dismiss a transient row. The focused notification's actions SHALL be reachable from the keyboard. The control channel SHALL list a row's actions and invoke one by label or id.

#### Scenario: Invoking an action from the control channel
- **WHEN** an agent invokes the `Reconnect` action of the connection row through the control channel
- **THEN** the same action runs as when the button is clicked

### Requirement: Rows with actions do not expire under the user
A transient row that carries actions SHALL stay up while the pointer is over it, and SHALL use a longer default lifetime than a plain row.

#### Scenario: Hovering a toast keeps it
- **WHEN** the pointer rests on a transient row with a button past its normal expiry
- **THEN** the row remains until the pointer leaves
