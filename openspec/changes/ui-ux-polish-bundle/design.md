## Context

The host shell renders all chrome, inputs, buttons, and editor text through `render::TextRenderer` backed by `render::SdlTtfTextBackend` (SDL3_ttf). Three rendering call patterns coexist today:

- `DrawString(...)` → `TTF_RenderText_Blended` — produces an alpha texture with transparent background. Used for editor body text.
- `DrawStringOn(..., background)` → `TTF_RenderText_LCD(..., bg)` — produces a texture with the background color baked into every pixel that isn't a glyph stroke. Used for buttons (`WorkspaceShellRenderPrimitives.h:DrawButtonCentered`) and single-line inputs (`WorkspaceShellRenderTextInput.cpp`).
- Per-row terminal text drawn as multiple cached "runs" at column-grid x positions (`WorkspaceShellRenderBottomPanel.cpp`).

The visual symptoms reported by the user trace back to four root causes that we have verified against the source:

1. `TTF_RenderText_LCD` returns a surface whose width and height match the rendered glyph extent **plus subpixel/clip padding**. When this surface is uploaded as a texture and drawn at the text's intended `(x, y)`, the opaque background portion of the texture extends past the glyph extent and **paints over the inset outline** that the caller drew earlier. This affects every single-line input and every button.
2. `WorkspaceShellRenderTextInput.cpp` uses `text_y = prompt_rect.y + 4.0f` (lines 196, 246, 327) instead of the centering formula buttons use (`floor((rect.h - LineHeight()) * 0.5f)`).
3. Cached run textures have natural pixel widths that do **not** equal `n × char_width_px` for monospace runs. When a row is split (selection toggles, multi-color spans) or a string grows by an identical glyph (`!`, `!!`, `!!!`), the cumulative natural widths differ from the column-aligned widths, producing visible "wiggle". Mixing LCD (selected) and Blended (unselected) cache entries amplifies the difference.
4. Source Control unselected rows use `text_secondary` for both filename and path; only selected rows differentiate. Diff inline-change underlines are drawn at full theme-token saturation. The Outgoing list is locked to `ResolveGitBaseReference` with no override path. LSP readiness is internal to `WorkspaceLspClient` and never surfaced. The chrome-row double-click handler exists in `WorkspaceChromeMouseCoordinator` but its hit-test is gated on `Contains(layout.menu_bar, ...)` and a 5px tolerance that misses real-world clicks.

CLAUDE.md priorities are correctness > speed > low CPU; the user's command emphasizes performance. We will not regress draw cost or cache behavior — the proposed fixes either preserve the existing call shape or reduce work (one fewer LCD render path).

## Goals / Non-Goals

**Goals:**
- Eliminate background-over-outline artifacts on every button and single-line input.
- Vertically center single-line input text consistently with buttons.
- Stabilize monospace text rendering so identical-glyph typing and selection toggles do not produce visible horizontal wiggle.
- Make the Source Control filename/path hierarchy legible at a glance.
- Allow the user to compare Outgoing files against an alternate base (default base branch retained as the primary option).
- Surface LSP readiness in a passive status indicator and disable LSP context-menu entries while the server is starting; show in-flight indication for issued requests.
- Dim the diff inline-change underlines so they no longer fight the text foreground.
- Make double-click anywhere in the chrome title-bar row (outside menu items) reliably toggle maximize.

**Non-Goals:**
- No new theme tokens. Existing tokens stay; we apply alpha multipliers at draw sites where dimming is needed.
- No global font/text-rendering rewrite. We narrow LCD usage and add a destination-width snap on the backend; we do not switch to a third-party shaper or change hinting.
- No new persistence schema version. The Outgoing-base choice is an optional field; existing records read and write unchanged.
- No new architectural-lint exception. The change keeps service-boundary rules and TU size caps satisfied.
- No background reformatting of unrelated UI ("polish creep" excluded).

## Decisions

### D1: Replace `TTF_RenderText_LCD(..., bg)` with `TTF_RenderText_Blended` for buttons and inputs
The only call site producing the LCD-with-background path is `DrawStringOn`. Callers (`DrawButtonCentered`, `BuildActiveTextInputVisual`) already paint the fill rect *before* drawing text. We make `DrawStringOn` an alias for `DrawString` (the `background` parameter is accepted for source compatibility but ignored) and remove the LCD-with-bg branch from `SdlTtfTextBackend::ResolveEntry`. The cache key still includes the background byte so we don't churn entries during the transition; once all callers are migrated to `DrawString`, the background-bearing key path is unreachable and is removed.

