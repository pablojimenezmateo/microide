## ADDED Requirements

### Requirement: A project is named by its identity, not by its filesystem root
Every project SHALL carry a `ProjectId` that is either `Local(path)` or `Remote(host, host_root)`, never both. The filesystem root that file, git, search and process sites use SHALL be derived from it: the path for a local project, and the mirror's tree for a remote one. The id SHALL be fixed when the project's catalog entry is created (by an open, a session restore or a recents entry) and SHALL survive every reset of the project's scoped state, as its locality does.

#### Scenario: A restored remote project is remote before it is activated
- **WHEN** a session with a local project (active) and a remote project (inactive) is restored
- **THEN** the remote project's catalog entry already carries a `Remote` id, before any connection and without its tab being activated

#### Scenario: A copy of a mirror is a local folder
- **WHEN** the user opens a copy of a mirror's tree from another directory
- **THEN** its id is `Local`, and nothing connects

### Requirement: Presentation renders the identity
Every user-visible name of a project SHALL be derived from its id: the project tab's title and badge, its tooltip, the window title, the welcome and Quick Open recents, and the prompts that name the project. A remote project's short label SHALL be its host root's basename, and its full location SHALL be `host:/abs/path`. No user-visible text SHALL show a remote project's mirror path, except messages about the mirror itself (for example a failure to create it).

#### Scenario: A remote project's tab and window title
- **WHEN** `worky:/home/me/repo` is open with `src/main.cpp` active
- **THEN** the project tab reads `repo`, its tooltip reads `worky:/home/me/repo`, and the window title is `main.cpp — repo — microide`

#### Scenario: A prompt that names the project
- **WHEN** Discard All is asked in that project
- **THEN** the prompt names `repo`, not `tree`

### Requirement: A remote recents entry outlives its mirror
A recent remote project whose mirror directory no longer exists SHALL stay in the recents list and SHALL reopen by creating the mirror again from its stored host and host root. `Remote: Remove Mirror` SHALL still remove the entry. A recent local project whose directory no longer exists SHALL stay hidden, as today.

#### Scenario: The data directory was wiped
- **WHEN** the mirror of `worky:/home/me/repo` is deleted outside the editor and the user picks it from the welcome recents
- **THEN** the editor opens `worky:/home/me/repo` and syncs a new mirror
