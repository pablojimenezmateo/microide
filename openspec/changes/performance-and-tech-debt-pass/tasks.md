## 1. Isolated Fixes (No Structural Impact)

- [ ] 1.1 Replace the per-call `std::vector<std::size_t> boundaries` allocation in `TextRenderer::TruncateToWidth` with a `thread_local` scratch buffer; shrink when capacity exceeds 1 024.
- [ ] 1.2 Cap the `TextRenderer` mutable width cache at 4 096 entries and clear it on overflow; add a focused test that exercises truncation in a tight loop to confirm no per-call allocation.
- [ ] 1.3 Remove `const` from `AsyncSubprocess::IsRunning()`, eliminating the `const_cast`; fix all call sites by compile-error.

## 2. Extract TerminalPane

- [ ] 2.1 Create `src/workspace/TerminalPane.h` and `TerminalPane.cpp`; move terminal session map, `OutputChannels`, `TerminalSelection`, `PanelMouseCoordinator`, and all terminal-related private members out of `WorkspaceShell`.
- [ ] 2.2 Move `WorkspaceShellTerminal.cpp`, the terminal portion of `WorkspaceShellRedraw.cpp`, and the bottom-panel render logic into `TerminalPane`; make `TerminalPane::Render(...)` the single entry point for bottom-panel rendering.
- [ ] 2.3 Wire `TerminalPane` into `WorkspaceShell`: store as a member, delegate `HandleEvent` terminal routes and bottom-panel `Render` call to it; confirm build and tests pass.

## 3. Extract SidebarPane

- [ ] 3.1 Create `src/workspace/SidebarPane.h` and `SidebarPane.cpp`; move `SidebarCoordinator`, `SidebarMouseCoordinator`, sidebar state members, and `ProjectFileMonitor` into it.
- [ ] 3.2 Move `WorkspaceShellSidebar.cpp` and `WorkspaceShellRenderSidebar.cpp` logic into `SidebarPane`; reduce the SidebarCoordinator Operations struct to only the cross-pane callbacks (`open_in_editor`, `open_compare_tab`, `open_git_merge`).
- [ ] 3.3 Wire `SidebarPane` into `WorkspaceShell`; confirm build and tests pass.

## 4. Extract ComparePane

- [ ] 4.1 Create `src/workspace/ComparePane.h` and `ComparePane.cpp`; move `CompareInteractionCoordinator`, `DiffTabCoordinator`, `CompareMouseCoordinator`, `MergeMouseCoordinator`, and compare/merge state members into it.
- [ ] 4.2 Move `WorkspaceShellCompare.cpp`, `WorkspaceShellCompareRender.cpp`, `WorkspaceShellMergeState.cpp`, and `WorkspaceShellMergeRender.cpp` logic into `ComparePane`.
- [ ] 4.3 Wire `ComparePane` into `WorkspaceShell`; confirm build and tests pass.

## 5. Extract EditorPane

- [ ] 5.1 Create `src/workspace/EditorPane.h` and `EditorPane.cpp`; move `TabCoordinator` (editor half), `EditorMouseCoordinator`, `EditorViewRenderer`, blame and hover state, and all editor-specific members into it.
- [ ] 5.2 Move `WorkspaceShellEditor.cpp`, `WorkspaceShellCursor.cpp`, `WorkspaceShellHoverPopup.cpp`, and the editor-surface render logic into `EditorPane`.
- [ ] 5.3 Wire `EditorPane` into `WorkspaceShell`; confirm build and tests pass.

## 6. Extract ChromePane

- [ ] 6.1 Create `src/workspace/ChromePane.h` and `ChromePane.cpp`; move `KeyInputCoordinator`, `TextInputCoordinator`, `MenuCoordinator`, `TabCoordinator` (tab strip), `CommandPromptCoordinator`, `DirtyPromptCoordinator`, and chrome/menu state members into it.
- [ ] 6.2 Move chrome and tab-strip render logic and the text-input render logic into `ChromePane`; reduce `WorkspaceShellRenderTextInput.cpp` and `WorkspaceShellTooling.cpp` to what belongs in the pane.
- [ ] 6.3 Wire `ChromePane` into `WorkspaceShell`; confirm build and tests pass.

## 7. Extract PluginService

- [ ] 7.1 Create `src/workspace/PluginService.h` and `PluginService.cpp`; move `WorkspacePluginRuntime`, `LspManager`, `DapManager`, and all provider registries (formatter, completion, code-action, task, tool) into it.
- [ ] 7.2 Replace direct shell plugin-notification calls with WorkspaceContext state mutations (buffer open/close events) that panes observe; keep only the diagnostics push as a direct `EditorPane` callback from `PluginService`.
- [ ] 7.3 Wire `PluginService` into `WorkspaceShell`; confirm build and tests pass.

## 8. Slim WorkspaceShell To Compositor

- [ ] 8.1 Delete all WorkspaceShell .cpp files whose logic has moved into panes; remove the corresponding dead declarations from `WorkspaceShell.h`.
- [ ] 8.2 Reduce `WorkspaceShell.h` to: SDL ownership, `WorkspaceContext`, pane member declarations (forward-declared where possible), Theme, TextRenderer, and the public API (`Initialize`, `Shutdown`, `HandleEvent`, `PrepareRenderFrame`, `Render`). Target under 20 direct includes.
- [ ] 8.3 Create `src/workspace/WorkspaceLayoutTypes.h` for any residual layout structs still referenced across pane boundaries; update includes accordingly.

## 9. Validation And Documentation

- [ ] 9.1 Run the full `ctest --test-dir build/microide --output-on-failure` suite and confirm zero regressions.
- [ ] 9.2 Run the host-facing slice (`microide_tests AppDirectories RuntimePaths FileOperationService Subprocess TerminalSession FileWatcher`) and the workspace shell tests.
- [ ] 9.3 Update `docs/active-work.md` to record the decomposition as shipped; update `docs/implementation-guide.md` with the new pane ownership map.
- [ ] 9.4 Update `performance-budgets` spec with the render-path allocation and incremental build budget lines.
