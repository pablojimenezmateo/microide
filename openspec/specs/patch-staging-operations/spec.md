## Purpose

Define hunk-level and selected-line patch staging, unstaging, and discard semantics for the Git
workstation, including patch preview, preflight, stale-state handling, and structured failure
results.

## Requirements

### Requirement: Patch Operations Are Service-Owned
Hunk-level and selected-line staging, unstaging, and discard operations SHALL be executed through a host-owned patch application service. UI surfaces SHALL submit typed repository-relative targets and selection ranges rather than constructing or applying Git patches directly.

#### Scenario: Compare tab stages hunk
- **WHEN** the user stages a selected hunk from a compare tab
- **THEN** the compare tab SHALL send a typed hunk target to the patch application service and SHALL NOT invoke Git apply directly

### Requirement: Stage And Unstage Support File, Hunk, And Selected Lines
MicroIDE SHALL support staging and unstaging at file, hunk, and selected-line granularity for text files where Git can apply the generated patch cleanly.

#### Scenario: Stage selected hunk
- **WHEN** the user stages a hunk from an unstaged working-tree diff
- **THEN** MicroIDE SHALL apply only that hunk to the index and refresh repository state after completion

#### Scenario: Stage selected lines
- **WHEN** the user selects changed lines inside one hunk and stages the selection
- **THEN** MicroIDE SHALL generate a patch containing only the selected changed lines plus required context and SHALL apply it to the index

#### Scenario: Unstage selected lines
- **WHEN** the user selects staged changed lines and invokes unstage
- **THEN** MicroIDE SHALL reverse the selected staged patch from the index without modifying unrelated worktree changes

### Requirement: Discard Operations Require Preview And Confirmation
Discard hunk and discard selected-line operations SHALL show the reverse patch or an equivalent exact preview before modifying the worktree. Confirmation SHALL be required for every discard operation.

#### Scenario: Discard hunk preview
- **WHEN** the user requests discard for a hunk
- **THEN** MicroIDE SHALL show the changes that will be removed and SHALL NOT modify the worktree until the user confirms

#### Scenario: User cancels discard
- **WHEN** the discard preview is visible and the user cancels
- **THEN** no Git apply or filesystem mutation SHALL run

### Requirement: Stale Patch Application Fails Safely
Patch operation requests SHALL carry repository snapshot and diff model generation metadata. If the target file, index, or diff generation changed before application, MicroIDE SHALL fail the operation with a structured stale or patch-failed result and offer refresh.

#### Scenario: File changes before stage selected lines
- **WHEN** a selected-line staging operation is requested from diff generation 20 and the file changes before the patch applies
- **THEN** MicroIDE SHALL report that the diff is stale, SHALL leave the index/worktree unchanged, and SHALL offer to refresh the diff

#### Scenario: Git apply fails
- **WHEN** Git rejects a generated patch
- **THEN** MicroIDE SHALL surface `patch_did_not_apply` with Git output available as detail and SHALL NOT silently retry against a different diff

### Requirement: Patch Application Is Preflighted And Atomic
Patch operations SHALL preflight applicability before mutation when supported. Failed apply operations SHALL leave index and worktree unchanged for the requested target scope, and reject-style partial patch output SHALL not be used for these user-facing operations.

#### Scenario: Preflight fails
- **WHEN** preflight applicability check fails for a generated patch
- **THEN** MicroIDE SHALL return a structured failure result and SHALL not run a mutating apply step

#### Scenario: Mixed staged and unstaged file
- **WHEN** a file has staged changes plus unrelated unstaged changes and the user unstages selected staged lines
- **THEN** only the index SHALL change for the selected lines and unrelated worktree edits SHALL remain unchanged

### Requirement: Binary And Unsupported Targets Are Explicit
MicroIDE SHALL disable hunk and selected-line patch actions for binary files, submodule pointer changes, and any diff target that lacks stable text line mapping.

#### Scenario: Binary file selected
- **WHEN** a binary file summary row is selected
- **THEN** hunk and selected-line stage/discard actions SHALL be disabled with an explanatory status message
