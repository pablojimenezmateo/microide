## ADDED Requirements

### Requirement: The host entry points sit beside their local twins
`Remote: Open Folder on Host…` SHALL appear in the File menu directly under Open Folder…, `Remote: Open Terminal on Host…` in the Terminal menu directly under New Terminal, and the cold-start welcome page SHALL offer Open Folder on Host… as a second start button under Open Folder…, in its own layout slot. The command palette entries remain.

#### Scenario: A user who has not read the README
- **WHEN** the editor starts with no project
- **THEN** the welcome page and the File menu both show how to open a folder on a host

#### Scenario: The welcome buttons never collide
- **WHEN** the welcome layout is computed for the no-project page at any size
- **THEN** the host-folder button lies below the open-folder button and above the recent rows, and clicking it opens the host prompt
