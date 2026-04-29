## 1. Foundations And Lint

- [x] 1.1 Add `util/Parse.{h,cpp}` with non-throwing `ParseInt`, `ParseInt64`, `ParseSize`, `ParseFloat` over `std::from_chars`, plus focused unit tests covering edge cases (empty, leading/trailing whitespace, overflow, negative for unsigned).
- [x] 1.2 Replace every `try { std::sto* } catch (...)` site in `src/` with the new helpers; delete `ParseSizeToken`/`ParseFloatToken`/`ParseIntToken`/`ParseInt64Token` from `WorkspacePersistenceFormat.cpp` and equivalent helpers elsewhere.
- [x] 1.3 Add an architectural-lint test under `microide_tests` (`ArchitectureInvariants` fixture) that walks `src/` and fails on: any `friend class`/`friend struct` in `src/workspace/*`, any constructor in `src/workspace/Workspace*Coordinator*.h` taking `WorkspaceShell&`/`WorkspaceShell*`, any `try` block in `src/` whose body contains `std::sto`, any single `src/plugin/*.cpp` translation unit > 800 lines, `src/workspace/WorkspaceShell.h` > 400 lines, or `src/workspace/WorkspaceShell.cpp` > 600 lines. Mark each invariant as a "soft fail" initially (warns until subsequent steps land), then flip each to hard-fail in the step that satisfies it.
- [x] 1.4 Wire the lint test into the default `ctest` run; document the invariants in `guidelines/architecture.md`.

## 2. Single-Line Editor Model

- [x] 2.1 Add `editor/SingleLineEditor.{h,cpp}` (buffer + caret + optional selection + standard ops) and `editor/SingleLineKeyHandler.{h,cpp}` for shared key dispatch.
- [x] 2.2 Add a focused regression suite for insert, backspace, delete-forward, left/right movement, home/end, select-all, copy, cut, paste, and selection-range invariants.
- [x] 2.3 Migrate prompt input, command input, overlay query field, and sidebar search field to the shared model; delete per-surface duplicates of those operations. Keep the chat composer on `editor::TextViewport` in this change and track its shared-input alignment separately.
- [ ] 2.4 Capture `MICROIDE_TRACE_REDRAW` evidence on a representative session covering each migrated surface; attach to the change record.

## 3. Persistence Format Foundation

- [x] 3.1 Add `persistence/PersistedRecord.{h,cpp}` defining the magic header, version, capability flags, CRC32C, typed primitives (`u32`, `i32`, `i64`, `f32`, `bool`, `string`, `path`, `vec`, `optional`), and the tag-length-payload record framing.
- [x] 3.2 Add `persistence/PersistedRecordReader.{h,cpp}` and `persistence/PersistedRecordWriter.{h,cpp}` with atomic-write semantics (`tmp + fsync + rename`), CRC verification on read, and `<file>.bak` fallback. Include round-trip tests and a corruption-recovery test.
- [x] 3.3 Define typed records for `ProjectWorkspaceState`, `UserConfigState`, `WorkspaceSession`, and `ConversationRegistry`. Each becomes a `Encode`/`Decode` pair against the shared reader/writer; no bespoke parser remains.
- [x] 3.4 Add a debug `microide-dump-state` subcommand that prints any persisted file in stable text form for `diff`-friendly inspection.

## 4. Persistence Cutover

- [x] 4.1 Land a one-shot importer that reads any pre-existing legacy text-format files (`project.state`, `user.config`, `session.workspace`, `chat.conversations`), writes the equivalent structured file via the atomic writer, verifies the new file by re-reading and CRC-checking it, and only then renames the legacy file to `<name>.legacy`.
- [x] 4.2 Delete `WorkspacePersistenceFormat.{h,cpp}` legacy reader code and any `WorkspacePersistenceCoordinator*` paths that depend on it; replace with calls into the new `PersistenceService` (introduced in step 5).
- [x] 4.3 Capture `MICROIDE_STARTUP_TRACE` before/after on a representative project; confirm load step is no slower than the legacy text-format load.

