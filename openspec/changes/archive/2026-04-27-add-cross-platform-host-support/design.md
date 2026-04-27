## Context

MicroIDE’s rendering, text model, compare or merge flows, and most workspace behavior are already largely platform-agnostic through SDL3 and CMake. The real portability gaps sit in host-facing services: app directories, process launch, PTY-backed terminals, file watching, recycle-bin or trash behavior, URL and reveal-in-file-manager actions, and packaging or launch validation. Today those seams are inconsistent: some are Linux-only, some are generic POSIX, some have partial macOS handling, and Windows support is still missing in several paths.

The repo already contains a detailed macOS plan and separate notes showing Windows pressure on the same architecture. That work points to the same conclusion: cross-platform support is not a packaging afterthought. It requires explicit host-owned service boundaries so macOS and Windows can be implemented cleanly without scattering conditional logic through `WorkspaceShell` and related UI orchestration.

## Goals / Non-Goals

**Goals:**
- Define a durable product contract for Linux, macOS, and Windows as first-class supported hosts.
- Move host-facing behavior behind narrow platform services instead of embedding Linux-first assumptions in terminal, process, watcher, and file-operation code.
- Make packaging, launch, and validation part of the supported-host contract so macOS bundles and Windows desktop builds are tracked as product work rather than “later”.
- Preserve host-owned rendering and workflow behavior while making platform-specific behavior explicit and testable.

**Non-Goals:**
- Rebuild the UI around native platform widgets, native menu bars, or platform-specific visual styling.
- Commit to bundling every external tool or language server for every platform in the first pass.
- Solve later-stage credential-storage polish such as Keychain or Windows Credential Manager integration in the same initial bring-up unless it blocks basic workflows.
- Promise exact feature parity for every optional OS integration before the core host services are correct.

## Decisions

### 1. Treat platform support as a host-service capability, not as scattered compatibility fixes

The implementation should center on a small set of explicit host-owned services: app directories, process launch, async process launch, terminal backend, file watching, file-manager or URL integration, and recycle-bin or trash behavior. Workspace and editor layers should call those services through stable contracts rather than knowing which host they are on.

Why this approach:
- It keeps platform complexity out of `WorkspaceShell` and the UI hot path.
- It gives Windows and macOS the same architectural status as Linux instead of treating them as fallback branches.
- It creates seams that are testable in isolation.

Alternatives considered:
- Adding targeted `#ifdef` branches inside existing Linux-first implementations. Rejected because it preserves the wrong ownership boundaries and makes later platform work harder.

### 2. Split terminal and process concerns before trying to “port the terminal”

Terminal screen parsing and rendering should remain host-agnostic, while PTY or console session launch, resize, shutdown, and child-process control should move behind an explicit terminal backend or process service boundary. Linux, macOS, and Windows can then provide platform-specific implementations without forking the terminal UI itself.

Why this approach:
- Terminal support is currently one of the largest Linux-only seams.
- Windows console or pseudoconsole support and macOS PTY behavior differ in real ways, but the screen model does not need to fork.
- This also improves subprocess handling for git, formatters, language servers, plugin tools, and provider bridges.

Alternatives considered:
- Porting the current `TerminalSession` implementation directly on each host. Rejected because it keeps lifecycle, process, and screen responsibilities coupled.

### 3. Require platform packaging and launch validation in the same capability

macOS `.app` bundles, Windows desktop packaging, runtime asset discovery, and host-specific launch expectations belong in the same capability as the underlying services. A host is not supported when a binary merely compiles; it is supported when it launches correctly, finds assets, uses correct app directories, and passes targeted validation on that host.

Why this approach:
- It prevents “compiles on macOS/Windows” from being mistaken for product support.
- It forces packaging and launch behavior to evolve with the platform service seams.
- It aligns with the repo’s existing macOS plan and with the need for Windows-specific desktop behavior.

Alternatives considered:
- Deferring packaging and CI to a later documentation-only phase. Rejected because it creates a false sense of host support and leaves durable product requirements undefined.

### 4. Use one capability for Linux, macOS, and Windows instead of separate per-host capabilities

The durable spec should define host support as one capability with requirement-level scenarios for each supported platform and subsystem. Implementation can still land in slices, but the contract should make the supported-host set explicit in one place.

Why this approach:
- The platform seams are shared: watcher, process, terminal, packaging, directories, file operations.
- It avoids duplicating the same product guarantees across separate macOS and Windows specs.
- It keeps the support matrix visible and coherent.

Alternatives considered:
- Separate `macos-support` and `windows-support` capabilities. Rejected because the architecture work is cross-cutting and the contract would fragment.

## Risks / Trade-offs

- [Risk] The change can sprawl across many subsystems and stall if treated as one giant port. → Mitigation: define one capability but implement it in phased slices around directories, process/terminal, file watching, then packaging and CI.
- [Risk] Windows terminal and process semantics may force a larger refactor than macOS. → Mitigation: make the backend boundary explicit first so Windows-specific work is isolated behind the host contract.
- [Risk] Platform packaging work can consume time without improving core editing behavior. → Mitigation: keep packaging in scope only to the degree needed to make supported-host launches real and testable.
- [Risk] Non-Linux CI can be flaky or slow initially. → Mitigation: start with focused platform service and smoke tests rather than trying to port every large integration fixture immediately.

## Migration Plan

1. Define the supported-host contract and identify the Linux-only or POSIX-only seams that violate it.
2. Introduce or tighten platform services for directories, process launch, terminal backend, watcher backend, and host integration.
3. Implement the missing macOS and Windows backends in coherent slices, starting with services that unblock launch and core workflows.
4. Add packaging, launch instructions, and targeted CI coverage for macOS and Windows so supported-host claims are verified continuously.

## Open Questions

- Which Windows terminal backend should be the first supported path: ConPTY-only, or a fallback strategy for older hosts?
- Should the first Windows and macOS pass include native secret storage, or should that remain follow-up work once launch, terminal, and watcher behavior are solid?
- How much target-specific tool packaging belongs in the first supported-host slice versus a later tooling-focused change?
