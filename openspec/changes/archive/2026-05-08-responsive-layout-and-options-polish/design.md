## Context

The shell composition was designed for a single "comfortable" working width and has not been audited against narrow windows or small/imprecise pointers. Concrete findings from `src/workspace/`:

- `WorkspaceShellMenu.cpp:33-35` truncates menu-bar items right-to-left without any visible affordance once `x + width > max_x`. There is no overflow chevron, no hamburger, and no other recovery path.
- `WorkspaceLayout.h:188-215` sets the visual chrome dimensions: 6px resize handle, 10px scrollbar, 14px tab close glyph, 18px bottom-panel header button. WCAG 2.2 SC 2.5.8 calls for ≥24×24 CSS px target areas (and Apple HIG/Material recommend 44/48). We are well under those for primary-pointer hits.
- `WorkspaceShellHoverTargets.cpp` builds the diagnostic-hover anchor as a 2px-tall strip (`anchor_rect = MakeRect(..., line_y + interaction.line_height - 2.0f, ..., 2.0f)`). The popup itself is wide, but the *trigger* line is 2px tall, which makes hover acquisition stochastic.
- `WorkspaceSettingsRegistry.cpp` exposes only six built-in keys; everything else (font, line endings, autosave, format-on-save, hover delay, scrollbar size) is unset and unsettable from the GUI.
- There is no `StatusBar*` or `Preferences*` surface anywhere in `src/workspace/`. The closest analog (`PromptSurfaceService` + the prompt-overlay rectangle) provides the modal pattern we should reuse.
- `WorkspaceShellSidebarMouse.cpp:50-65` exposes only "click rail → cycle provider" and a key chord. There is no provider picker overlay, no model selection UI, and no place to enter an API key from the GUI.

Constraints from `openspec/specs/`:

- `product-vision`: priority order is correctness → speed → low CPU → low memory → architectural clarity. "Recent projects/files" and native OS menu integration are explicit non-goals — we will not add them.
- `workspace-architecture`: hard caps on `WorkspaceShell.h` (≤400) and `WorkspaceShell.cpp` (≤600). New work must route through a service. No `friend class`. Render TUs must consume view models.
- `persisted-state-format`: every persisted record routes through `PersistedRecordReader`/`PersistedRecordWriter`. We add new typed records but keep the same format.
- `performance-budgets`: render TUs must not materialize strings in hot paths. This rules out per-frame string concatenation in the new status bar and overlay.

The user goal is "as fast as possible" — so the design picks discrete, cached, layout-time decisions over per-frame fluid reflow, and pushes view-model construction and overlay rendering to event-driven rebuilds.

## Goals / Non-Goals

**Goals:**

- Add an explicit two-mode (`Regular`/`Compact`) layout system that resolves once per layout pass and is read everywhere else by lookup.
- Eliminate silent menu-bar truncation; never lose a command without a visible affordance.
- Bring every interactive shell affordance up to a ≥24×24 px hit target by inflating *hit pads* without changing the visible chrome.
- Add a host-owned status bar that surfaces the metadata users currently have to chase through tooltips and menus.
- Add a host-owned settings overlay that exposes every setting from `AllSettingInfos()` with type-aware editors and search.
- Replace click-to-cycle AI provider selection with a real picker (provider list, model dropdown, API-key entry, default + per-project override).
- Expand the menu bar's coverage of common workflows (Selection/Go/Run/Git/Terminal/Preferences/Help) using existing actions plus the new picker entry points.

**Non-Goals:**

- Fluid web-style reflow with animations between layout sizes — the user explicitly does not want this and it would burn CPU.
- Native OS menus, recent-projects/recent-files surfaces, account systems — all forbidden by `product-vision`.
- Plugin-replaceable status bar or settings overlay — these stay host-owned. Plugins still extend the *settings catalog* through the existing `ContributedSettings` API.
- New plugin marketplace, theme marketplace, or remote install — out of scope.
- A debugger UI beyond the "first-pass start/stop + output channel" already permitted by `product-vision`.
- Touch input. WCAG 2.2 SC 2.5.8 (24×24) is the target; we do not chase the 44×44 (touch) bar.

## Decisions

### Decision 1: Discrete layout modes with hysteresis, computed in `ComputeLayout`

`LayoutMode` is a two-valued enum stored on `WorkspaceLayout`. `ComputeLayout` resolves it once from `ui.layout_mode` (override) and `ui.layout_compact_breakpoint_px` (default 720px). To avoid flapping at the boundary, a 24px hysteresis band is applied: once `Compact`, the mode stays compact until window width ≥ `breakpoint + 12`; once `Regular`, it stays regular until width ≤ `breakpoint - 12`.

