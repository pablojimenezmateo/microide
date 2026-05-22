## ADDED Requirements

### Requirement: Preview Scope Is Narrow And Explicit
The Git Workstation Preview SHALL document support for opening a local Git repository, inspecting working-tree state, viewing staged and unstaged diffs, staging and unstaging files and hunks, resolving common text conflicts, committing staged changes, and reviewing a branch against a base. Unsupported workflows SHALL be listed explicitly.

#### Scenario: User reads preview scope
- **WHEN** a user opens preview release documentation
- **THEN** the documentation SHALL list supported and unsupported workflows without claiming MicroIDE is a complete IDE

#### Scenario: Unsupported hosted review
- **WHEN** preview documentation mentions branch review
- **THEN** it SHALL state that hosted PR review, account auth, and network review sync are not part of the preview

#### Scenario: Conflict-class support boundaries are explicit
- **WHEN** preview documentation describes merge support
- **THEN** it SHALL explicitly identify fully supported common text conflicts versus recognized summary-only conflict classes such as binary, submodule, or complex rename/file-directory conflicts

### Requirement: Plugins Can Be Disabled At Startup
MicroIDE SHALL provide `--disable-plugins` and `--safe-mode` startup flags. `--disable-plugins` SHALL skip user-scope plugin loading. `--safe-mode` SHALL imply plugin disabling and SHALL skip nonessential startup behavior documented by the flag.

#### Scenario: Disable plugins flag
- **WHEN** MicroIDE starts with `--disable-plugins`
- **THEN** it SHALL not load user-scope Lua plugins or plugin syntax files, while built-in editor, Git, diff, merge, search, and terminal workflows remain available

#### Scenario: Safe mode flag
- **WHEN** MicroIDE starts with `--safe-mode`
- **THEN** it SHALL run with plugins disabled, disable plugin syntax loading, skip workspace/session restore, and start in a recovery-safe empty-shell mode unless an explicit project argument is provided

### Requirement: Safe Mode V1 Behavior Is Exact
`--safe-mode` v1 SHALL imply `--disable-plugins`, disable plugin syntax loading, skip workspace/session restore, open only an explicitly provided project argument or an empty shell, and surface visible safe-mode state.

#### Scenario: Safe mode v1 with no project argument
- **WHEN** MicroIDE starts with `--safe-mode` and no explicit project path argument
- **THEN** it SHALL open an empty shell with visible safe-mode state and SHALL not auto-restore prior workspace/session state

### Requirement: Disabled Plugin State Is Visible
When plugins are disabled by startup flag or safe mode, MicroIDE SHALL surface a visible status in host-owned UI such as the status bar, Help/About, or startup notice.

#### Scenario: User verifies safe mode
- **WHEN** MicroIDE is running in safe mode
- **THEN** the UI SHALL visibly indicate that safe mode or plugins-disabled mode is active

### Requirement: Preview Release Artifacts Are Reviewable
A Git Workstation Preview release SHALL include a tagged version, release notes, checksums for distributed binaries, screenshot gallery or equivalent visual walkthrough, short demo or scripted walkthrough, known limitations, tested workflows list, crash/data-loss reporting instructions, and build-from-source instructions.

#### Scenario: Preview tag is published
- **WHEN** the preview release is cut
- **THEN** the tag, checksums, release notes, tested workflows, and known limitations SHALL be available from the repository

### Requirement: Preview Claims Avoid Comparative Marketing
Preview documentation SHALL describe MicroIDE as native, low-footprint, responsive, and methodology-measured. It SHALL NOT claim to be fastest or compare CPU/memory use against other editors unless a future approved methodology adds comparative benchmarks.

#### Scenario: Release notes describe performance
- **WHEN** release notes mention performance
- **THEN** they SHALL reference internal regression baselines or supported workflows rather than claiming superiority over another editor

### Requirement: Branch Review Persistence Is Optional For Preview
Preview scope SHALL require branch review against a base, but persistent reviewed-file or reviewed-hunk state MAY ship after preview if core Git/diff/merge/commit workflows and safety requirements are already met.

#### Scenario: Preview ships without persistent reviewed markers
- **WHEN** branch review mode is available and reviewed-state persistence is deferred
- **THEN** preview documentation SHALL list persistent review markers as a post-preview enhancement rather than a shipped workflow guarantee
