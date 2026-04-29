# Host Services Guide

Purpose: define the durable rules for project, platform, plugin, and other host-owned service layers in `microide`.

## Quick Scan

- Service code owns integration with the filesystem, subprocesses, git, PTYs, file watching, and plugin runtime.
- Service code must not depend on shell rendering details.
- Prefer narrow request and result types over passing broad mutable objects through the stack.
- Asynchronous execution is a tool, not the default. Add it when a real workload justifies the complexity.
- Keep errors, path handling, and tool invocation explicit.

## Service Boundaries

Typical service homes:

- `src/project/*` for file operations, search, git, blame, compare, indexing, and project-root logic
- `src/platform/*` for filesystem helpers, subprocesses, app directories, and file watching
- `src/plugin/*` and focused workspace runtime helpers for plugin lifecycle and contribution plumbing

Rules:

- Do not leak render or widget concerns into these services.
- Do not let services depend on `WorkspaceShell` when a narrower callback or state dependency would do.
- Keep external command construction, path normalization, parser logic, and retry or fallback behavior inside the service boundary that owns it.

Workspace-specific rules:

- `WorkspaceShell` should own service instances and route events, not own broad subsystem behavior.
- Coordinators should consume narrow service interfaces (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PersistenceService`) instead of `WorkspaceShell&`.
- Workspace persistence file I/O should route through `PersistenceService`; ad hoc direct reads or writes for workspace/session/config/conversation state are not allowed.

## Interface Design

- Prefer small structs for requests and results when a service has more than one input or output field.
- Prefer explicit callback bundles over hidden singleton access.
- Prefer value returns when ownership is simple and obvious.
- Use owning pointers only when lifetime or polymorphism actually requires them.
- If a service exposes mutable state, make the ownership and mutation rules explicit.

## Error Handling

- Make failure modes visible in the interface.
- Distinguish user-facing operational failures from programmer errors and invariant violations.
- Validate external inputs such as paths, process arguments, and parsed tool output near the boundary that receives them.
- Include enough structured context for callers to produce a clear message without re-parsing the failure.

## Filesystem And Process Rules

- Use `std::filesystem::path` at integration boundaries instead of raw strings for file paths.
- Keep shell-command construction narrow and explicit. Prefer argv-style execution helpers over stringly shell invocation when possible.
- Normalize path assumptions in one place rather than scattering equivalent helpers across the UI.
- When integrating with external tools, keep vendor-specific parsing and quirks inside the owning service.

## Sync And Async Policy

- Start synchronous when the workload is short, deterministic, and on a non-sensitive path.
- Add asynchronous execution when work would block typing, scrolling, redraw, startup, or other latency-sensitive flows.
- Keep async boundaries narrow: one owner for task lifetime, one result model, and one wakeup or callback path.
- Avoid speculative background systems. Add them when an actual plugin or built-in workflow needs them.

## Testing Expectations

- Service tests should exercise the real owned boundary: parsing, path handling, error mapping, and side effects that matter to callers.
- Prefer focused fixtures and temporary directories over broad integration harnesses when possible.
- If a service is difficult to test without the shell, the boundary is probably too wide.
