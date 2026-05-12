# UI/UX Debt Inventory — May 2026

Reviewed against the repository state on 2026-05-11. Scope is **user-facing experience** (interaction, clarity, consistency, responsiveness where visible, keyboard workflows, overlays/menus/chrome, rendering feedback). Backend refactors appear only where they surface as perceptible UX gaps.

---

## Executive summary

The shell demonstrates recent investment (responsive layout, status bar chrome, consolidated settings/help overlay, WCAG-aligned hit pads in spec and code paths). Concentrated UX debt clusters around **alignment between authored specs/guides and shipped chrome**—notably the bottom **status bar** (`openspec/specs/workspace-status-bar/spec.md` versus `WorkspaceShellChrome.cpp` / `RenderViewModelBuilder::BuildStatusBar`), **dual “status” surfaces** (breadcrumb plugin/LSP ribbons vs closed-enum footer bar), **documentation drift** (`guidelines/ui-shell.md`, `docs/editor-essentials.md`, `docs/implementation-guide.md`, several `openspec/specs/*/spec.md` “Purpose” placeholders), and **minimal platform accessibility semantics** for a custom-rendered SDL shell. Remaining shipped gaps called out elsewhere—multi-caret surround partial work, capped async search semantics—produce **honest friction** rather than undocumented bugs.

Approximate tally (non-exclusive items counted once at highest severity appearing in §3): **9** topical sections; **~28** distinct findings referenced below (**~8** high impact, **~12** medium, **~8** lower).

---

## Table of contents

