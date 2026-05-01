## Context

The codebase audit identified WorkspaceShell as a 17 300-line god object spanning 40 translation units and a 1 542-line header with 71 direct includes, 224 private members, and 696 method declarations. Alongside this, five secondary issues compound the problem: a per-call heap allocation in `TextRenderer::TruncateToWidth`, an unbounded width cache, a `const_cast` in `AsyncSubprocess::IsRunning`, mouse coordinator headers that drag in the full shell header, and a callback-hell Operations pattern in coordinators that keeps WorkspaceShell as the mandatory orchestrator of all side effects.

The current architecture has one useful structural element already in place: `WorkspaceContext` aggregates most mutable product state (ProjectCatalogState, ProjectWorkspaceState, InteractionState, and their nested tab/overlay/chrome trees). Coordinators are already thin; they hold references to state slices and a bound Operations struct of function objects for side effects. The coordinator pattern is sound — the problem is that WorkspaceShell both owns the context and binds all the Operations callbacks into itself, making it impossible to decompose without breaking the callback lattice.

Breaking compatibility is acceptable. A full rewrite of WorkspaceShell into a compositor over self-owned pane types is the right long-term fix.

## Goals / Non-Goals

**Goals:**
- Decompose WorkspaceShell into 5–6 self-owned pane or service types (`EditorPane`, `TerminalPane`, `ComparePane`, `SidebarPane`, `PluginService`, and a thin `WorkspaceShell` compositor), each holding its own coordinator, state slice, and render logic.
- Replace the Operations-lambda callback pattern (40+ bound lambdas per coordinator) with direct state references where the called code belongs in the pane, reducing inter-pane coupling to WorkspaceContext queries.
- Eliminate the per-call `std::vector` allocation in `TextRenderer::TruncateToWidth` and bound the width cache.
- Fix `AsyncSubprocess::IsRunning` const unsoundness.
- Reduce WorkspaceShell.h to a minimal compositor header (under 20 direct includes) by pushing includes into pane headers and their .cpp files.
- Maintain all existing test coverage; existing public-API tests should continue to pass against the new compositor surface.

**Non-Goals:**
- Changing any plugin extension API, LSP/DAP protocol, or user-visible behaviour.
- Rewriting WorkspaceContext or the state trees inside it — they are a good aggregator and stay as-is.
- Changing the SDL event loop, OS integration, or render backend.
- Splitting the test files to mirror the new pane structure (tests can remain coarse-grained against the compositor API).

## Decisions

### 1. WorkspaceShell becomes a thin compositor over self-owned panes

The five pane types and one service type own their coordinator, their slice of WorkspaceContext state, their render logic, and their direct service dependencies:

| Type | Owns | Renders |
|---|---|---|
| `EditorPane` | TabCoordinator, EditorMouseCoordinator, EditorViewRenderer, blame/hover state | editor surface, editor overlays |
| `ComparePane` | CompareInteractionCoordinator, DiffTabCoordinator, CompareMouseCoordinator, MergeMouseCoordinator | compare and merge surfaces, scrollbars |
| `TerminalPane` | TerminalSession map, PanelMouseCoordinator, OutputChannels | bottom panel, terminal tabs |
| `SidebarPane` | SidebarCoordinator, SidebarMouseCoordinator, ProjectFileMonitor | sidebar surface |
| `ChromePane` | TabCoordinator (tab strip), MenuCoordinator, KeyInputCoordinator, TextInputCoordinator, PromptCoordinator | title bar, tab strip, menus, prompts, dirty prompts |
| `PluginService` | WorkspacePluginRuntime, LspManager, DapManager, all provider registries | (none — service only) |