## 5. PersistenceService

- [x] 5.1 Add `workspace/PersistenceService.{h,cpp}` as the single owner of disk I/O for workspace state, config, session, and conversation files; expose narrow load/save APIs typed against the encode/decode pairs from step 3.
- [x] 5.2 Reroute every existing caller of persistence flows through the new service; remove any direct file I/O for these artifacts from `WorkspaceShell` and `WorkspacePersistenceCoordinator*`.
- [x] 5.3 Add fixtures covering atomic write under simulated crash (write-then-truncate-tmp, write-then-die-before-rename) and CRC failure with valid `.bak` fallback.

## 6. EditorTabService And Active-Viewport Cleanup

- [x] 6.1 Add `workspace/EditorTabService.{h,cpp}` owning tab list, active index, splits, dirty state, and view restore. Move tab-mutating logic out of `WorkspaceShell`/`WorkspaceShellTooling`.
- [x] 6.2 Remove `current_project_state_.text_viewport` and the `text_viewport_` alias entirely. Move welcome/placeholder responsibilities to a dedicated `WelcomeSurface` model.
- [x] 6.3 Migrate every editor-action caller to `EditorTabService::ActiveViewport()` (or equivalent typed accessor). Verify with `grep` that no `text_viewport_` reference remains.
- [x] 6.4 Rewrite `WorkspaceTabCoordinator`, `WorkspacePathMutationCoordinator*`, and `WorkspaceDirtyPromptCoordinator` to take `EditorTabService&` (and `PromptSurfaceService&` from step 7); remove the `WorkspaceShell&` parameter.
- [x] 6.5 Capture `MICROIDE_TRACE_REDRAW` and `MICROIDE_STARTUP_TRACE` evidence; attach to change record.

## 7. PromptSurfaceService, SidebarService, ProjectCatalogService

- [x] 7.1 Add `PromptSurfaceService` owning prompt + dirty-prompt + path-mutation prompt lifecycle. Migrate `WorkspaceCommandPromptCoordinator`, `WorkspaceDirtyPromptCoordinator`, and `WorkspacePathMutationCoordinator*` against it.
- [x] 7.2 Add `SidebarService` owning sidebar mode, refresh, and open-or-select. Migrate `WorkspaceSidebarCoordinator*` against it; remove direct shell access from sidebar mouse coordinators.
- [x] 7.3 Add `ProjectCatalogService` owning open projects, switch, close, project-state activation. Migrate `WorkspaceProjectCatalogCoordinator` and `WorkspaceProjectStateCoordinator` against it; route `LoadProjectState`/`StoreCurrentProjectState` through this service.
- [ ] 7.4 For each of 7.1–7.3, capture `MICROIDE_TRACE_REDRAW` evidence on representative sessions; attach to change record.

## 8. CompareMergeService, TerminalPanelService, Mouse Coordinators

- [x] 8.1 Add `CompareMergeService` owning compare/merge tab orchestration and navigation commands. Migrate `WorkspaceCompareInteractionCoordinator`, `WorkspaceCompareMouseCoordinator`, `WorkspaceMergeMouseCoordinator`, and `WorkspaceDiffTabCoordinator` against it.
- [x] 8.2 Add `TerminalPanelService` owning terminal tabs, focus, and panel layout requests. Migrate `WorkspacePanelMouseCoordinator` and the terminal-side bits of `WorkspaceLayout` consumers.
- [ ] 8.3 Migrate the remaining mouse coordinators (`WorkspaceChromeMouseCoordinator`, `WorkspaceEditorMouseCoordinator`, `WorkspaceTabMouseCoordinator`, `WorkspaceSidebarMouseCoordinator`) to consume only the relevant services; remove `WorkspaceShell&` from every coordinator constructor.
- [x] 8.4 Flip the lint rule for `WorkspaceShell&` in coordinator constructors from soft-fail to hard-fail.

