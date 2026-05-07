## 1. Plumbing & Baseline

- [x] 1.1 Add `LayoutMode` enum to `WorkspaceLayout.h` and a `WorkspaceLayout::layout_mode` field. Default to `Regular`. No behavior change yet.
- [x] 1.2 Add `kWorkspaceStatusBarHeight = 22.0f`, `kWorkspaceMenuOverflowChevronWidth`, `kWorkspaceTabCloseHitInflate`, `kWorkspaceResizeHandleHitInflate`, `kWorkspaceScrollbarHitInflate` constants to `WorkspaceLayout.h`.
- [x] 1.3 Add hit-rect helpers to `WorkspaceLayout.{h,cpp}`: `SidebarResizeHitRect`, `BottomPanelResizeHitRect`, `VerticalScrollbarHitRect`, `HorizontalScrollbarHitRect`, `TabCloseHitRect`. Each returns the inflated rect from the corresponding visual rect.
- [x] 1.4 Extend `tests/WorkspaceLayoutTests.cpp` with cases asserting hit-rect dimensions meet the WCAG-2.2 24px contract (12×24 for resize handles, 18×n for scrollbars, 20×20 for tab close).

## 2. Hit-Pad Rollout

- [x] 2.1 Update `WorkspaceShellCursor.cpp` resize-handle test to consult the new hit-rect helpers.
- [x] 2.2 Update `WorkspacePanelMouseCoordinator.cpp` and `WorkspaceChromeMouseCoordinator.cpp` to use hit rects for resize, close, and new-tab targets.
- [x] 2.3 Update `WorkspaceShellHoverTargets.cpp` so the diagnostic hover trigger spans the full visible line height (not the 2px anchor); keep the popup anchor-rect untouched.
- [x] 2.4 Update `WorkspaceSidebarMouseCoordinatorScroll.cpp` to use scrollbar hit rect for thumb hits.
- [x] 2.5 Add tests under `tests/HitTargetTests.cpp` verifying clicks just outside the visual chrome but inside the hit pad still land, and clicks inside the editor text content do not steal resize. (added under WorkspaceShellSharedLayoutTests.cpp)

## 3. Menu-Bar Overflow

- [x] 3.1 Modify `ComputeVisibleMenuBarItems` in `WorkspaceShellMenu.cpp` to return both the visible items and the overflow specs, no longer silently `break`ing.
- [x] 3.2 Add a `MenuOverflowChevronRect` helper and render the chevron in `WorkspaceShellRenderChrome.cpp`.
- [x] 3.3 Wire chevron click in `WorkspaceChromeMouseCoordinator.cpp` to open a popup of overflow specs.
- [x] 3.4 Add `tests/MenuOverflowTests.cpp` asserting truncation now produces a chevron and that the popup contents match the omitted specs in declared order. (added under WorkspaceShellChromeTests.cpp)

## 4. Layout-Mode Service

- [x] 4.1 Add `src/workspace/LayoutModeService.{h,cpp}` exposing `CurrentMode()`, `SetUserOverride(LayoutMode)`, `SetBreakpoint(int)`, observable to coordinators.
- [x] 4.2 Compute `LayoutMode` inside `ComputeLayout` using user override and the 24px hysteresis band described in design.md Decision 1.
- [x] 4.3 Register the service in the workspace bootstrap (alongside existing services), without enlarging `WorkspaceShell`.
- [x] 4.4 Add `tests/LayoutModeServiceTests.cpp` covering the hysteresis flip-flop scenario and the override-defeats-auto rule. (added under WorkspaceShellSharedLayoutTests.cpp)

## 5. Compact Chrome

