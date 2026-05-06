## 1. Text Backend: Outline Integrity And Monospace Stability

- [x] 1.1 In `src/render/SdlTtfTextBackend.cpp::ResolveEntry`, drop the `TTF_RenderText_LCD(..., bg)` branch; always call `TTF_RenderText_Blended`. Keep the `background` parameter on the cache key for one cycle (it will be unused) so existing callers compile.
- [x] 1.2 In `src/render/SdlTtfTextBackend.cpp::DrawString` and `DrawStringOn`, when `CanUseFastAscii(text)` is true, set the destination rect width to `text.size() * char_width_pixels / scale_x` (where `char_width_pixels = char_width_ * scale_x`). Non-ASCII text keeps the natural-width path.
- [ ] 1.3 Add a `TextRendererBackend` test that draws `!`, `!!`, `!!!` at `x=0` and asserts the right edges land at `1*char_width`, `2*char_width`, `3*char_width` (within rounding tolerance).
- [x] 1.4 Add a `TextRendererBackend` test that asserts a blended draw of "abc" produces a texture whose top-left and bottom-right pixels are alpha=0 (transparent), confirming no opaque background.
- [ ] 1.5 Run sanitizer presets (`microide-asan`, `microide-ubsan`) on the text-renderer tests to confirm no regressions.

## 2. Buttons And Single-Line Inputs

- [x] 2.1 In `src/workspace/WorkspaceShellRenderPrimitives.h::DrawButtonCentered`, change the `DrawStringOn(...)` call to `DrawString(...)` (drop the `colors.fill` argument). Confirm the explicit fill rect is drawn before the text.
- [x] 2.2 In `src/workspace/WorkspaceShellRenderTextInput.cpp` (lines around 196, 246, 327), replace `text_y = prompt_rect.y + 4.0f` with `text_y = prompt_rect.y + std::floor((prompt_rect.h - text_renderer_.LineHeight()) * 0.5f)` and switch `DrawStringOn` calls to `DrawString`.
- [x] 2.3 In `src/workspace/WorkspaceShellRenderBottomPanel.cpp` command-input render path, audit any remaining `DrawStringOn` callers and switch to `DrawString`.
- [ ] 2.4 Add a `WorkspaceShell` redraw-comparison test under SDL dummy video that draws a button and a single-line input, then verifies that every pixel of the outline rect retains the outline color after the text is drawn.
- [ ] 2.5 Add a redraw-comparison test that asserts single-line input text top/bottom margins differ by ≤1px.

## 3. Source Control Filename / Path Contrast

- [x] 3.1 In `src/workspace/WorkspaceShellRenderSidebar.cpp` around line 566–567, change unselected `primary_color` to `theme_.text_primary` and `secondary_color` to `theme_.text_muted`. Selected colors stay as-is.
- [ ] 3.2 Update or extend the existing sidebar render test fixture to assert filename and path use distinct theme tokens (primary vs muted) in both selection states.

## 4. Outgoing Base Picker

- [x] 4.1 Add `OutgoingBaseChoice { enum Kind { Auto, PreviousCommit, SpecificRef }; std::string custom_ref; }` to `src/workspace/WorkspaceSidebarState.h`.
- [x] 4.2 Add a project-state field for `outgoing_base_choice` in the persisted record schema. Use `PersistedRecordReader/Writer`; treat missing field as `Auto`. Add a regression test that an older record loads with `Auto`.
- [x] 4.3 In `src/workspace/WorkspaceSidebarCoordinatorRefresh.cpp`, branch on the choice: `Auto` → `ResolveGitBaseReference` (existing path); `PreviousCommit` → fixed `HEAD~1`; `SpecificRef` → use `custom_ref` directly. Pass the resolved string into `CollectGitBranchOutgoingFiles` unchanged.
- [x] 4.4 In `src/workspace/WorkspaceGitSidebarPresentation.cpp`, render a small chevron button to the right of the "Outgoing" header.
- [x] 4.5 Wire the chevron click to open an anchored popup menu via the `WorkspaceShellRenderMenus` infrastructure with three entries: `Auto (base branch)`, `Previous commit (HEAD~1)`, `Specific ref…`.
- [x] 4.6 The `Specific ref…` entry opens the existing prompt surface; on accept, write `OutgoingBaseChoice{SpecificRef, entered_string}` and persist.
- [x] 4.7 Add a unit test for `WorkspaceSidebarCoordinatorRefresh` covering each choice kind.
- [x] 4.8 Add a fixture test that a project's `OutgoingBaseChoice` round-trips through save/load.

## 5. LSP Readiness And Action Gating

- [x] 5.1 Expose `WorkspaceLspClient::ReadinessSnapshot()` returning `{ State, message, indexed_count }`. Implement under the existing client mutex; return by value.
- [x] 5.2 Add a status segment to the bottom panel render path that displays the snapshot's state and message. No new theme tokens.
- [x] 5.3 In `src/workspace/WorkspaceMenuRegistry.cpp`, mark `GoToDefinition`, `FindReferences`, and any other LSP-driven entries as enabled only when `state == Ready`. Provide labels like `"Go to Definition (LSP starting…)"` for the disabled state.
- [x] 5.4 In `src/workspace/WorkspaceShellAssist.cpp`, set a transient "lsp request in flight" flag when dispatching async LSP requests, cleared on completion or timeout. The bottom panel reads this flag.
- [x] 5.5 Add a unit test for the menu-availability path covering `Starting`, `Indexing`, `Ready`, and `Failed` states.

## 6. Diff Underline Dimming

- [x] 6.1 In `src/editor/DecoratedTextGridRenderer.cpp` underline draw site (around line 177), multiply the underline color's alpha by 0.55 before submitting to SDL.
- [x] 6.2 In `src/workspace/WorkspaceShellCompareRender.cpp` around line 302, ensure the same dim multiplier is applied. If both code paths share a helper, apply the multiplier in the helper to avoid drift.
- [ ] 6.3 Add a render test that compares the underline pixel alpha against the corresponding gutter chip alpha for the same diff token; assert the underline is at most 0.60×.

## 7. Title-Bar Double-Click Maximize

- [ ] 7.1 In `src/workspace/WorkspaceChromeMouseCoordinator.cpp` around lines 212–227, drop the 5px positional gate on the manual fallback while keeping the 500 ms time gate (raised from 400 ms). The SDL `event.button.clicks == 2` path stays.
- [ ] 7.2 Verify `layout.menu_bar` is the full chrome row width when custom chrome is enabled. If it is narrower than the visible title bar strip, widen the layout to match in `WorkspaceShellRenderChrome.cpp`.
- [ ] 7.3 Add a synthetic-event test in the chrome mouse coordinator tests: two clicks at slightly different x positions (e.g., 10 and 25) within 400 ms inside the menu_bar but outside any item rect produce one `WindowAction::ToggleMaximize`.

## 8. Validation

- [ ] 8.1 Build clean: `cmake -S . -B build && cmake --build build`.
- [ ] 8.2 `ctest --test-dir build --output-on-failure` — all green, including the new tests.
- [ ] 8.3 Run sanitizer presets `microide-asan` and `microide-ubsan`; clean.
- [ ] 8.4 Manual smoke: launch the app, open a project, verify each of the eight reported issues is resolved (button outlines intact, input vertically centered, no wiggle on `!!!` or terminal selection, source-control contrast, base picker works, LSP indicator updates, diff underlines dimmed, title-bar double-click toggles maximize).
- [ ] 8.5 Re-run a perf-harness scenario from `docs/perf-harness.md` covering editor + terminal redraw to confirm no regression. Compare against pre-change numbers.