## 9. Render View Models

- [ ] 9.1 Add `workspace/RenderViewModelBuilder.{h,cpp}` and a typed `<Surface>ViewModel` struct per render surface. Populate from service queries.
- [ ] 9.2 Migrate `WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`, `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`, and `WorkspaceShellHoverTargets` to consume their view-model structs only.
- [ ] 9.3 Verify with the lint that no render translation unit calls `WorkspaceShell` member functions other than the view-model builder entry. Flip lint to hard-fail.
- [ ] 9.4 Capture `MICROIDE_TRACE_REDRAW` for typing, scrolling, and merge-tab scrolling; confirm no regression vs. baseline.

## 10. Plugin Host Decomposition

- [ ] 10.1 Carve out `plugin/LuaRuntime.{h,cpp}` as the sole owner of `lua_State*` lifecycle (create/suspend/destroy, pcall wrapping, error capture). All other plugin code consumes it through opaque handles.
- [ ] 10.2 Split `PluginHost.cpp` into per-surface modules: `PluginCommandRegistry`, `PluginSidebarRegistry`, `PluginSyntaxRegistry`, `PluginDiagnosticsRegistry`, `PluginHoverRegistry`, `PluginProviderRegistry`, `PluginLifecycle`. Each owns its registry, exposes a narrow API, and stays ≤ 800 lines.
- [ ] 10.3 Reduce `PluginHost` to a thin coordinator that holds the runtime and the registry set, exposing only what plugin lifecycle and the workspace-side `PluginRuntimeService` actually need.
- [ ] 10.4 Capture `MICROIDE_STARTUP_TRACE` covering plugin load + first idle frame; confirm no regression. Flip the lint rule for `src/plugin/*.cpp` size to hard-fail.

## 11. WorkspaceShell Final Slim And God-Class Removal

- [ ] 11.1 Move all remaining state and behavior off `WorkspaceShell` into the appropriate service. The shell's final responsibility is to own service instances and route input events.
- [ ] 11.2 Delete `WorkspaceShellTooling.cpp`, `WorkspaceShellTooling.h`, and `WorkspaceActionContext.{h,cpp}` once their contents are absorbed into services. Rewrite `WorkspaceActionCoordinator` against the service interfaces directly.
- [ ] 11.3 Delete `WorkspaceShellTesting.h`; rewrite test fixtures to drive services directly.
- [ ] 11.4 Verify `WorkspaceShell.h` ≤ 400 lines, `WorkspaceShell.cpp` ≤ 600 lines, zero `friend` declarations. Flip the corresponding lint rules to hard-fail.

## 12. Full-Tree Validation And Documentation

- [ ] 12.1 Run `cmake --build build/microide` clean and `ctest --test-dir build/microide --output-on-failure`; resolve any flake.
- [ ] 12.2 Run startup-trace and runtime-profiling capture per `docs/startup-tracing.md` and `docs/runtime-profiling.md` end-to-end. Compare against the pre-cleanup baseline; verify all `performance-budgets` requirements still hold.
- [ ] 12.3 Update `docs/active-work.md` to reflect the new architecture as the shipped baseline. Close items 1, 2, 3, 4, and 7 in `docs/known-tech-debt.md`.
- [ ] 12.4 Update `guidelines/architecture.md`, `guidelines/host-services.md`, `guidelines/cpp.md`, and `guidelines/plugins.md` to describe the service-oriented model, the single-line editor model, the persistence format, and the lint invariants.
- [ ] 12.5 Schedule the `.legacy`-file cleanup as a follow-up change for the release after next; do not delete `.legacy` files in this change.

## 13. Final Status

- [ ] 13.1 Run `openspec status --change "comprehensive-tech-debt-cleanup"` and confirm all artifacts are done; archive the change.