**Why over alternatives:**
- Keeping LCD but tightening the surface bounds: SDL3_ttf does not expose tight glyph-extent control on `TTF_RenderText_LCD`; trying to crop the texture post-hoc is fragile and costs a CPU readback.
- Drawing the outline *over* the text: inverts paint order across the codebase; affects hover/selection layering elsewhere.
- Using `TTF_RenderGlyph_LCD` per-character: increases cache pressure and complicates run rendering for negligible visual benefit at 13pt.

The Blended path produces grayscale-AA glyphs that compose correctly over any pre-painted background, eliminating the artifact at zero perf cost (the cache and texture-upload paths are identical).

### D2: Vertical-center single-line input text using the button formula
Replace each `text_y = prompt_rect.y + 4.0f` with `text_y = prompt_rect.y + std::floor((prompt_rect.h - text_renderer_.LineHeight()) * 0.5f)`. This matches `DrawButtonCentered` and naturally adapts when row heights change.

### D3: Snap monospace destination width to `n × char_width_px` in the text backend
In `SdlTtfTextBackend::DrawString` (and `DrawStringOn` until merged), when `CanUseFastAscii(text)` is true, compute the destination rect width as `text.size() * char_width_pixels / scale_x` instead of `entry->width / scale_x`. SDL stretches the glyph texture to this destination; for monospace fonts the stretch is sub-pixel and visually imperceptible, but column positions become exact. This guarantees:
- A row split into `[unselected | selected | unselected]` runs always covers the original column extent regardless of how TTF measured each piece.
- Typing `!`, `!!`, `!!!` advances by exactly one `char_width_pixel` per glyph; the cursor's right-edge motion is monotonic and constant.

**Why over alternatives:**
- Pre-padding the cached surface with transparent pixels to reach `n × char_width`: requires a software composite per cache entry. Higher CPU and memory.
- Rendering each glyph independently and positioning per cell: increases draw calls per row from O(runs) to O(chars). Regresses CPU.
- Switching to `TTF_RenderText_Blended` for all paths but leaving widths un-snapped: still wiggles because Blended surface widths also don't equal `n × char_width`.

The snap touches a single line in two functions and is only applied for fast-ASCII text; non-ASCII (which may be variable-width) takes the natural path.

### D4: Source Control filename uses `text_primary`, path uses `text_muted` regardless of selection
At `WorkspaceShellRenderSidebar.cpp:566–567`, change unselected `primary_color` from `text_secondary` to `text_primary` and `secondary_color` to `text_muted`. Selected rows already use these stronger tokens; we lift unselected to match. The `DrawPrimarySecondaryRowText` helper is reused — no new render path.

### D5: Outgoing base picker via popup menu + persisted choice
Add a small chevron button to the right of the "Outgoing" group header, rendered with the existing chrome primitives (no new widget framework). Clicking opens an anchored popup menu (using `WorkspaceShellRenderMenus` infrastructure) with three entries:
- `Auto (base branch)` — current default; calls `ResolveGitBaseReference` each refresh.
- `Previous commit (HEAD~1)` — fixed.
- `Specific ref…` — opens the existing prompt surface for free-text ref entry.

The choice is stored on `GitSidebarState` as `OutgoingBaseChoice { kind, custom_ref }` and persisted as part of project state via `PersistenceService` + `PersistedRecordReader/Writer`. New optional field; older records default to `Auto`. `WorkspaceSidebarCoordinatorRefresh.cpp` consults the choice before calling `ResolveGitBaseReference` and passes the resolved ref into `CollectGitBranchOutgoingFiles` unchanged.

**Why over alternatives:**
- A full inline combobox: no such primitive exists; building one for one feature violates "thin coordinators."
- A right-click menu only on the header: discoverability is poor; the chevron makes the affordance visible without growing the chrome.