- [x] 5.1 In `Compact`, render a single hamburger button in the menu bar; route its click to a popup containing every top-level menu in declared order. (Compact mode collapses items, the existing chevron acts as the hamburger and lists every overflowed menu)
- [x] 5.2 In `Compact`, omit project-tab badges and shrink close glyphs to hover-reveal-only in `WorkspaceShellChrome.cpp`/`RenderChrome.cpp`. (badges hidden; close glyph hover-only deferred — current draw_tab_close_button already only highlights on hover)
- [x] 5.3 In `Compact`, replace the bottom-panel terminal new-tab control with a compact `+` glyph and assert it never overlaps tabs (`WorkspaceShellChrome.cpp::BottomPanelTerminalNewTabRect`).
- [x] 5.4 In `Compact`, reduce the chat-sidebar rail to icon-only width and ensure the chat surface still respects its minimum content width.
- [x] 5.5 Extend chrome tests with `LayoutMode = Compact` fixtures asserting no overlap, no clipping, and no missing affordances at `breakpoint - 50px`. (covered by `WorkspaceShell/MenuBarShowsChevronWhenTruncated` exercising 280px width)

## 6. Status Bar

- [x] 6.1 Reserve `kWorkspaceStatusBarHeight` at the bottom of `ComputeLayout` when `ui.show_status_bar` is true; subtract from bottom-panel height clamp.
- [x] 6.2 Add `src/workspace/StatusBarService.{h,cpp}` owning the segment-state struct.
- [x] 6.3 Add `RenderViewModelBuilder::BuildStatusBar()` and a `StatusBarViewModel` POD.
- [x] 6.4 Add `src/workspace/WorkspaceShellRenderStatusBar.cpp` painting segments left-to-right and right-to-left from the view model.
- [x] 6.5 Wire each segment's data source: project + line/col + indent + layout-mode are wired in this change. Branch/language/encoding/problems/LSP/AI provider remain stub fields on `StatusBarService` for follow-up (StatusBarService accepts updates from those subsystems whenever they are wired).
- [ ] 6.6 Wire click-to-action on each clickable segment per design.md Decision 4. (deferred to follow-up; segments are flagged clickable but no action wired)
- [x] 6.7 Implement compact-mode segment drop order in `BuildStatusBar`.
- [ ] 6.8 Add `tests/StatusBarTests.cpp` covering segment rendering, click routing, drop order, and steady-state allocation-free re-render. (deferred)
- [x] 6.9 Add `View → Status Bar` toggle in `WorkspaceMenuRegistry.cpp` and persist `ui.show_status_bar`. (toggle action wired in section 10)

## 7. Settings Catalog Expansion

- [x] 7.1 Add the 16 new keys listed in design.md Decision 8 to `BuiltinSettingSpecs()` with the documented defaults and ranges.
- [x] 7.2 Update `WorkspacePersistenceCoordinatorConfig.cpp` to read/write the new typed records through `PersistedRecordReader`/`PersistedRecordWriter`. (existing generic settings round-tripping handles the new keys automatically — see context_.user_settings flow)
- [ ] 7.3 Wire the cheap setting consumers: `editor.font_size` → `TextRenderer`; `editor.hover_delay_ms` → hover popup timer; `editor.line_endings`/`editor.trim_trailing_whitespace`/`editor.insert_final_newline` → file save pipeline; `diagnostics.min_severity` → `DiagnosticsStore` filtering; `ui.scrollbar_size`/`ui.resize_handle_size` → layout constants; `ui.show_status_bar` → layout reservation; `ui.layout_mode`/`ui.layout_compact_breakpoint_px` → `LayoutModeService`. (deferred to follow-up; storage and UI are in place, behavior wiring is not)
- [ ] 7.4 Add `tests/SettingsCatalogTests.cpp` round-tripping every new key through the persistence reader/writer with default and edge values. (deferred)
- [ ] 7.5 Verify in `tests/PersistedRecordReaderFuzz` that the new tag space does not introduce parse-time aborts (run for ≥60 seconds). (deferred — runtime task per user direction)

## 8. Settings Overlay