`WorkspaceShell` retains:
- SDL window and renderer ownership
- `WorkspaceContext` ownership (shared state bus passed by reference into all panes)
- Top-level event dispatch (`HandleEvent` → routes to correct pane)
- Top-level render dispatch (`Render` → calls each pane's `Render` in order)
- Lifecycle (`Initialize`, `Shutdown`, `PrepareRenderFrame`)
- Theme and TextRenderer (shared rendering resources, injected into panes)

Why this boundary: coordinators already take state references and Operations callbacks. Moving the Operations callbacks into the pane itself — since the pane now owns the same state — eliminates most of the callback lattice. Cross-pane calls (e.g. opening a compare tab from the sidebar) go through `WorkspaceContext` state mutations that the other pane observes on its next update, not through direct shell method calls.

Alternatives considered:
- Keep WorkspaceShell as owner, only extract rendering: rejected because the render functions depend on the same state the coordinator mutates; separating render without separating state produces the same header coupling.
- Full subsystem inversion (each pane owns its WorkspaceContext slice): rejected because WorkspaceContext is already well-structured and its persistence format is tied to the whole-context layout.

### 2. Replace Operations callback structs with direct coordinator→pane method calls

Currently each coordinator receives an Operations struct with 20–40 bound lambdas that call back into WorkspaceShell. Once each pane owns the coordinator and the relevant state, the coordinator can call pane methods directly. The Operations struct becomes a much smaller set of cross-pane callbacks (e.g. `open_in_editor`, `open_compare_tab`) injected only where the pane boundary is genuinely crossed.

This does not change the coordinator's public interface — it changes where the lambda bodies live.

Alternatives considered:
- Keep the Operations struct unchanged: acceptable for small coordinators, but the 40-callback KeyInputCoordinator and 20-callback SidebarCoordinator generate too much boilerplate and keep the shell as a required orchestrator.

### 3. TextRenderer allocation fix: thread-local scratch buffer + capped cache

`TruncateToWidth` allocates `std::vector<std::size_t> boundaries` on every call. Replace with a `thread_local std::vector<std::size_t>` that is cleared and reused; shrink it back when its capacity exceeds 1 024. Cap the mutable width cache at 4 096 entries and reset it when the cap is reached.

### 4. AsyncSubprocess const fix

Remove `const` from `AsyncSubprocess::IsRunning()`. Fix call sites by compile-error. No behavioural change.

### 5. WorkspaceShell.h include reduction follows from pane extraction

Once render and coordinator logic moves into pane headers and .cpp files, WorkspaceShell.h only needs to declare the five pane types (forward-declared) and own the SDL and context members. The 71-include header becomes a ~15-include compositor header automatically, not as a separate manual cleanup pass.

## Risks / Trade-offs

- [Risk] Pane extraction requires touching all 40 WorkspaceShell .cpp files simultaneously. → Mitigation: extract one pane at a time, keeping the others as WorkspaceShell methods until the full set is done; compile and test after each pane.
- [Risk] Some coordinator Operations callbacks call into logic that genuinely crosses pane boundaries (e.g. opening an editor tab from the git sidebar). → Mitigation: keep a minimal set of cross-pane callbacks in each pane's constructor; these become the explicit seams.
- [Risk] WorkspaceShellTesting.h accesses private members via friendship. → Mitigation: tests use only the public API per the audit; the test access pattern can be maintained on the new compositor surface.
- [Risk] PluginService couples to all panes through notification callbacks. → Mitigation: plugin notifications go through WorkspaceContext state mutations (buffer open/close events) that panes observe; direct callbacks are limited to diagnostics push, which goes to EditorPane.

## Migration Plan

1. Fix TextRenderer and AsyncSubprocess (isolated, no structural impact).
2. Extract `TerminalPane` first — it is the most isolated pane with the least cross-pane callbacks.
3. Extract `SidebarPane` — depends on context queries only; sidebar→editor cross-pane calls reduced to `open_in_editor` callback.
4. Extract `ComparePane` — takes CompareInteractionCoordinator, DiffTabCoordinator, and both mouse coordinators with it.
5. Extract `EditorPane` — takes TabCoordinator (editor half), EditorMouseCoordinator, EditorViewRenderer.
6. Extract `ChromePane` — takes KeyInputCoordinator, TextInputCoordinator, MenuCoordinator, PromptCoordinator.
7. Extract `PluginService` — takes WorkspacePluginRuntime, LspManager, DapManager, registries.
8. What remains in WorkspaceShell is the compositor. Remove dead code and trim the header.
9. Run full ctest suite; update docs.

## Open Questions

- Should `WorkspaceContext` be passed to panes as a mutable reference or should each pane own a typed accessor/view? (Lean toward mutable reference for now — WorkspaceContext is already stable and accessor types add abstraction with no present benefit.)
- ChromePane owns key input dispatch, but key events currently route through WorkspaceShell::HandleEvent to call into all panes. Should HandleEvent remain on the compositor and dispatch to panes, or should ChromePane own the dispatch logic? (Lean toward compositor dispatch — it keeps the SDL glue in one place and panes stay passive consumers.)
