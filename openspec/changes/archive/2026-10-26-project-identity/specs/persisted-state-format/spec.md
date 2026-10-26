## ADDED Requirements

### Requirement: Project lists persist project identities
The workspace session's open projects and the MRU recent projects SHALL be written as project-identity records (`Local` with its path, or `Remote` with its host and host root). For one release window the writer SHALL also write each entry's filesystem root under the legacy path tag, so that an older build of the same major version restores the session. A reader SHALL prefer the identity record. When an entry has only the legacy path, the reader SHALL resolve it as the editor resolves a typed path: `Remote` when the path is the default tree of the mirror whose record names it, `Local` otherwise.

#### Scenario: A file written before this change
- **WHEN** a session file listing a local project and a remote project's mirror tree by path alone is read
- **THEN** the first restores as `Local(path)` and the second as `Remote(host, host_root)`, with no loss

#### Scenario: An older build reads a new file
- **WHEN** a build without identity records reads a session written by this one
- **THEN** it skips the identity records as unknown tags and restores the same projects from the legacy paths
