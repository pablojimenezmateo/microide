## ADDED Requirements

### Requirement: Remote components stay out of the shell
Remote client components SHALL live in `src/project/remote/` (codec, `RemoteServerClient`, `RemoteHostSession`, `RemoteProcessLauncher`, `RemoteTerminalView`) behind one workspace-level `RemoteHostService`. No `WorkspaceShell*.cpp` companion SHALL be added and the shell's hard-linted line caps and ratchets SHALL NOT be raised for this work. A host session SHALL own the `ProjectLocality` it hands out and outlive every project state that points at it.

#### Scenario: The shell companion count is unchanged
- **WHEN** this change is complete
- **THEN** the architecture lint's companion-TU count and the `WorkspaceShellMembers.inc` cap are at or below their values before it

### Requirement: The server binary links only the kernel
`microide-server` SHALL be built from `microide_kernel` plus `src/server/` and SHALL NOT link `microide_core`, SDL or any windowing library. The kernel SHALL have its own test executable, `microide_kernel_tests`, that links no windowing library and runs in the default, ASAN, UBSAN, TSAN and clang lanes.

#### Scenario: A server source that includes a shell header fails the build lint
- **WHEN** a file under `src/server/` includes, directly or transitively, an SDL header or a header outside the kernel source list
- **THEN** the architecture lint fails naming the include chain
