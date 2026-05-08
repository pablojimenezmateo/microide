## ADDED Requirements

### Requirement: Legacy AI Records Are Tolerated But Not Rewritten
The persisted-state reader SHALL tolerate legacy AI-related records that may exist in historical workspace/session files, and the writer SHALL NOT emit new AI conversation/provider records after this change.

#### Scenario: Workspace with legacy AI records is opened and saved
- **WHEN** a persisted workspace/session payload contains historical AI conversation or provider fields
- **THEN** load SHALL complete without failure, and the next successful save SHALL omit AI-only records

## REMOVED Requirements

### Requirement: AI Provider Configuration Has A Persisted Section
**Reason**: Built-in AI provider configuration is removed from product scope.
**Migration**: Stop writing AI provider config/override payloads and treat legacy values as ignored compatibility data during load.