A new `LayoutModeService` exposes a stable `CurrentMode()` snapshot for non-layout consumers (e.g., the status bar render TU). The service is a thin owner, not a recomputation site — `ComputeLayout` is the single source of truth.

**Alternatives considered:**

- *Pure breakpoint, no hysteresis*: simpler, but resizing the window across the threshold would flip-flop the chrome and trash the redraw cache. Rejected.
- *Three modes (regular/compact/mini)*: more granular, but the user explicitly asked for two. Rejected as scope creep.
- *Per-surface compact flags*: gives finer control but couples every surface to its own breakpoint logic. Rejected — one global mode is easier to reason about and matches the perf priority.

### Decision 2: Menu-bar overflow chevron + Compact hamburger

In `Regular` we keep the existing menu-bar item layout; when width is exhausted we render a chevron at the right edge of the last fitting item. Click on the chevron opens a popup whose entries are exactly the menu specs that did not fit, in declared order. In `Compact`, the menu bar collapses to a single hamburger that opens the entire menu list.

Implementation: extend `ComputeVisibleMenuBarItems` to return both the visible items and the overflow specs. Add a new `VisibleMenuBarOverflow` rect. The chevron is a 24×24 hit pad with a 14px glyph centered in it.

**Alternatives considered:**

- *Always show a hamburger*: fewer code paths. Rejected — wide-window users get the muscle-memory of "Edit → Undo" in one click; we should not force a popup on them.
- *Drop items left-to-right instead of right-to-left*: matches "important menus on the left" but breaks reading order. Rejected.

### Decision 3: Hit pads independent of visual chrome

We split each affordance into a *visual rect* (what is painted) and a *hit rect* (what the cursor tests against). The visual rect is unchanged; the hit rect is inflated. The hit rect is owned by `WorkspaceLayout` so cursor and click logic can read both.

| Affordance | Visual size | Hit-pad size | Currently |
| --- | --- | --- | --- |
| Sidebar/bottom-panel resize handle | 6px (existing) | 12×24px | 6px both |
| Scrollbar thumb | 10px (existing) | 18px cross-axis, full track length | 10px both |
| Tab close glyph | 14px | 20×20px | 14px both |
| Bottom-panel new-tab button | 14–18px | 24×24px | 14–18px both |
| Diagnostic hover trigger | 2px-tall (anchor) | full line height (text width) | 2px |
| Window control buttons | menu_bar.h | menu_bar.h (already ≥18px) | OK |

Inflation is computed in `WorkspaceLayout` helper functions (`SidebarResizeHitRect`, etc.). Mouse coordinators read the hit rect; render TUs read the visual rect. Hit pads MAY overlap adjacent surfaces but MUST NOT extend into the active editor text area, to avoid stealing clicks from the cursor.

**Alternatives considered:**

- *Increase visual chrome to 24px*: gives the same hit area, but bloats the chrome and burns vertical real estate. Rejected — visual density is a feature, not a bug.
- *User-configurable inflation only*: ships the bug as a setting. Rejected — the default must be accessible.

The settings `ui.scrollbar_size` and `ui.resize_handle_size` (compact/regular/large) let users tune both visual and hit pads in lockstep, with `regular` being the new default.

### Decision 4: Status bar as a new layout slot + render TU

A new `WorkspaceLayout.status_bar` rect is reserved at the bottom of the window when `ui.show_status_bar` is true. `kWorkspaceStatusBarHeight = 22.0f` at scale 1.0. The bottom panel's height clamp is updated to subtract the status-bar height.

A new `StatusBarService` owns the segment list (a fixed-size array of POD segment values). `RenderViewModelBuilder::BuildStatusBar()` snapshots the service into a POD `StatusBarViewModel`. The new `WorkspaceShellRenderStatusBar.cpp` paints the strip from this view model.

Segment ordering (left-to-right): project + branch, language, indent, encoding, line/col, problems, LSP, AI provider/model, layout-mode badge. In `Compact`, segments drop in the documented order until the strip fits.

Click-to-action map:

- project + branch → opens git submenu
- language → opens language picker
- indent / encoding → opens settings overlay scoped to that key
- line/col → opens "go to line" prompt
- problems → opens Problems sidebar
- LSP → opens output channel
- AI provider/model → opens AI provider picker
- layout-mode → flips `ui.layout_mode`

**Alternatives considered:**

- *Top status bar*: matches some IDEs but conflicts with the existing menu bar position. Rejected.
- *Plugin-contributed segments*: extensibility is nice, but the user wants speed; gating segments through a contribution registry adds per-frame indirection. Plugins keep their existing notification/diagnostics surfaces.

### Decision 5: Settings overlay reusing the prompt-surface pattern

The settings overlay is a new modal surface centered on the editor area, rendered via `ComputePromptSurfaceRect` (already used for dirty/save dialogs). A new `SettingsOverlayService` owns the open/closed state, search query, scroll, and any in-flight unsaved changes (we persist on each edit, so "unsaved" should be empty in steady state).