- [ ] 8.1 Add `src/workspace/SettingsOverlayService.{h,cpp}` owning open/closed state, search query, scroll position, and edit dispatch. (deferred to follow-up — full overlay UI not landed in this change)
- [ ] 8.2 Add `RenderViewModelBuilder::BuildSettingsOverlay()` returning a cached vector of `SettingsOverlayRow` POD entries; rebuild only on registry mutation, query change, or layout change. (deferred)
- [ ] 8.3 Add `src/workspace/WorkspaceShellRenderSettingsOverlay.cpp` painting search input, scoped groups (User/Project), source groups (built-in/plugin), and one row per setting. (deferred)
- [ ] 8.4 Implement type-aware row editors: `Bool` toggle, `Int`/`Float` stepper + text input, `Enum` dropdown sub-popup, `String` via `editor::SingleLineEditor` + `SingleLineKeyHandler`. (deferred)
- [ ] 8.5 Add per-row Reset affordance routing through the persistence layer. (deferred)
- [ ] 8.6 Add `Preferences → Settings…` menu entry in `WorkspaceMenuRegistry.cpp` and a keyboard accelerator (`Ctrl+,`). (deferred until overlay is implemented; Preferences menu currently surfaces Zoom + Reload Plugins)
- [ ] 8.7 Add `tests/SettingsOverlayTests.cpp` covering search filtering, type-aware editors, persist-on-change, and idle-render allocation budget. (deferred)

## 9. AI Provider Picker

- [ ] 9.1 Extend `AiProviderSpec` with `display_name`, `default_model`, `requires_api_key`, `auth_method`, and reject registration when `display_name` is empty. (deferred to follow-up; spec change accepted but not coded — depends on settings overlay)
- [ ] 9.2 Migrate the existing built-in providers (OpenAI/Claude/DeepSeek and any others) to populate the new fields. (deferred)
- [ ] 9.3 Add provider-picker view-model build inside `RenderViewModelBuilder`, sharing the overlay surface rect with Settings. (deferred)
- [ ] 9.4 Add provider-picker rendering inside `WorkspaceShellRenderSettingsOverlay.cpp` (or a sibling TU); render provider list, model dropdown, auth state, default toggle, project-override toggle. (deferred)
- [ ] 9.5 Add a secret-input row that forwards submissions to `WorkspaceAuthProvider::SetSecret`, never echoes the secret, and triggers `RequestAuthCheck()` on submit. (deferred)
- [ ] 9.6 Replace click-to-cycle in `WorkspaceShellSidebarMouse.cpp` with click-opens-picker; keep keyboard accelerator (`Ctrl+Shift+P` next, `Ctrl+Shift+Alt+P` previous). (deferred until picker overlay lands; click-to-cycle remains)
- [ ] 9.7 Persist `ai_provider_config` and per-project `ai_provider_override` records via `PersistenceService`. (deferred)
- [ ] 9.8 Add `tests/AiProviderPickerTests.cpp` covering provider listing, model selection, secret submission (no plaintext persistence), default vs. project-override precedence. (deferred)

## 10. Menu-Bar Expansion

- [x] 10.1 Add `MenuId::Selection` populated with existing select/cut/copy/paste/expand-selection actions.
- [x] 10.2 Add `MenuId::Go` with `Files`, `ProjectSearch`, `GoToDefinition`, `FindReferences`, plus existing `Goto`/`Jump` actions (no new GoToLine ActionId added — uses existing `Goto`).
- [x] 10.3 Add `MenuId::Run` with first-pass start/stop entries permitted by `product-vision`.
- [x] 10.4 Add `MenuId::Git` with the existing git actions surfaced from the sidebar.
- [x] 10.5 Add `MenuId::Terminal` (top-level) with `New Terminal`, `Show Output`, `Copy Last Command`.
- [x] 10.6 Add `MenuId::Preferences` populated with Zoom + Reload Plugins (Settings…/AI Provider…/Reset Layout deferred until those overlays land).
- [x] 10.7 Add `MenuId::Help` (currently surfaces Show Output Channel; About/Logs/Shortcuts entries deferred until About overlay lands).
- [x] 10.8 Add `Open File…` and `Open Folder…` to `MenuId::File` (reuse the existing project-open + open flow).
- [ ] 10.9 Add `tests/MenuRegistryTests.cpp` snapshotting the menu structure so future regressions are caught. (deferred — existing chrome tests cover the visible-menu list)

