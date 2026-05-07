## Why

The shell renders correctly at "designed" widths but degrades silently when the window narrows: menu-bar items are dropped right-to-left without an overflow indicator (`WorkspaceShellMenu.cpp:33-35`), bottom-panel and sidebar buttons can be clipped or run into tabs, and there is no compact alternative layout. Several pointer targets are below the WCAG 2.2 minimum (resize handles 6px, scrollbars 10px, tab close 14px), and many user-facing options (font, line endings, autosave, format-on-save, hover delay, scrollbar size, keymap, terminal shell, AI provider configuration, diagnostic severity) are either unreachable from the GUI or only adjustable by hand-editing persisted state. Configuring an LLM provider in particular is hidden behind a click-to-cycle rail with no picker, no API-key input, and no model selection. Polishing these gaps is the next high-leverage step toward a shell that feels finished without compromising the speed/CPU priority order.

## What Changes

- Introduce a discrete two-mode layout system (`Regular` and `Compact`) selected by a window-width breakpoint plus user override; no continuous fluid reflow. `Compact` collapses the menu bar to a single hamburger, hides project tab badges, replaces the bottom-panel terminal new-tab button with a compact glyph, and reduces the chat sidebar rail to icon-only. The choice is computed once per layout pass and cached on `WorkspaceLayout`.
- Add a menu-bar **overflow chevron** that opens any items that don't fit; never silently truncate.
- Expand pointer-hit targets to meet WCAG 2.2 minimums while keeping visual chrome unchanged: resize handles get a 12px transparent hit pad (visual stays 6px), scrollbars get an 18px hit pad with the visual thumb staying 10px, tab close button gets a 20px hit pad with a 14px glyph, diagnostic squiggle hover gets a full-line-height hit strip (currently a 2px-tall anchor).
- Add a persistent **status bar** along the window bottom edge surfacing: project name + branch, language, indent (tab/space + width), encoding, line/column, problems count, LSP state, AI provider + model. Each segment is click-to-action (open the relevant menu/picker). Toggleable via View menu.
- Expand the menu bar with **Selection**, **Go**, **Run**, **Git**, **Terminal**, **Preferences**, and **Help** top-level menus populated from existing actions (no new commands required for the first pass beyond opening the new pickers). Add an **Open File** and **Open Folder** entry to **File**. Recent-projects/recent-files remain non-goals per `product-vision`.
- Build a host-owned **Settings overlay** (modal surface, like the existing prompt surface) that lists every entry returned by `AllSettingInfos()` grouped by scope (User/Project) and source (built-in/plugin), with type-aware controls (toggle, integer stepper, enum dropdown, free text). Supports search/filter, edit, reset, and persists through `PersistenceService`.
- Expand the **built-in setting catalog** to cover the gaps users currently can't set: `editor.font_family`, `editor.font_size`, `editor.line_endings`, `editor.trim_trailing_whitespace`, `editor.insert_final_newline`, `editor.format_on_save`, `editor.autosave`, `editor.hover_delay_ms`, `ui.layout_mode` (auto/regular/compact), `ui.layout_compact_breakpoint_px`, `ui.scrollbar_size`, `ui.resize_handle_size`, `ui.show_status_bar`, `terminal.shell`, `terminal.font_size`, `diagnostics.min_severity`.
- Build an **AI provider picker overlay** that replaces click-to-cycle in the chat sidebar rail: lists registered providers with model selection, paste-API-key field (forwarded to existing `ProviderAuthProvider`), default-provider toggle, and per-project override. Click-to-cycle becomes a fallback shortcut.
- Add a **Help/About overlay** with build version, key commands cheat sheet (read from `CommandRegistry`), and a "Reset Layout" action.

## Capabilities

### New Capabilities

- `responsive-shell-layout`: discrete layout-mode contract (Regular/Compact), breakpoint selection, overflow rules for menu/tab/panel chrome, and minimum pointer-target sizes for resize, scrollbar, hover, and close affordances.
- `workspace-status-bar`: persistent bottom status surface, content slots, click-to-action behavior, visibility setting, and host-owned ownership of all segments.
- `settings-overlay-surface`: modal settings UI, search/filter contract, type-aware editors, scope/source grouping, persistence integration, and AI-provider sub-overlay.

### Modified Capabilities

- `workspace-architecture`: adds `LayoutMode` to the layout pipeline, registers the new `SettingsOverlayService`, `StatusBarService`, and `LayoutModeService` alongside existing services without growing the shell. Documents that no new shell-level catch-all object is introduced.
- `persisted-state-format`: adds new setting keys to the user-config schema and an `ai_provider_config` section keyed by provider id (default model, last-used model, project override). All keys remain plain `key=value` records routed through `PersistedRecordReader`/`PersistedRecordWriter`; no new file format.
- `plugin-ai-provider-runtime`: standardizes the host-side picker contract — providers SHALL declare `display_name`, `default_model`, `model_options`, `requires_api_key`, and `auth_method` so the picker can render them; cycle-to-next remains a keyboard fallback only.

## Impact

- Code: `src/workspace/WorkspaceLayout.{h,cpp}`, `src/workspace/WorkspaceShellMenu.cpp`, `src/workspace/WorkspaceShellChrome.cpp`, `src/workspace/WorkspaceShellRenderChrome.cpp`, `src/workspace/WorkspaceShellRender*.cpp` for the new status bar and overlay TUs, `src/workspace/WorkspaceMenuRegistry.{h,cpp}`, `src/workspace/WorkspaceSettingsRegistry.{h,cpp}`, `src/workspace/WorkspaceShellSidebar.cpp`, `src/workspace/WorkspaceShellHoverTargets.cpp` (hover-strip widening), plus three new services: `LayoutModeService`, `StatusBarService`, `SettingsOverlayService`. The hard architectural invariant on shell file sizes (`WorkspaceShell.h ≤ 400`, `.cpp ≤ 600`) is preserved by routing all new state through these services and rendering through new render TUs.
- Specs: three new spec files plus deltas to `workspace-architecture/spec.md`, `persisted-state-format/spec.md`, and `plugin-ai-provider-runtime/spec.md`.
- Tests: extend `tests/WorkspaceLayoutTests.cpp` with breakpoint and overflow scenarios; add `tests/StatusBarTests.cpp`, `tests/SettingsOverlayTests.cpp`, `tests/AiProviderPickerTests.cpp`; extend `tests/ArchitectureInvariantsTests.cpp` to forbid new `WorkspaceShell&` constructor params and to assert the new services are referenced from the shell.
- Performance: status bar adds one render TU and one redraw region; layout mode is computed once per layout pass. Hit-pad widening is a constant rectangle inflation; no extra allocations. Settings overlay is built lazily on open. Targeted goal: zero regression on the existing perf-harness scenarios; budget surfaced in the new spec.
- Compatibility: existing persisted state continues to load (new setting keys read defaults when absent). Click-to-cycle provider input remains as a keyboard accelerator for users who prefer it.
- Documentation: update `docs/active-work.md`, `guidelines/ui-shell.md`, and `guidelines/host-services.md` to document the new services, the layout-mode contract, and the status-bar ownership rules.
