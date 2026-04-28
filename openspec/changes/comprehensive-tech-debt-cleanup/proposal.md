## Why

MicroIDE's workspace layer has grown into a small set of god-class objects (`WorkspaceShell`, `WorkspaceShellTooling`, `PluginHost`, `WorkspaceActionContext`) that own most state and mutation, while ~21 "coordinators" reach back into them through shell back-references and friend-style access rather than narrow contracts. The result is sparse, partly duplicated state (viewport ownership split between `current_project_state_.text_viewport` and `ActiveEditorViewport()`, single-line editing logic duplicated across 5+ surfaces, ~22 hand-rolled numeric parse sites), a brittle command-style text format for project state and preferences, and an architecture where new plugin-platform work keeps adding helpers and friends instead of stable seams. Correctness and performance gains from the recent passes risk eroding unless the underlying ownership model is fixed now, while the project explicitly allows breaking compatibility.

## What Changes

- **BREAKING** Replace the god-class workspace layer with a small set of host-owned services and value-typed state models. `WorkspaceShell` becomes a thin orchestrator (target ≤ 400 lines header, zero `friend class` declarations); `WorkspaceShellTooling`, `WorkspaceActionContext`, and `WorkspaceShellTesting` are dissolved into focused services or removed.
- **BREAKING** Replace coordinator-with-shell-back-reference pattern. Coordinators consume narrow injected service interfaces (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `PersistenceService`, `SidebarService`, `CompareMergeService`, `PluginRuntimeService`, etc.) instead of holding `WorkspaceShell&`. Mutating shell internals from outside the shell is no longer possible.
- **BREAKING** Decompose `PluginHost` (5074 LoC) into a runtime core plus extension-surface modules (commands, sidebars, syntax, diagnostics, hover, providers, lifecycle), each owning its own registry, with the Lua VM lifecycle isolated behind a single seam.
- **BREAKING** Replace the text-command-based project state, user config, and session-restore format with a single schema-versioned, structured persistence format (typed records, explicit version field, forward/backward compatibility rules, atomic write + checksum, deterministic round-trip). Drop the per-token `std::stoull`/`stoll`/`stof`/`stoi` `try`/`catch` parsing pattern from all call sites in favor of a shared typed token reader.
- **BREAKING** Collapse the dual viewport ownership: `current_project_state_.text_viewport` is removed; the active editor viewport is always tab-owned. Welcome/placeholder state moves to a dedicated welcome model.
- Introduce a single shared `SingleLineEditor` model (buffer + caret + selection + standard edit ops) consumed by prompt input, command input, chat composer, overlay query fields, and sidebar search fields, replacing the per-surface backspace/movement/copy/cut/select-all duplicates.
- Consolidate numeric, path, and token parsing helpers into one `util/Parse.*` layer with non-throwing `std::from_chars`-based primitives; remove `try`/`catch` numeric parsing throughout the tree.
- Tighten render-path inputs: per-surface render functions take explicit view-model structs, not the shell. Hover, overlay, and chrome render code stops querying shell helpers at draw time.
- Reduce private surface area on shared types: replace large `friend class` lists with explicit narrow accessor APIs or move state to the consuming service.
- Bake architectural invariants into the build: a CI lint step rejects new `friend class` declarations in workspace code, new `WorkspaceShell&` parameters in coordinators, and new `try`/`catch` numeric parsing.
- **BREAKING** All existing on-disk project state, preferences, and session files are migrated once at first launch and rewritten in the new format; the old text-command parser is deleted (no compatibility shim).

## Capabilities

### New Capabilities

- `workspace-architecture`: durable contract for workspace-layer ownership — service-oriented decomposition, no god classes, no friend-based coupling, render inputs as explicit view models, coordinators as input-to-intent translators only.
- `persisted-state-format`: schema-versioned, structured on-disk format for project state, user preferences, workspace sessions, and chat conversations, with atomic write semantics, integrity checks, and a single typed reader/writer.
- `shared-edit-primitives`: host-owned reusable primitives (single-line editor model, typed token parser, view-model structs) that eliminate the duplicated single-line editing and parsing code paths.

### Modified Capabilities

- `performance-budgets`: add a non-regression requirement that the architecture overhaul SHALL preserve or improve the existing typing, scrolling, idle CPU, startup, and large-file budgets, validated with `MICROIDE_STARTUP_TRACE` and `MICROIDE_TRACE_REDRAW` evidence captured before and after the rewrite of each affected subsystem.

## Impact

- Affected code: every file under `src/workspace/*` (especially `WorkspaceShell.{h,cpp}`, `WorkspaceShellTooling.cpp`, `WorkspaceActionContext.{h,cpp}`, every `Workspace*Coordinator*`, `WorkspacePersistenceFormat.{h,cpp}`, `WorkspacePersistenceCoordinator*`), `src/plugin/PluginHost.{h,cpp}`, `src/util/SingleLineText.{h,cpp}` (replaced by `SingleLineEditor`), and the render-path files (`WorkspaceShellRender*.cpp`).
- Affected APIs: internal-only — no public C ABI exists; plugin-facing Lua APIs are unchanged.
- Affected on-disk artifacts: project state files, preferences, session restore, conversation persistence — all migrated once and rewritten under the new format. Old files are read by a one-shot importer that is removed after the next minor release.
- Affected docs: `AGENTS.md`, `docs/active-work.md`, `docs/implementation-guide.md`, `docs/known-tech-debt.md` (items 1–4 and 7 closed), `guidelines/architecture.md`, `guidelines/host-services.md`, `guidelines/cpp.md`, `guidelines/plugins.md`.
- Affected tests: `microide_tests` workspace, plugin, persistence, and render fixtures rewritten against the new service interfaces; new persistence-format round-trip and migration coverage; new lint test for architectural invariants.
- Risk: large surface; mitigated by sequencing the work as a tree of independent service extractions, each landing with before/after performance evidence and full test runs.
