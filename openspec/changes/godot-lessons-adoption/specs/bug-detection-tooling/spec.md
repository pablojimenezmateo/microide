## ADDED Requirements

### Requirement: Non-Throwing Precondition Macro Family

The repository SHALL ship a header `src/util/Fail.h` providing a small family of precondition macros that assert in debug builds and log+return in release builds. Each macro SHALL be runtime-toggleable via a thread-local flag controlled by the `LogSilencer` RAII guard so that test failure-path coverage does not pollute test output. No macro SHALL throw; the macros are subject to the existing repository policy that bans exceptions in these paths.

#### Scenario: Index check returns the supplied default and logs in release
- **WHEN** code at site `S` invokes `MICROIDE_FAIL_INDEX_V(i, n, retval)` with `i >= n` in a release build
- **THEN** the macro SHALL log a single failure entry through the existing `Log` infrastructure that includes the file path and line number of `S`
- **AND** SHALL evaluate to a `return retval;`
- **AND** SHALL NOT throw, abort, or call `std::terminate`

#### Scenario: Index check asserts in debug builds
- **WHEN** code invokes `MICROIDE_FAIL_INDEX_V(i, n, retval)` with `i >= n` in a debug build
- **THEN** the macro SHALL trigger a debug assertion that aborts the process with a diagnostic identifying the file and line

#### Scenario: Compatible variants exist for common precondition shapes
- **WHEN** a developer needs the equivalent macros for boolean conditions, null-pointer checks, and `void`-returning functions
- **THEN** the header SHALL provide `MICROIDE_FAIL_COND_V(cond, retval)`, `MICROIDE_FAIL_NULL_V(ptr, retval)`, and the void-returning variants `MICROIDE_FAIL_INDEX(i, n)`, `MICROIDE_FAIL_COND(cond)`, `MICROIDE_FAIL_NULL(ptr)`
- **AND** each variant SHALL share the same debug-assert / release-log+return semantics

#### Scenario: Macros respect the log-silencer flag
- **WHEN** a `MICROIDE_FAIL_*` macro fires inside a scope where a `LogSilencer` is active
- **THEN** the macro SHALL NOT emit a log entry
- **AND** SHALL still evaluate to the expected `return` so control-flow semantics are unchanged

### Requirement: Scoped LogSilencer RAII Guard

The repository SHALL ship a header `tests/util/LogSilencer.h` providing an RAII guard that, on construction, flips a thread-local flag to suppress `MICROIDE_FAIL_*` log emissions and, on destruction, restores the flag to its prior value. The guard SHALL be safe to nest.

#### Scenario: Guard suppresses logs within its scope
- **WHEN** a test constructs `LogSilencer guard;` and then triggers a code path that fires `MICROIDE_FAIL_*` macros
- **THEN** no log entries SHALL be emitted from those macros while `guard` is in scope
- **AND** the test summary SHALL NOT contain noise from the deliberately-triggered failure paths

#### Scenario: Flag is restored on scope exit
- **WHEN** `guard` goes out of scope
- **THEN** the thread-local suppression flag SHALL return to the value it held before `guard` was constructed
- **AND** subsequent `MICROIDE_FAIL_*` macros SHALL log normally

#### Scenario: Nested guards behave as expected
- **WHEN** a `LogSilencer outer;` is in scope and code constructs a nested `LogSilencer inner;`
- **THEN** suppression SHALL remain active for the duration of both guards
- **AND** when `inner` is destroyed, suppression SHALL remain active because `outer` is still in scope
- **AND** when `outer` is destroyed, suppression SHALL be cleared

#### Scenario: Dedicated unit test asserts restoration
- **WHEN** the test suite runs
- **THEN** at least one test SHALL exercise the construct/destruct cycle and assert that the suppression flag is restored after destruction
- **AND** SHALL exercise the nested case

### Requirement: EventRecorder Test Helper For Host Service Notifications

The repository SHALL ship a header `tests/util/EventRecorder.h` providing a recorder that subscribes to a host service's notification surface, captures every emitted event in order with a stable to-string serialization, and exposes the captured sequence so tests can assert on the entire dispatched sequence (not only on final state).

#### Scenario: Recorder captures the dispatched sequence
- **WHEN** a test attaches an `EventRecorder` to a host service (e.g., `EditorTabService`) and then performs a sequence of actions that emit notifications
- **THEN** the recorder SHALL contain the emitted events in the order they were dispatched
- **AND** each captured event SHALL be a stable, deterministic string for use in test assertions

#### Scenario: Recorder is a test-only utility
- **WHEN** the production binary is built
- **THEN** `EventRecorder` SHALL NOT be linked into the production binary
- **AND** the host services SHALL NOT carry any `EventRecorder`-specific code paths in production

#### Scenario: Recorder unsubscribes on destruction
- **WHEN** an `EventRecorder` instance goes out of scope
- **THEN** it SHALL unsubscribe from the host service
- **AND** subsequent emissions on that service SHALL NOT be recorded by the destroyed instance
