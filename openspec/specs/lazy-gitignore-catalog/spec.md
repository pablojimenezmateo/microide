# lazy-gitignore-catalog Specification

## Purpose
TBD - created by archiving change editor-project-scope-polish. Update Purpose after archive.
## Requirements
### Requirement: Ignored Nodes Stay Visible In The Project Tree

MicroIDE SHALL show `.gitignore`-matched files and directories in the project tree using ignored-node presentation instead of hiding them outright, unless the user explicitly enables a separate hide-ignored filter.

#### Scenario: Ignored file remains reachable
- **WHEN** a project contains `.env.local` and `.gitignore` matches that file
- **THEN** the project tree SHALL still show `.env.local` as an ignored file node that the user can open directly

### Requirement: Ignored Directories Materialize On Demand

MicroIDE SHALL treat ignored directories as collapsed opaque nodes during initial tree build. Initial tree construction SHALL record the ignored directory node itself without recursively enumerating ignored descendants. Expanding an ignored directory SHALL enumerate only its immediate children at that moment, and deeper ignored descendants SHALL remain unmaterialized until their parent is expanded.

#### Scenario: Large ignored subtree stays collapsed on initial load
- **WHEN** a project contains an ignored `node_modules/` directory with many nested descendants
- **THEN** initial project-tree construction SHALL show the `node_modules/` node without recursively materializing its children

#### Scenario: Expanding an ignored directory reveals one level
- **WHEN** the user expands an ignored directory node
- **THEN** MicroIDE SHALL enumerate and show that directory's immediate children without treating deeper descendants as already materialized

### Requirement: Ignored Content Stays Out Of Background Tools By Default

Background indexing, search, diagnostics discovery, git/project refresh, and file-watcher-triggered parsing SHALL exclude ignored descendants by default even if those nodes are visible or expanded in the tree. Opening an ignored file directly SHALL still load it in a normal editor tab.

#### Scenario: Opened ignored file remains excluded from background scans
- **WHEN** the user opens an ignored file directly from the project tree
- **THEN** the file SHALL open normally in an editor tab, and ignored descendants SHALL remain excluded from background indexing and search unless a separate opt-in feature explicitly includes them

#### Scenario: Expanded ignored directory does not join project search
- **WHEN** the user expands an ignored directory and then runs project search
- **THEN** the search corpus SHALL still exclude files under that ignored directory by default

