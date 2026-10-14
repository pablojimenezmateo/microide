## ADDED Requirements

### Requirement: One queue of work that runs after deferred saves land
Every caller that must act after a save SHALL register a continuation on the tabs it is waiting for and SHALL NOT block the shell thread on a formatter. A continuation SHALL run once every tab it waits on has completed its deferred save, SHALL be cancelled with a notification if any of those saves fails or is refused, and SHALL re-check its preconditions when it runs. Tab close, rename, delete, project close and quit SHALL all use this one mechanism; the separate `close_after_save` flag and parked `deferred_path_mutation` SHALL be replaced by it.

#### Scenario: Closing a dirty tab with a slow formatter
- **WHEN** the user closes a dirty tab, chooses Save, and its formatter takes two seconds
- **THEN** the shell keeps rendering during those two seconds and the tab closes when the write lands

#### Scenario: A failed save cancels the continuation
- **WHEN** a rename waits on a save that the external-change guard refuses
- **THEN** the file is not renamed and a notification says why

### Requirement: Quit waits for saves with progress and a Cancel button
Quitting with dirty buffers the user chose to save SHALL show a sticky progress row ("Saving N files before quitting…") with a Cancel action, keep the shell responsive, and exit when the last write lands. Cancel SHALL abort the quit and leave every buffer open with its contents; saves already written stay written. The application SHALL NOT exit with a save in flight and SHALL NOT exit unformatted on a timeout without the user's choice.

#### Scenario: Quit with two dirty files and a slow formatter
- **WHEN** the user quits, chooses Save All, and each formatter takes a second
- **THEN** a progress row with Cancel is shown, the window keeps repainting, and the application exits after both files are written

#### Scenario: Cancelling the quit
- **WHEN** the user clicks Cancel on the quit progress row while a formatter is running
- **THEN** the application stays open, the buffer whose save had not landed is still open and dirty, and no further continuation runs

### Requirement: Closing a project waits the same way
Closing a project with dirty buffers the user chose to save SHALL defer those saves, show the same progress row with Cancel, and tear down the project only after the last write lands. A continuation SHALL NOT run against a project that was switched away from or closed by another path meanwhile.

#### Scenario: Close a project while its formatter runs
- **WHEN** the user closes a project with a dirty buffer whose formatter is slow
- **THEN** the project closes after the write lands, and its state is not torn down before
