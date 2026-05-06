## Why

Several visual and interaction defects in the host shell add up to a feeling of unpolished UX: text in single-line inputs and buttons is not vertically centered and its background paints over the surrounding outline; the Source Control list lacks contrast between filename and path; the Outgoing list is locked to the resolved base branch with no way to compare against another ref; LSP context-menu actions appear to freeze the app while the server is still warming; diff inline-change underlines are too saturated; the title bar does not reliably toggle maximize on double-click; and monospace text "wiggles" when selection toggles or when adjacent identical glyphs are typed (`!!!`). Each issue is small, but together they erode trust in the editor's polish — and several share a root cause in the text backend, so they are best fixed as one bundle.

## What Changes

- Switch button and single-line-input text from the LCD-with-background path (`DrawStringOn` → `TTF_RenderText_LCD`) to the alpha-blended path (`DrawString` → `TTF_RenderText_Blended`) so the cached glyph texture no longer paints an opaque rectangle over the outline. The fill rect is already drawn explicitly under the text, so LCD's pre-baked background is redundant.
- Vertically center single-line-input text using the same formula buttons use (`floor((rect.h - LineHeight()) * 0.5f)`) instead of a hard-coded `+ 4.0f` offset.
- Snap monospace render runs to `n × char_width` destination width when the run is ASCII, so split runs (terminal selection, multi-color spans) and additive typing of identical glyphs (`!`, `!!`, `!!!`) always occupy the same column extent regardless of TTF's natural surface width. This eliminates the wiggle without changing glyph rendering quality.
- Increase contrast between filename and path in the Source Control sidebar: filename uses `text_primary` (selected and unselected), path uses `text_muted`.
- Add a base-ref picker to the Outgoing group: a small chevron control on the "Outgoing" header opens a popup menu with `Base branch (default)`, `Previous commit (HEAD~1)`, `Specific ref…`. The chosen base is persisted per project via `PersistenceService` and replaces the resolved default in `CollectGitBranchOutgoingFiles`.
- Surface LSP readiness in the bottom panel/status row (`LSP: starting…` / `LSP: ready` / `LSP: indexing N…`) and gate context-menu LSP entries: when not ready, the entries are disabled with a tooltip "LSP starting…"; when in flight, an inline spinner appears next to the cursor or in the status row so the request never looks like a freeze.
- Dim diff inline underline colors. Add multiplier (alpha ≈ 0.55) at the underline draw site so the squiggle no longer outshines the text. Color tokens stay; only the underline render dims them.
- Make double-click anywhere in the chrome title-bar row (outside menu items) toggle maximize. The double-click handler exists; widen the hit-test region to the full menu_bar rect minus item rects, relax the 5px tolerance to ~8px, and ensure SDL events with `event.button.clicks == 2` fire the action even if our manual fallback misses.

## Capabilities

### New Capabilities
- `host-ui-polish`: durable UX contracts for the host shell — single-line input vertical centering, button/input outline integrity, monospace text stability under split runs and additive typing, Source Control filename/path contrast, Outgoing-base picker persistence, LSP readiness surfacing, dimmed diff underlines, and title-bar double-click toggles maximize.

### Modified Capabilities
<!-- None — no existing requirements change behavior; the new contracts live in host-ui-polish. -->


## Impact

- Code: `src/render/SdlTtfTextBackend.cpp`, `src/render/TextRenderer.{h,cpp}`, `src/workspace/WorkspaceShellRenderPrimitives.h`, `src/workspace/WorkspaceShellRenderTextInput.cpp`, `src/workspace/WorkspaceShellRenderBottomPanel.cpp`, `src/workspace/WorkspaceShellRenderSidebar.cpp`, `src/workspace/WorkspaceSidebarState.h`, `src/workspace/WorkspaceSidebarCoordinatorRefresh.cpp`, `src/workspace/WorkspaceGitSidebarPresentation.cpp`, `src/workspace/WorkspaceMenuRegistry.cpp`, `src/workspace/WorkspaceShellAssist.cpp`, `src/workspace/WorkspaceLspClient.h` (read-only API exposure), `src/workspace/WorkspaceShellRenderChrome.cpp`, `src/workspace/WorkspaceChromeMouseCoordinator.cpp`, `src/workspace/WorkspaceShellCompareRender.cpp`, `src/editor/DecoratedTextGridRenderer.cpp`, `src/render/Theme.{h,cpp}` (no new tokens; dim multiplier applied at draw site).
- APIs: `DrawStringOn` becomes a thin wrapper that calls `DrawString` (background arg ignored except to advise alpha pre-multiplication if ever needed); callers unchanged. New `GitCompareService` overload accepts an explicit `base_ref` choice plus a small `BasePickerChoice` enum. `WorkspaceLspClient::ReadinessSnapshot()` exposed for the status row.
- Persistence: project-state record gains an optional `outgoing_base_choice` field (default = "auto"); existing records stay readable. Routed through `PersistedRecordReader/Writer`.
- Dependencies: none added.
- Tests: extend `TextRendererBackend` tests for monospace snap; extend `WorkspaceShell` tests for input/button outline coverage and centering; add unit coverage for outgoing-base persistence; add an architectural-lint exception list update only if needed (it should not be).
- Performance: zero net cost. Switching `DrawStringOn` to Blended uses the same cache path; monospace snapping is one extra float multiply per draw call; dim multiplier is constant-folded; LSP indicator is event-driven, not polled.
