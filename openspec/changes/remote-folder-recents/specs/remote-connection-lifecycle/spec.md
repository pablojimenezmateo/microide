## ADDED Requirements

### Requirement: The host prompt offers recently opened host folders
The Open Folder on Host… prompt SHALL list, under its input and headed Recent, the remote folders most recently opened, newest first, at most five, each shown as the `[user@]host[:port]:/path` it is reopened by and never as its mirror's local path. Clicking a row SHALL open that folder as Enter would with that text. Up and Down SHALL cycle through the rows, writing the row into the input without opening it. The row whose text equals the input SHALL be highlighted. With no remote folder in the recents the prompt SHALL show no Recent section and keep its plain size. Rows that do not fit the window SHALL be dropped rather than drawn over the buttons, and every hit test SHALL use the same rows the paint draws.

#### Scenario: Reopening a host folder without typing
- **WHEN** the user has opened `dev@build:/srv/app` and later opens Open Folder on Host…
- **THEN** a Recent row reads `dev@build:/srv/app`, and clicking it opens that folder

#### Scenario: Only the five latest, newest first
- **WHEN** seven remote folders and some local folders have been opened
- **THEN** the prompt lists the five remote folders opened last, newest first, and no local folder

#### Scenario: Picking with the keyboard
- **WHEN** the prompt lists two recent folders and the user presses Down twice and then Enter
- **THEN** the second row is written into the input, highlighted, and opened by Enter

#### Scenario: Nothing to recall
- **WHEN** no remote folder has ever been opened
- **THEN** the prompt has no Recent heading and is the same size as before
