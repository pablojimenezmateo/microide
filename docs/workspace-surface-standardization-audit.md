# Workspace Surface Standardization Audit

Reviewed on 2026-04-23.

Scope:

- `src/workspace/*`
- `src/editor/*`
- user-visible shell surfaces, render helpers, labels, tooltips, and local UI conventions

This document started as a drift audit for repeated shell UI patterns. The original high-impact
findings are now addressed. The current purpose of this file is to record the resulting baseline
and the few distinctions that remain intentional.

## Standardized Baseline

The shell now uses shared primitives for the main repeated UI families:

- cards, titled cards, compact tooltips, and common surface fills or outlines now route through
  shared surface helpers:
  `src/render/SurfacePrimitives.h`
  `src/workspace/WorkspaceShellRenderPrimitives.h`
- prompts, overlays, sidebar search fields, and the bottom-panel command or chat prompt now share
  one framed text-field baseline:
  `src/workspace/WorkspaceShellRenderPrompts.cpp`
  `src/workspace/WorkspaceShellRenderOverlay.cpp`
  `src/workspace/WorkspaceShellRenderSidebar.cpp`
  `src/workspace/WorkspaceShellRenderBottomPanel.cpp`
  `src/workspace/WorkspaceShellRenderTextInput.cpp`
- buttons now share one renderer with explicit neutral, accent, and destructive tones:
  `src/workspace/WorkspaceShellRenderPrimitives.h`
  `src/workspace/WorkspaceShellRenderPrompts.cpp`
  `src/workspace/WorkspaceShellRenderSidebar.cpp`
  `src/workspace/WorkspaceShellRenderTextInput.cpp`
  `src/workspace/WorkspaceShellRenderBottomPanel.cpp`
- popup-menu rows plus the main selectable shell rows now share common row-background and
  primary-plus-secondary row-text helpers:
  `src/workspace/WorkspaceShellRenderPrimitives.h`
  `src/workspace/WorkspaceShellRenderMenus.cpp`
  `src/workspace/WorkspaceShellRenderSidebar.cpp`
  `src/workspace/WorkspaceShellRenderOverlay.cpp`
- project tabs, editor tabs, and bottom-panel tabs now share one strip-tab draw path and one
  close-glyph hover treatment:
  `src/workspace/WorkspaceShellRenderPrimitives.h`
  `src/workspace/WorkspaceShellRenderChrome.cpp`
  `src/workspace/WorkspaceShellRenderBottomPanel.cpp`
- common shell glyphs such as close, plus, check, chevron, and window controls now live in shared
  helpers instead of local ASCII fallbacks:
  `src/workspace/WorkspaceShellRenderPrimitives.h`
- prompts, overlays, command-prompt status text, and placeholder labels now share a consistent
  title-case and `A  |  B  |  C` hint style through shared text helpers:
  `src/workspace/WorkspaceUiText.h`
  `src/workspace/WorkspaceCommandPromptCoordinator.cpp`
  `src/workspace/WorkspaceShellRenderPrompts.cpp`
  `src/workspace/WorkspaceShellRenderOverlay.cpp`
  `src/workspace/WorkspaceShellRenderSidebar.cpp`
  `src/workspace/WorkspaceShellPresentation.cpp`
  `src/workspace/WorkspaceProjectPresentation.cpp`
- the empty-editor placeholder now uses the same titled-card vocabulary as the rest of the shell
  instead of a bespoke accent-strip card:
  `src/editor/EditorViewRenderer.cpp`

## Intentional Distinctions

Some surfaces still differ, but they now do so for functional reasons rather than because each
surface invented its own local chrome:

- editor hover popups remain a richer card family than compact tooltips because they need wrapped
  text, semantic emphasis, and optional actions:
  `src/workspace/WorkspaceShellHoverPopup.cpp`
  `src/workspace/WorkspaceShellRenderTextInput.cpp`
- menu-bar items still use a menu-specific active underline rather than pretending to be document
  tabs:
  `src/workspace/WorkspaceShellRenderChrome.cpp`
- tree git markers remain ultra-compact status markers because that surface optimizes for dense
  scanability:
  `src/workspace/WorkspaceShellRenderPrimitives.h`
  `src/workspace/WorkspaceShellRenderSidebar.cpp`

These are deliberate product distinctions, not unresolved standardization drift.

## Non-Findings

- single-line text-input behavior itself is no longer an inconsistency; the shell shares one
  insertion, caret, composition, and tail-truncation path:
  `docs/text-surface-unification.md`
  `docs/text-surface-audit.md`
- action and menu labels already had a healthy central contract before this audit, and that
  remains true:
  `src/workspace/WorkspaceCommandRegistry.cpp`