1. [Shell chrome](#1-shell-chrome)
2. [Editor / editing UX](#2-editor--editing-ux)
3. [Diff / compare / merge UX](#3-diff--compare--merge-ux)
4. [Search / find UX](#4-search--find-ux)
5. [Git / SCM UX](#5-git--scm-ux)
6. [Terminal UX](#6-terminal-ux)
7. [Plugins surfacing UX](#7-plugins-surfacing-ux)
8. [Platform-level UX](#8-platform-level-ux)
9. [Consistency](#9-consistency)
10. [UX quick wins](#10-ux-quick-wins)
11. [Needs design decision](#11-needs-design-decision)

---

## 1. Shell chrome

**Impact — High**

- **Status bar segment contract vs inventory:** `workspace-status-bar` requires an ordered segment list including project identity, branch, cleanliness, language, indent, encoding, line/column, problems, LSP, and layout badge. Code enumerates placeholders in `StatusBarSegmentId` and `RenderViewModelBuilder::BuildStatusBar` (`src/workspace/StatusBarService.h`, `src/workspace/RenderViewModelBuilder.cpp`) but **`WorkspaceShell::RefreshStatusBar` is the sole writer** (`src/workspace/WorkspaceShellChrome.cpp`) and only sets Project (combined branch/cleanliness string), LayoutMode, LineColumn, and Indent. **Branch, Language, Encoding, Problems, and Lsp segments are never populated**, so users do not receive the richer at-a-glance model the durable spec describes.
- **Duplicate status semantics:** Plugin-contributed and LSP status strings are assembled for **breadcrumb** placement via `WorkspaceShell::ComputeVisibleStatusItems` and `ResolveStatusItems` (`src/workspace/WorkspaceShell.cpp`, `src/workspace/WorkspaceStatusRegistry.cpp`). The footer **status bar** remains a separate, host-enum-only surface (`WorkspaceShellRenderStatusBar.cpp`). Users may confuse which strip is authoritative for SCM vs language-server state.

**Impact — Medium**

- **Footer status tooltips under-specified in render VM:** Segment values carry `tooltip` in `StatusBarSegmentValue`, but **`StatusBarSegmentViewModel` omits tooltip** (`src/workspace/RenderViewModelBuilder.h`), and **`WorkspaceShell::RenderStatusBar` paints plain text only** (`WorkspaceShellRenderStatusBar.cpp`). The spec scenario calling for parity with tab tooltip patterns is not evidenced in this render path.

**Impact — Lower**

- **Settings preview vs live bar:** The settings overlay drills into segment affordances (`WorkspaceShellSettingsOverlay.cpp` references segment ids including those absent from production population). Preview UX may diverge from the live strip until segment wiring completes.

---

## 2. Editor / editing UX

**Impact — High**

- **Guide contradiction on shipped editor affordances:** `guidelines/ui-shell.md` (decorated-row section) states **sticky scroll “has no band in the renderer yet”** and that **snippet placeholder layers do not exist** in `EditorViewModel`. **`WorkspaceShellRenderFrame.cpp`** builds sticky lines into the editor view model and adjusts metrics when `sticky_lines` is non-empty, and **`editor::EditorViewRenderer::ComputeMetrics`** reserves `sticky_scroll_rows`. **`docs/active-work.md`** (2026-05-10) lists sticky scroll band and snippets among shipped essentials. Readers following `ui-shell.md` or **`docs/editor-essentials.md` § Snippets (partial)**—which still claims **no snippet engine / Insert Snippet overlay**—receive **stale mental models** relative to shipped behavior.

**Impact — Medium**

- **Partial multi-caret product story:** `docs/active-work.md` flags **multi-caret per-caret-selection surround** as remaining partial dependency on multicursor model work. **`openspec/specs/editor-multicursor-and-wrap/spec.md`** “Purpose” remains **TBD**, which obscures contract-level completeness for UX-sensitive editing flows.

**Impact — Lower**

- **Discoverability:** Many toggles surface in menus per `docs/editor-essentials.md` (indent guides, render whitespace, outline, occurrences, snippets, save normalization); **without a centralized in-app shortcut map for every toggle**, keyboard-first parity relies on discovery through keybinding registry menus or external docs (`WorkspaceCommandRegistry.cpp` ids listed in editor-essentials).

---

## 3. Diff / compare / merge UX

**Impact — Medium**

- **Specification skimmability:** `openspec/specs/diff-merge-editor/spec.md` packs **dense keyboard and navigation scenarios** contiguous to requirement headings—accurate but heavy for quick onboarding compared to surfaced in-app hints.

- **Keyboard choreography density:** Requirements codify **`[` / `]` hunk stepping**, **`Enter` / `o` open file**, **merge pane keys (`i`,`b`,`c`,`m`,`I`,`B`,`C`,`M`,`a`)** (`diff-merge-editor` spec scenarios). Practical debt is **onboarding/documentation**: these bindings are precise but easy to overlook without inline hints or unified compare/merge help rows.

**Impact — Lower**

- **Legibility palette contract:** Low-contrast added/removed/conflict fills with neutral foreground are specified (`diff-merge-editor`). Any future theme work must preserve that contract; regressions manifest as readability debt rather than correctness bugs.

---

## 4. Search / find UX

**Impact — Medium**

- **Honest truncation, limited progress granularity:** Sidebar + overlay summaries reflect **caps** (`WorkspaceShellProjectSearch.cpp` caps at **`kMaxProjectSearchResults == 200`**) and **`truncated` / “Searching N matches” during runs** (`WorkspaceShellRenderSidebar.cpp`, overlay summary in `WorkspaceShellRenderOverlay.cpp`). While running, counts reflect snapshots only—**there is no file-level or indexed-fraction denominator** surfaced, so anxious users lack explicit “completed X of Y files” reassurance on large repos.

- **Sidebar vs overlay duplication:** Project search appears in **persistent sidebar workflows** and a **modal overlay variant** (`OverlayMode::ProjectSearch`). Users may oscillate unless mental model distinguishes “browse while searching” versus “narrow modal.” No finding on wrong behavior—**consistency/copy** minimizes confusion.

**Impact — Lower**

- **Regex replace constraints:** Sidebar hints note **literal mode for replace-all** pathways (`WorkspaceShellRenderSidebar.cpp` messages such as `"R literal mode required"` pattern in search results). UX debt: **modes are correct but explanatory surface is narrow** unless users read sidebar hint rows.

---

## 5. Git / SCM UX

**Impact — Medium**

- **Status bar vs tree signals:** Footer **Project** segment text encodes **`branch_label + " [" + cleanliness + "]"`** (`WorkspaceShellChrome.cpp`). **Cleanliness merges tree-modified snapshots and sidebar git snapshots** (both OR’d). Divergent edge states are possible if one subsystem lags refresh; UX shows a single rollup string without distinguishing **source** of “dirty.”

**Impact — Lower**

- **Outbound / compare affordances:** `diff-merge-editor` and `responsive-shell-layout` treat compare/merge and git flows as durable; UX friction concentrates in **density of git sidebar chrome** (`WorkspaceShellRenderSidebar.cpp` outgoing menu paths). No single TODO marker surfaced in audited render paths—debt skews perception when many controls share two-row headers.

---

## 6. Terminal UX

**Impact — Medium**

- **Compact-mode terminal tab affordances:** `responsive-shell-layout` specifies compact **`+`** controls with mandated hit pads. Layout tests are spec-backed; perceptual regression risk remains when **dense tab strips** coexist with draggable reorder (`docs/implementation-guide.md` drag reorder semantics).

**Impact — Lower**

- **Contrast with editor:** Alternate screen, OSC 52, clipboard context menu flows are baseline-shipped (`docs/active-work.md`). UX debt manifests when **keyboard focus edges** blur between panel command surface and terminals—coordination-heavy areas without an audit artifact here beyond general shell focus conventions.

---

## 7. Plugins surfacing UX

**Impact — Medium**

- **Contributor status ribbons vs barred footer adoption:** Plugins register **`StatusItemView` entries** surfaced in **breadcrumb ribbons** (`WorkspaceStatusRegistry.cpp`, merge with synthetic **host LSP badge** priority in `WorkspaceShell.cpp`). The **footer bar spec explicitly forbids plugin segments**. Extension authors perceive **fragmented branding surfaces** versus a single authoritative strip.

**Impact — Lower**

- **Sidebar providers:** Builtin registry lists **five** modes plus plugin slots (`WorkspaceSidebarRegistry.cpp`). `docs/plugin-runtime-research.md` still describes an older “three built-in sidebar union” framing—partially superseded—the doc can misinform third-party UX expectations unless cross-checked against actual registry enumeration.

---

## 8. Platform-level UX

**Impact — Medium**

- **Assistive-tech gap:** Repo-wide substring scan for **`accessibility` / screen reader idioms under `src/`** does not expose a cohesive accessibility façade (hits were unrelated or syntax-token noise aside from syntax grammars listing HTML-ish tokens). **`responsive-shell-layout` references WCAG-style pointer targets**, but **no equivalent catalog for narration, keyboard-only discovery of implied controls, or high-contrast system integration** surfaced in audited paths.

- **Purpose placeholders in platform specs:** `openspec/specs/host-platform-support/spec.md` retains **Purpose: TBD** at archive time—users mapping cross-platform parity expectations rely on **`docs/active-work.md`** narratives instead (`HiDPI` note under responsive pass).

**Impact — Lower**

- **Fullscreen / OS chrome:** **`product-vision`** constrains native OS menus and detach windows—not a backlog item but a perceptual UX boundary when users expect desktop conventions.

---

## 9. Consistency

**Impact — Medium**

- **Architectural layering vs perceptual uniformity:** Sidebar project-search status strings concatenate in **`WorkspaceShellRenderSidebar.cpp`** alongside overlay variants in **`WorkspaceShellRenderOverlay.cpp`**. `ARCHITECTURAL` parity pushes copy into builders for hot-render paths elsewhere; sidebar still mixes **`JoinHintSegments` / `FormatEmptyState` inline**. Copy can drift between sidebar and overlay for equivalent states.

**Impact — Lower**

- **Spec “Purpose” fields:** **`ui-command-labeling-and-discoverability`**, **`workspace-status-bar`**, **`responsive-shell-layout`**, **`settings-overlay-surface`**, **`host-platform-support`**, and **`editor-multicursor-and-wrap`** carry **`## Purpose`** sections marked **“TBD - created by archiving change …”.** Consumers lack one-paragraph authoritative intent summaries.

---

## 10. UX quick wins

Evidence-backed wins that disproportionately clarify experience without widening scope arbitrarily:

| Win | Basis |
|-----|-------|
| **Populate or narrow the footer status-bar spec** | Either wire `Language`, `Encoding`, `Problems`, `Lsp` (plus distinct `Branch`/`Project` semantics) via `WorkspaceShellChrome.cpp`/`RenderViewModelBuilder`, or revise `workspace-status-bar`/`ui-command-labeling` to match deliberately minimal shipped segments. |
| **Refresh `guidelines/ui-shell.md` decorated-row appendix** | Remove obsolete sticky-scroll and snippet-absence claims; aligns engineers and reviewers with **`WorkspaceShellRenderFrame.cpp`** + shipped essentials narrative. |
| **Reconcile `docs/editor-essentials.md` Snippets §** With **`docs/active-work.md`** | Removes contradictory “no overlay” guidance if snippet UI is genuinely live. |
| **Propagate footer status tooltips into `StatusBarSegmentViewModel`** | Match `StatusBarSegmentValue::tooltip` to hover behavior described in **`workspace-status-bar`** (render + hit-testing path). |
| **Fill archived spec Purpose paragraphs** | One-sentence intents for TBD headings reduce onboarding cost for reviewers and planners. |

---

## 11. Needs design decision

Ambiguous UX topics where code and contracts suggest tension rather than clear bugs:

- **Single authoritative “status narrative”**: Choose whether **footer bar subsumes plugin/LSP summary** (spec change + implementation) or **breadcrumb remains the extension surface** (spec + guide rewrite clarifying intentional split).
- **Project search modality**: Affirm sidebar-first vs overlay-first as **recommended posture** so users are not confronted with duplicated entry points differing only subtly.
- **Accessibility stance for SDL-rendered IDE**: Decide whether baseline is **pure self-voicing omission** (explicit non-goal doc) versus a future **narrow AT bridge** milestone; informs whether WCAG citations stay pointer-only (`responsive-shell-layout`) or broaden.
- **Tests / Problems placeholders**: Sidebar modes exist (`Tests`, `Problems`); experiential completeness vs stub states should be articulated in **`docs/active-work.md`** or product guide to match user expectations (`docs/implementation-guide.md` still catalogs those modes superficially versus depth of Problems dogfooding narratives).

---

## References (non-exhaustive)

- Specs: `openspec/specs/workspace-status-bar/spec.md`, `openspec/specs/ui-command-labeling-and-discoverability/spec.md`, `openspec/specs/responsive-shell-layout/spec.md`, `openspec/specs/settings-overlay-surface/spec.md`, `openspec/specs/diff-merge-editor/spec.md`, `openspec/specs/product-vision/spec.md`, `openspec/specs/editor-multicursor-and-wrap/spec.md`, `openspec/specs/host-platform-support/spec.md`
- Guides/docs: `guidelines/ui-shell.md`, `docs/active-work.md`, `docs/implementation-guide.md`, `docs/editor-essentials.md`, `docs/text-surface-unification.md`, `docs/plugin-runtime-research.md`
- Code: `src/workspace/WorkspaceShellChrome.cpp`, `src/workspace/RenderViewModelBuilder.cpp`, `src/workspace/WorkspaceShellRenderStatusBar.cpp`, `src/workspace/WorkspaceShell.cpp`, `src/workspace/WorkspaceShellRenderSidebar.cpp`, `src/workspace/WorkspaceShellRenderOverlay.cpp`, `src/workspace/WorkspaceShellProjectSearch.cpp`, `src/workspace/WorkspaceShellRenderFrame.cpp`, `src/workspace/WorkspaceSidebarRegistry.cpp`, `src/workspace/WorkspaceStatusRegistry.cpp`

---

*Method: `rg` sweeps for `TODO`/`FIXME` under `src/workspace` render/layout units returned **no hits** at inventory time; evidence instead draws from specs, architectural comments, structural gaps between spec lists and setter coverage, render/view-model structs, and active-work/guide contradictions.*
