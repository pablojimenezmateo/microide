# Follow-Ups

## legacy-persistence-cleanup

- target release: `+2` from this change landing
- owner area: `workspace persistence`

### Planned Deletions

- `src/workspace/WorkspacePersistenceLegacyFormat.h`
- `src/workspace/WorkspacePersistenceLegacyFormat.cpp`
- legacy importer call sites in workspace persistence startup/load paths
- legacy sidecar artifacts named `<file>.legacy` generated for migration compatibility

### Preconditions Before Removal

The following perf harness scenarios must remain green at cleanup time:

- `cold_startup_no_project`
- `cold_startup_small_project`
- `cold_startup_large_project`
- `multi_project_switch`
- `multi_tab_cycle`
- `typing_small_file`
- `typing_large_file`
- `scroll_large_file`
- `project_search_literal`
- `project_search_regex`

The following behavior checks should also remain green:

- session restore from persisted records
- workspace/project config round-trip through `PersistenceService`
