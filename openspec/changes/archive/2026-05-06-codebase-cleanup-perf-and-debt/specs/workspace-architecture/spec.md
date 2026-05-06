## ADDED Requirements

### Requirement: No Synchronous Subprocess In Workspace Code

No translation unit under `src/workspace/` SHALL call `platform::RunSubprocess(` directly. Workspace code that needs to run an external process (formatters, tool validators, hashers, language helpers) SHALL dispatch the work through `ProjectBackgroundExecutor` and apply the result back on the main thread via the executor's completion-callback path. The architectural-lint test SHALL hard-fail on a violation.

#### Scenario: New synchronous subprocess call is rejected
- **WHEN** a file under `src/workspace/` adds the substring `platform::RunSubprocess(` outside an explicit allowlist of executor wrappers
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the file and line in the failure message

#### Scenario: Allowlist is documented and minimal
- **WHEN** the lint runs
- **THEN** the allowlist SHALL be empty by default; any addition SHALL require a code change to the lint with a comment explaining why the call site cannot route through `ProjectBackgroundExecutor`

### Requirement: Render Translation Units Do Not Materialize Strings At Draw Time

No function body in any `src/workspace/WorkspaceShellRender*.cpp` translation unit SHALL allocate or compose a `std::string`. All draw-time strings SHALL come from prebuilt fields on the view model passed in by `RenderViewModelBuilder`. Forbidden patterns include the `std::string(...)` constructor with a non-empty argument list, `operator+` and `operator+=` between string-typed expressions, `to_string`, and `std::format` / `fmt::format` returning `std::string`. The architectural-lint test SHALL hard-fail on a violation.

#### Scenario: Per-frame string concat is rejected
- **WHEN** a function body inside `src/workspace/WorkspaceShellRender*.cpp` contains `"foo" + std::string(view)` or a similar materializing pattern
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the offending function

#### Scenario: View-model fields are the documented destination
- **WHEN** the lint reports a violation
- **THEN** the failure message SHALL direct the contributor to add a string field on the surface's view model and have `RenderViewModelBuilder` populate it instead

#### Scenario: Builder TUs are exempt
- **WHEN** the lint runs
- **THEN** files whose names match `src/workspace/RenderViewModelBuilder*.cpp` SHALL be excluded; only the render-consumer TUs are constrained

### Requirement: Text Viewport Mutators Do Not Snapshot Whole Documents

No non-const member function of `editor::TextViewport` SHALL capture a copy of `document_->lines` as a whole vector for undo or comparison purposes. Mutators that need an undo snapshot SHALL capture only the affected line ranges, consistent with the range-based undo model already used by ordinary edits. The architectural-lint test SHALL hard-fail on a violation.

#### Scenario: Full-document copy in mutator is rejected
- **WHEN** a non-const member function of `TextViewport` contains `std::vector<std::string> <name> = document_->lines` or `auto <name> = document_->lines` (without a subscript)
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the offending function

#### Scenario: Range-based snapshots remain allowed
- **WHEN** a mutator captures `document_->lines[i]`, `document_->lines.begin() + lo` through `document_->lines.begin() + hi`, or any subscripted/iterator-bounded slice
- **THEN** the architectural-lint test SHALL accept the pattern; only the full-vector copy is rejected