The overlay is built on open and on input events; it is not rebuilt on every cursor-move frame. The view model is a `std::vector<SettingsOverlayRow>` POD; rows reference `SettingInfo` snapshots from the registry. Rendering reads the cached vector.

Type-aware editors:

- `Bool`: toggle (left/right click flips)
- `Int`: number with `-`/`+` steppers and a typed input field
- `Float`: same as Int with float formatting
- `Enum`: dropdown opens a sub-popup of declared `enum_values`
- `String`: single-line text input via `editor::SingleLineEditor` (per the architectural rule that single-line input surfaces consume `SingleLineEditor`)

**Alternatives considered:**

- *Settings as a sidebar panel*: better for long browsing but worse for "open, change one thing, close" — which is the dominant flow. Rejected.
- *Settings as a separate window*: forbidden by `product-vision`'s single-window rule.

### Decision 6: AI provider picker as a settings sub-overlay

The provider picker reuses the settings-overlay infrastructure (same surface rect, same focus model) but renders a focused list: providers on the left, the selected provider's detail panel on the right (model dropdown, auth state, "Use as default" toggle, "Per-project override" toggle, secret input field if needed).

We extend `AiProviderSpec` with `display_name`, `default_model`, `requires_api_key`, `auth_method`. Existing providers are grandfathered: missing `display_name` falls back to `id` *with a logged warning*, and the architectural-lint test will fail builds where any new provider lacks `display_name`.

Click-to-cycle on the chat-sidebar rail is replaced with "click → open picker." The keyboard shortcut still cycles for power users.

Secrets continue to flow through `WorkspaceAuthProvider::SetSecret(provider_id, value)`. The picker's input field never echoes the secret to the screen and never persists it in plain text — the surrounding render TU uses dot-mask glyphs only.

**Alternatives considered:**

- *Inline picker in the chat sidebar*: would blow up the rail in compact mode and force scroll. Rejected.
- *Plugin-owned picker*: each provider could ship its own; rejected per `plugin-ai-provider-runtime` ("host rejects provider-owned workflow replacement").

### Decision 7: Menubar expansion uses existing actions only

The new top-level menus (`Selection`, `Go`, `Run`, `Git`, `Terminal`, `Preferences`, `Help`) are wired to existing `ActionId` values plus the three new actions added by this change (`OpenSettings`, `OpenAiProviderPicker`, `OpenHelpAbout`). No new background work or model code is invented; this is purely a discoverability change.

`File` gets `Open File...` and `Open Folder...` (both reuse existing project-open + reveal-in-tree actions). Recent files/projects remain explicitly out of scope.

**Alternatives considered:**

- *A full command palette*: more powerful but larger scope. Existing palette-like surfaces (Files action, ProjectSearch action) cover that footprint. Defer.

### Decision 8: New built-in setting keys are added with conservative defaults

Defaults (chosen to avoid behavior changes for existing users):

