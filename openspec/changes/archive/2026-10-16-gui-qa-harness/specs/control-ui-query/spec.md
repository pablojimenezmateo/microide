## ADDED Requirements

### Requirement: The ui query names what is on screen

The control channel SHALL answer the query `ui` with a JSON object describing the current window: its size, the focus target, the sidebar (visible, view id), the overlay (visible, mode), the menu state (open menu, active item, submenu, context menu), the panel (visible, content, maximized), the layout's surface rects (menu bar, tab strip, breadcrumb, editor surface, sidebar, right pane, bottom panel, status bar), and an `elements` array. Each element SHALL carry a stable `name`, a `kind`, a `rect` as `[x, y, w, h]` in window coordinates (the coordinates mouse events use), and, where it applies, `label`, `enabled`, `active` and `selected`.

#### Scenario: Menu bar items are locatable
- **WHEN** a client queries `ui` with no menu open
- **THEN** every visible menu bar item appears as an element named `menu:<label>` (for example `menu:Edit`) whose rect contains the point a click must hit to open it

#### Scenario: An open menu's rows are locatable
- **WHEN** a menu is open
- **THEN** each of its visible rows appears as `menu-item:<menu>/<label>` with `enabled`, and a separator is not an element

#### Scenario: Workspace chrome is locatable
- **WHEN** a client queries `ui` with a project open
- **THEN** editor tabs appear as `tab:<group>/<title>`, sidebar view buttons as `sidebar-view:<view id>`, visible sidebar rows as `row:<view id>/<label>`, bottom panel tabs as `panel-tab:<title>`, status bar segments as `status:<segment id>`, and editor banner, notification and sidebar header buttons as `button:<surface>/<label>`

#### Scenario: Names are unique
- **WHEN** two elements would share a name (two tabs titled `main.cpp`)
- **THEN** the later ones carry a `#2`, `#3`, ... suffix in visual order, so a name always resolves to one element

### Requirement: The ui query reads state and never changes it

Answering `ui` SHALL NOT change any state, request a redraw, or depend on a frame having been painted since the last change; geometry SHALL come from the same layout and hit-test computations that input handling uses, so a rect the query reports is the rect a click is tested against.

#### Scenario: Querying is invisible
- **WHEN** a client queries `ui` repeatedly with no input in between
- **THEN** every answer is identical and no frame is rendered because of the queries

#### Scenario: Reported rects hit what they name
- **WHEN** a client clicks the center of an element's rect
- **THEN** the click lands on that element, by the same hit test that handles a mouse click