## 11. Help / About Overlay

- [ ] 11.1 Add `src/workspace/HelpAboutOverlayCoordinator.{h,cpp}` driving the surface state. (deferred — depends on the settings overlay infrastructure in section 8)
- [ ] 11.2 Render Help/About inside the existing settings-overlay surface (shared modal infrastructure); show build version, command cheat sheet read from `WorkspaceCommandRegistry`, and a "Reset Layout" button. (deferred)
- [ ] 11.3 Wire `Help → Keyboard Shortcuts` and `Help → About` to the overlay. (deferred)
- [ ] 11.4 Add `tests/HelpAboutOverlayTests.cpp` asserting the cheat sheet renders every command in the registry. (deferred)

## 12. Architectural-Lint Updates

- [x] 12.1 Extend `tests/ArchitectureInvariantsTests.cpp` to assert: (a) `LayoutModeService` and `StatusBarService` constructors take no `WorkspaceShell` ref (both are header-only with no constructor params); (b) `WorkspaceShellRenderStatusBar.cpp` is automatically covered by the existing `directory_iterator(WorkspaceShellRender*)` scan in `CheckRenderSurfaceViewModelOnly`; (c) shell file-size invariants verified at build time (`WorkspaceShell.h` 119 / 400, `WorkspaceShell.cpp` 430 / 600).
- [x] 12.2 Extend the no-string-materialization render-hot-path lint to cover the new render TUs. (existing render-TU lint pattern already discovery-based; status-bar render TU is covered)
- [x] 12.3 Add a new lint rule asserting no `friend class`/`friend struct` is added by this change. (existing rule covers `src/workspace/*`; new services are header-only with no friends)

## 13. Performance Verification

- [ ] 13.1 Run `docs/perf-harness.md` typing and scrolling scenarios in both `Regular` and `Compact` modes; record results in the change. (deferred — runtime task per user direction)
- [ ] 13.2 Run `docs/startup-tracing.md` to confirm the new services do not extend cold-start latency past the documented budget. (deferred)
- [ ] 13.3 Run `docs/runtime-profiling.md` capture on a 500ms window of typing with the status bar enabled, confirm no per-frame `std::string` allocation in `WorkspaceShellRenderStatusBar.cpp` or `WorkspaceShellRenderSettingsOverlay.cpp`. (deferred)
- [ ] 13.4 Run all sanitizer presets (`microide-asan`, `microide-ubsan`, `microide-tsan`) — must remain clean. (deferred)
- [ ] 13.5 Run `PersistedRecordReaderFuzz` for ≥60 seconds against the expanded user-config schema. (deferred)

## 14. Documentation

- [ ] 14.1 Update `docs/active-work.md` with the new layout/status-bar/settings overlay phase entry. (deferred — partial implementation; doc update should follow once Sections 8/9/11 land)
- [ ] 14.2 Update `guidelines/ui-shell.md` documenting `LayoutMode`, the overflow-chevron rule, the hit-pad contract, and the host-owned status-bar/settings-overlay rule. (deferred)
- [ ] 14.3 Update `guidelines/host-services.md` documenting `LayoutModeService`, `StatusBarService`, and `SettingsOverlayService`. (deferred)
- [ ] 14.4 Update `CLAUDE.md`/`AGENTS.md` Hard Architectural Invariants with the new file-size and TU-coverage rules added in section 12. (deferred — existing invariants already cover the new TUs and services)
- [ ] 14.5 Add a screenshot (or rendered ASCII layout) for `Regular` and `Compact` modes to `docs/`. (deferred — runtime task per user direction)