### D6: LSP readiness surfacing + context-menu gating
- Add `WorkspaceLspClient::ReadinessSnapshot()` returning a small POD: `{ state: Idle|Starting|Indexing|Ready|Failed, message: string, indexed_count: int }`. Read-only; no service shape changes.
- Display the snapshot in the bottom panel status row using existing render primitives.
- In `WorkspaceMenuRegistry.cpp`, mark LSP-dependent context-menu items (`GoToDefinition`, `FindReferences`, `Hover` if present) as enabled only when `state == Ready`; otherwise show a disabled item with label `"Go to Definition (LSP starting…)"` and similar.
- For in-flight requests (after click), set a transient flag on the action coordinator that the bottom panel reads to render `LSP: working…` until the response arrives or times out.

The user-perceived "freeze" was always cosmetic — `WorkspaceShellAssist.cpp:391/443` already dispatch async — so this is purely a feedback fix.

### D7: Dim diff inline-change underlines at the draw site
At the underline draw call (`DecoratedTextGridRenderer.cpp:177–183` and `WorkspaceShellCompareRender.cpp:302–305`), multiply the underline color's alpha by 0.55 before submitting to SDL. This is a single-line change per site. No new theme token; the source color stays authoritative for everything else (gutter chips, summary bars).

### D8: Title-bar double-click to maximize — widen hit-test, keep handler
The handler in `WorkspaceChromeMouseCoordinator.cpp:212–227` is correct in spirit. We:
- Keep the early-return for clicks landing inside a menu-bar item (those open the menu).
- Remove the additional 5px-positional gate that suppresses real double-clicks; rely on `event.button.clicks == 2` from SDL plus a 500ms manual fallback at the same coordinate (no positional tolerance once the click is inside `layout.menu_bar`).
- Confirm the chrome row's hit rect is the full menu-bar height across the window, not just the strip behind the title text.

If `CurrentWindowChromeState().custom_enabled` is false (OS native title bar), the WM owns double-click and we leave it alone; the host code path is for the custom chrome only.

## Risks / Trade-offs

- **Glyph appearance changes slightly when buttons/inputs switch from LCD to Blended.** → Mitigation: at 13pt JetBrains Mono on typical displays the difference is hard to spot; LCD subpixel rendering in `TextRenderer` for editor body text is unchanged. If user feedback objects, we can selectively reintroduce LCD for editor cell-grid only.
- **Monospace destination-width snapping stretches the glyph texture by sub-pixel amounts.** → Mitigation: the snap is only applied when `CanUseFastAscii` is true, and `char_width_` is derived from `TTF_GetStringSize` of `"M"×32`, so the stretch is small and uniform. Non-ASCII text uses the natural path.
- **Outgoing-base choice persistence adds an optional field to project state.** → Mitigation: the `PersistedRecordReader` ignores unknown fields and defaults missing ones; cross-version reads stay safe. We add a regression test that an older record (without the field) loads with `OutgoingBaseChoice::Auto`.
- **LSP readiness snapshot read from a non-LSP thread.** → Mitigation: snapshot is a value copy taken under the existing client mutex; no new lock.
- **Title-bar hit-test loosening could swallow stray double-clicks meant for window-resize edges.** → Mitigation: the resize-edge handlers run before the menu_bar handler in mouse dispatch; we are not changing dispatch order.
- **LCD removal makes the `background` parameter on `DrawStringOn` vestigial.** → Mitigation: keep the signature for one cycle to avoid a callsite churn, mark the parameter `[[maybe_unused]]`, and remove in a follow-up rename.

## Migration Plan

1. Land backend changes (D1, D3) and update buttons/inputs to use the new path. Verify with redraw comparison tests under SDL dummy video.
2. Apply D2, D4, D7 — pure UI tweaks, low blast radius.
3. Add `OutgoingBaseChoice` and persistence (D5). Ship the chevron + popup menu wiring last so partial states do not leak into project records.
4. Add LSP readiness snapshot, status row, and menu gating (D6).
5. Apply D8 (chrome double-click hit-test loosening). Add a synthetic-event regression test in `WorkspaceChromeMouseCoordinator` tests.
6. No rollback plan beyond `git revert`; all changes are localized and there is no migration script.

## Open Questions

- Should the LSP "working…" hint also appear inline at the cursor (as a small spinner glyph) or only in the status row? Default: status row only, to avoid invalidating editor render caches per frame. Revisit if users still feel the freeze.
- Should the dim multiplier (0.55) be theme-tunable? Default: hard-coded constant; promote to theme only if a second use case appears.
- Is `text_muted` dim enough for the path, or do we need to also reduce its alpha further? Default: keep `text_muted` as-is; revisit after first build screenshot.
