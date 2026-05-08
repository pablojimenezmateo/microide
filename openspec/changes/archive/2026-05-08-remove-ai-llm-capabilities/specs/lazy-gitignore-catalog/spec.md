## MODIFIED Requirements

### Requirement: Ignored Content Stays Out Of Background Tools By Default

Background indexing, search, diagnostics discovery, git/project refresh, and file-watcher-triggered parsing SHALL exclude ignored descendants by default even if those nodes are visible or expanded in the tree. Opening an ignored file directly SHALL still load it in a normal editor tab.

#### Scenario: Opened ignored file remains excluded from background scans
- **WHEN** the user opens an ignored file directly from the project tree
- **THEN** the file SHALL open normally in an editor tab, and ignored descendants SHALL remain excluded from background indexing and search unless a separate opt-in feature explicitly includes them

#### Scenario: Expanded ignored directory does not join project search
- **WHEN** the user expands an ignored directory and then runs project search
- **THEN** the search corpus SHALL still exclude files under that ignored directory by default
