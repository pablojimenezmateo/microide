# Agent Guide

Purpose: first-stop operating guide for agents working in this repository.

## Quick Scan

- `microide` is a native desktop IDE shell built in C++20 with CMake and SDL3.
- The current priority is correctness first, then speed, then low CPU usage.
- Plugin support is an active expansion phase; keep plugin seams narrow and host-owned.
- `AGENTS.md` owns repo policy, `docs/active-work.md` owns current direction, and `docs/implementation-guide.md` owns the durable product map.
- Build with `cmake`, test with `ctest`, and use focused `microide_tests` filters for quick validation.
- Prefer explicit ownership, deterministic helpers, and thin coordinators over broad mutable facades.

## Project Context

`microide` is a compact single-window desktop editor and IDE shell. The codebase currently centers on built-in editor, compare, merge, search, git, terminal, and plugin workflows.

- Language: C++20
- Build system: CMake
- Windowing and input: SDL3
- Optional text backend: SDL3_ttf
- Search engine: PCRE2
- Plugin runtime: Lua 5.4
- Terminal model: PTY-backed terminal sessions
- Test runner: CTest with the in-tree `microide_tests` binary

## Source Of Truth

When guidance conflicts, use this order:

1. `AGENTS.md`
2. `docs/active-work.md`
3. `docs/implementation-guide.md`
4. Focused subsystem docs in `docs/`
5. The handbook under `guidelines/`

## Agent Best Practices

- Start by narrowing the problem with fast repo inspection. Prefer `rg`, `rg --files`, `sed -n`, `git show`, and targeted reads over broad dumps.
- Match the repo's design direction instead of preserving stale boundaries. Broad refactors are acceptable when they improve correctness or subsystem ownership.
- Keep rendering host-owned. Plugins should contribute data, commands, providers, or structured requests, not raw shell internals.
- Avoid growing `WorkspaceShell` as a catch-all object. If work wants shell access, look for or add a narrower registry, coordinator, or service boundary.
- Prefer RAII, explicit ownership, and value semantics. Reach for inheritance only when there is a durable polymorphic boundary; otherwise prefer plain types, composition, and focused helpers.
- Keep deterministic logic out of SDL event glue and paint code when possible. Thin orchestration layers are easier to test and safer to refactor.
- Avoid hidden coupling through mutable global state, shared singleton-style services, or broad friend access.
- Treat performance-sensitive work as measurable engineering. Use `docs/startup-tracing.md` and `docs/runtime-profiling.md` instead of guessing.

## Development Workflow

Configure and build with the repo's CMake flow:

```bash
cmake -S . -B build/microide
cmake --build build/microide
```

Run the full automated test suite with:

```bash
ctest --test-dir build/microide --output-on-failure
```

Run focused tests with one or more substring filters:

```bash
./build/microide/microide_tests TextRenderer
./build/microide/microide_tests "WorkspaceShell/EditorDirty"
```

When a change affects performance-sensitive code, collect before-and-after evidence with the existing tracing and profiling docs.

## Architecture Stance

- Correctness beats compatibility unless compatibility is explicitly required.
- Plugin expansion should tighten host boundaries, not widen ad hoc shell access.
- Rendering, redraw policy, layout, and shell chrome remain host-owned.
- External tools and OS integration should stay behind narrow services in `src/project/*`, `src/platform/*`, or similarly focused modules.
- Coordinators should translate input or intent into state changes and service calls; they should not become long-lived ownership sinks.
- If a subsystem boundary is wrong, fix the boundary rather than adding a compatibility shim around it.

## Validation Expectations

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before considering work complete.
- Keep redraw comparison tests serial under SDL dummy video when they share global SDL state.
- Use focused fixtures for git, search, compare, merge, terminal, and plugin-adjacent workflows.
- Update docs when a durable architecture decision, workflow, or shipped capability changes.

## Related Docs

- `AGENTS.md`: repo policy, priorities, and iteration loop
- `docs/active-work.md`: current shipped baseline and active phases
- `docs/implementation-guide.md`: durable product and subsystem map
- `guidelines/architecture.md`: handbook summary of subsystem boundaries
- `guidelines/host-services.md`: service and integration rules
- `guidelines/ui-shell.md`: shell composition and render-path rules
- `guidelines/cpp.md`: C++ ownership and implementation guidance
- `guidelines/plugins.md`: plugin extension rules and boundaries
- `guidelines/performance.md`: profiling and performance expectations
- `guidelines/testing.md`: test strategy and validation loop