| Key | Type | Default |
| --- | --- | --- |
| `editor.font_family` | string | `""` (empty → use TextRenderer's current default) |
| `editor.font_size` | int (8..32) | `13` |
| `editor.line_endings` | enum (`lf`/`crlf`/`auto`) | `auto` |
| `editor.trim_trailing_whitespace` | bool | `false` |
| `editor.insert_final_newline` | bool | `false` |
| `editor.format_on_save` | bool | `false` |
| `editor.autosave` | enum (`off`/`on_focus_change`/`after_delay`) | `off` |
| `editor.hover_delay_ms` | int (0..2000) | `350` |
| `ui.layout_mode` | enum (`auto`/`regular`/`compact`) | `auto` |
| `ui.layout_compact_breakpoint_px` | int (600..2000) | `720` |
| `ui.scrollbar_size` | enum (`compact`/`regular`/`large`) | `regular` |
| `ui.resize_handle_size` | enum (`compact`/`regular`/`large`) | `regular` |
| `ui.show_status_bar` | bool | `true` |
| `terminal.shell` | string | `""` (use platform default) |
| `terminal.font_size` | int (8..32) | `13` |
| `diagnostics.min_severity` | enum (`hint`/`info`/`warning`/`error`) | `hint` |

Actual *behavior* of each key is implemented in the layer that reads it (e.g., the file-save pipeline reads `trim_trailing_whitespace` and `insert_final_newline`). Where a behavior is non-trivial, the work is broken out as a sub-task in `tasks.md`. The first pass implements every key's *storage and UI*, but only the keys that are inexpensive to wire (`ui.*`, `editor.font_size`, `editor.hover_delay_ms`, `editor.line_endings` on save, `editor.trim_trailing_whitespace` on save, `editor.insert_final_newline` on save, `diagnostics.min_severity`) connect to live behavior in this change. The rest land in follow-up changes; their entries still appear in the overlay so users see the surface.

## Risks / Trade-offs

- **[Compact-mode regressions in existing tests]** Many existing chrome tests assume regular spacing. → Add a `LayoutMode` parameter to fixtures and assert both modes; default test fixture stays in `Regular` to avoid mass churn.
- **[Hit-pad clicks steal events from the editor]** A 12px resize hit pad sits 6px into the editor area. → Cursor and click coordinators check the hit pad *first*, but only when the cursor is also outside the editor's text-content rect (`text_x + visible_columns * char_width`). Tests assert that clicking inside text never triggers a resize.
- **[Status bar adds redraw work]** Each frame would compute view-model strings if naive. → Cache `StatusBarViewModel` strings, invalidate on the underlying state change (file save, branch change, cursor move, problem count change, provider change). Reuse existing `RequestRedraw` flow.
- **[Setting catalog churn breaks the persistence reader]** Adding 16 typed records risks divergence. → Reuse the existing `Forward And Backward Compatibility Rules` (skip unknown tags). Older builds are unaffected. New keys round-trip through the same `PersistedRecordReader/Writer` pair.
- **[Provider picker UX vs. existing cycle muscle memory]** Long-time users who rely on click-to-cycle will be surprised. → Keep the keyboard accelerator. Surface a one-time toast on first launch after the change explaining the new picker.
- **[Architectural-lint flake on new render TUs]** New `WorkspaceShellRender*` TUs must be discovered by the lint scan. → The architectural-lint test is already discovery-based per `workspace-architecture` Requirement: Render Surface Lint Coverage Is Discovery-Based. Verify with a deliberate test that adds a new TU.
- **[WorkspaceShell file-size cap]** Three new services + status-bar wiring would tempt growing the shell. → Land each service with its own coordinator/render TU; the shell only forwards. A pre-merge test asserts file sizes stay under cap.
- **[Hover delay setting interacts with running hover popups]** If a user changes `editor.hover_delay_ms` while a popup is shown, the active timeout uses the old value. → Acceptable; the next hover acquisition uses the new value.

## Migration Plan

1. Land `LayoutMode` in `WorkspaceLayout` with `Regular` as default — no chrome change yet, just plumbing. Verify no perf regression on the existing perf harness.
2. Land hit-pad inflation in `WorkspaceLayout` + cursor coordinators. Verify `WorkspaceShellCursor` and mouse-coordinator tests still pass; add new tests for the inflated rects.
3. Land overflow chevron in `WorkspaceShellMenu.cpp` + `WorkspaceShellRenderChrome.cpp`. Verify menu-bar truncation tests fail before, pass after.
4. Land `LayoutModeService`, wire `Compact` rendering for menu bar, project tabs, bottom-panel header button, chat sidebar rail. Behind a default-on `ui.layout_mode = auto` setting.
5. Land `kWorkspaceStatusBarHeight` reservation, `StatusBarService`, `WorkspaceShellRenderStatusBar.cpp`. Default `ui.show_status_bar = true`.
6. Expand `BuiltinSettingSpecs()` with the 16 new keys and update `WorkspacePersistenceCoordinatorConfig` to read/write them.
7. Land `SettingsOverlayService` + `WorkspaceShellRenderSettingsOverlay.cpp`. Wire Preferences → Settings… menu entry.
8. Land AI provider picker as a sub-overlay; remove rail click-to-cycle, keep keyboard cycle. Extend `AiProviderSpec` and validate at registration.
9. Wire the seven new menu-bar entries (Selection/Go/Run/Git/Terminal/Preferences/Help) to existing actions plus the three new "open overlay" actions.
10. Wire the cheap `editor.*` and `diagnostics.*` settings (font size, hover delay, trim trailing whitespace, insert final newline, line endings, min severity) to their consumers.

Rollback: each step is a separable PR; reverting any one step leaves the prior steps working. The settings catalog is additive — reverting it leaves persisted keys ignored on the next read (per the forward-compat rule).

## Open Questions

- Should `editor.font_family` default to a hard-coded "MicroIDE Mono" if available? Pending: confirm what `TextRenderer` exposes as discoverable system fonts on each platform.
- Should the AI provider picker offer to *test* the API key after entry (auth-check ping)? Probably yes, but the round-trip cost depends on the runtime's `RequestAuthCheck()` shape — verify the call is async and non-blocking.
- Should the status bar's "layout-mode badge" double as a toggle, or is the View menu enough? Default plan: yes, single click toggles between auto and the opposite of the current resolved mode.
- The `editor.format_on_save` and `editor.autosave` settings need a wired implementation that is non-trivial; defer connecting these to behavior to a follow-up change while still landing the storage + UI here.
