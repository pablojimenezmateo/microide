# AGENTS

## Mission

Build `microide` as the fastest, lowest-footprint AI-focused desktop IDE — a compact C++/SDL3
single-window application with no GPU acceleration requirement, keyboard-first workflows,
best-in-class diff and merge, and AI as a built-in first-class capability.

Do not optimize for keeping old boundaries alive. If the correct fix breaks compatibility, touches
many files, or requires a broader refactor, prefer the better design.

## Product Pillars

MicroIDE's durable built-in capabilities are:

- **Editor** — text editing, UTF-8, IME, undo/redo, syntax highlighting, blame, splits
- **Diff & Merge** — compare tabs (HEAD, commits, outgoing), three-way merge tabs, shared
  row-decoration pipeline; see `openspec/specs/diff-merge-editor/spec.md` for the contract
- **Search** — async project search (literal + regex), replace-in-project, file finder
- **Git** — working-tree changes, staging, conflicts, outgoing files, blame
- **Terminal** — PTY-backed tabs with scrollback, alternate screen, full ANSI support
- **AI** — host-owned chat pane, ghost-text inline completion, MCP tool execution, provider
  bridges; see `openspec/specs/ai-workflows/spec.md` for the contract
- **Plugins** — Lua 5.4 runtime with narrow host-owned registries for commands, sidebars,
  settings, keybindings, status items, diagnostics, hover, formatters, tasks, tools, tests,
  SCM, auth, and AI providers

The authoritative product thesis, priority order, and non-goals live in
`openspec/specs/product-vision/spec.md`.

## Priority Order

When tradeoffs conflict, use this order:

1. correctness
2. speed
3. low CPU usage
4. low memory usage
5. maintainability and simplicity
6. compatibility only when it is explicitly required

Compatibility is not a default constraint. Internal APIs, temporary abstractions, and stale call
patterns can be broken or removed if that is the cleanest way to improve the system.

## Default Engineering Stance

- Prefer correct behavior over minimal diffs.
- Prefer cohesive refactors over local patches that preserve bad architecture.
- It is fine to touch many files when the change crosses ownership boundaries in reality.
- Delete dead code, stale docs, and temporary compatibility shims as soon as the new path is established.
- Keep the host small and explicit; registries and services are better extension points than giant mutable objects.

## Plugin Rules

- Do not expose `WorkspaceShell` wholesale to plugins.
- Prefer host-owned registries, commands, sidebars, and services over ad hoc plugin hooks.
- Keep rendering host-owned; plugin contributions should provide data, commands, or structured requests.
- Add async plugin execution only when real plugin workloads justify it.
- If plugin work reveals that a subsystem boundary is wrong, fix the boundary instead of layering a compatibility adapter over it.

## Performance Rules

- Speed is the main optimization target after correctness.
- CPU comes before memory, especially idle CPU and redraw-path CPU.
- Measure before and after performance-sensitive changes.
- Use `docs/startup-tracing.md` and `docs/runtime-profiling.md` instead of guessing.
- Preserve typing, scrolling, resize, and startup responsiveness even when adding features.
- Prefer deleting redundant work over caching everything by default.
- The durable performance budget contract lives in `openspec/specs/performance-budgets/spec.md`.

## Architecture Rules

- Keep ownership narrow and obvious.
- Prefer small focused translation units over catch-all "shared" files.
- Push external tool and OS integration behind `src/project/*` or similarly narrow service boundaries.
- Keep UI orchestration thin; deterministic logic belongs in testable helpers.
- Avoid hidden coupling through mutable global state.
- If a coordinator grows because a subsystem lacks a real API, add the API and move logic out.

## Testing Rules

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before committing.
- Redraw comparison tests under SDL dummy video should run serially.
- Use focused fixtures for git, search, compare, merge, and plugin-adjacent workflows.
- If a change is hard to test, treat that as a design smell and improve the seam.

## Documentation Rules

- Keep `docs/active-work.md` current when priorities or shipped status change.
- Keep `docs/implementation-guide.md` aligned with durable product direction.
- Update subsystem design docs when a change materially alters the intended architecture.
- Remove stale or split-brain docs rather than leaving contradictory guidance around.
- When a durable policy changes, update the relevant `openspec/specs/` file in the same commit.

## Iteration Loop

The default loop is:

1. implementation
2. tests
3. docs
4. commit

Repeat in coherent slices. Do not leave tests, docs, or commits as an afterthought.

## Commit Discipline

- Prefer coherent commits over giant mixed snapshots.
- Each commit should represent a defensible step forward.
- If the right fix is broad, make it broad but still keep the commit logically unified.
- Mention major refactors plainly in the commit message instead of hiding them behind vague wording.
