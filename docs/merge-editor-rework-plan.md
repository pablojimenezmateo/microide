# Merge Editor Rework Plan

This document is forward-looking.
It describes the intended rework of the three-way merge editor as of 2026-04-10.

## Reviewed Code Anchors

This plan was checked against the current codebase with emphasis on:

- `src/compare/MergeModel.h`
- `src/compare/MergeModel.cpp`
- `src/workspace/WorkspaceShell.h`
- `src/workspace/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellCompare.cpp`
- `src/workspace/WorkspaceShellCompareRender.cpp`
- `src/workspace/WorkspaceShellMouse.cpp`
- `src/workspace/WorkspaceShellInput.cpp`
- `src/workspace/WorkspaceShellPersistence.cpp`
- `src/workspace/WorkspaceShellShared.h`
- `src/editor/TextViewport.h`
- `src/editor/TextViewport.cpp`
- `tests/CompareModelTests.cpp`
- `tests/WorkspaceShellSessionTests.cpp`

The latest directly relevant performance commit at review time is:

- `fdd109f5ec4da125de5bb5ecbd4e7fa8416591df` `feat: performance improvements for large files`

## Confirmed Product Decisions

These interaction decisions are fixed for this rework:

- manual edits in `Result` must be preserved around accepted conflicts
- hover previews must be transient and must not commit a choice
- hovering `Incoming` or `Current` previews that choice immediately in `Result`
- hovering a conflict in `Result` must expose `Base`, `Incoming`, `Current`, and `Both`
- `Both` means `Incoming` first, then `Current`
- `All Auto` must be removed from the UI and from the code
- the three panes must become resizable with two draggable vertical dividers
- divider positions should persist in session state

## Current Problems

The current merge editor has several architectural issues:

- toolbar buttons draw label backgrounds over the button fill, and the labels are not centered
- merge actions are selection-driven from a global toolbar instead of conflict-local and hover-driven
- `Result` is regenerated from `MergeModel` choices and loaded back into `result_viewport` on every change
- `Result` is therefore not a true editable source of truth
- opening large files still does too much eager work for merge tabs
- accepting one merge choice rebuilds and re-highlights too much state
- `Incoming`, `Result`, and `Current` widths are fixed
- session persistence stores merge hunk choices, but that model is too weak once `Result` becomes editable

The current large-file cost is especially visible in:

- eager full-buffer tokenization for `incoming_tokens`, `current_tokens`, and `result_tokens`
- full `BuildMergeDisplayModel` rebuilds
- full `MergeResultText` regeneration
- full `result_viewport.LoadContent(...)` reloads

## Target UX

The new merge editor should behave like this:

- `Incoming | Result | Current` remain visible side by side
- `Result` is a real editable text buffer inside the merge editor
- hovering a conflict in `Incoming` previews the incoming version in `Result`
- hovering a conflict in `Current` previews the current version in `Result`
- hovering a conflict in `Incoming` or `Current` shows one contextual `Accept` button below that source conflict
- hovering a conflict in `Result` shows contextual actions for `Base`, `Incoming`, `Current`, and `Both`
- moving between hover actions updates the preview immediately
- clicking an action commits only that conflict span into the editable `Result` buffer
- manual edits outside that conflict span remain untouched
- panes can be resized by dragging either divider

## Core Architecture Change

The key change is this:

- `Result` stops being a regenerated mirror of hunk choices
- `Result` becomes the authoritative editable buffer
- merge choices become conflict-local patch operations against that buffer

That change is necessary for both correctness and performance.

## Data Model Rework

### 1. Remove `Auto` from merge choice state

`Auto` should not survive this rework.

Instead:

- remove `MergeChoice::Auto`
- remove `All Auto` mouse handling, keyboard handling, labels, and persistence
- remove `MergeChoiceLabel(Auto)` and any session parsing for `auto`
- replace auto behavior with one-time bootstrap logic when the merge tab opens

Bootstrap behavior should be:

- apply non-conflicting changes automatically when the tab is built
- seed conflicting regions with `Base` in the initial `Result` buffer to match current behavior unless a better preserved output source is introduced later

After initialization, there is no `auto` state anymore.

### 2. Keep a static conflict model

`MergeModel` should become the static description of the three input files:

- stable hunk ids
- source line ranges
- base/incoming/current text for each conflict
- whether the change is auto-resolved at bootstrap or still conflict-bearing

It should no longer be the live owner of the full result text.

### 3. Add tracked result spans per conflict

The merge tab needs explicit tracked spans for where each conflict currently lives in `Result`.

Each conflict should track:

- stable conflict id
- current start line in `result_viewport`
- current end line in `result_viewport`
- last committed choice
- whether the conflict span is still valid after edits

Accepting a choice then becomes:

- replace the tracked line span in `result_viewport`
- update downstream tracked spans by the line delta
- refresh only the affected merge metadata

This is what preserves manual edits around the chosen conflict.

## Rendering Rework

### 1. Use the editor renderer for `Result`

The center pane should stop using precomputed `display_model.result_text` rows as its only source.

Instead:

- render `Result` through `editor::TextViewport` and the normal editor rendering path
- keep the center pane editable, selectable, scrollable, and syntax highlighted like a normal buffer
- overlay merge-specific conflict affordances on top of that editor surface

This reduces bespoke rendering work and reuses existing editor behavior.

### 2. Keep source panes read-only

`Incoming` and `Current` stay read-only.

They should render:

- line numbers
- syntax highlighting
- conflict-aware tinting
- hover affordances

But they do not need to be editable viewports.

### 3. Remove full-row dependency for hover preview

Hover preview must not mutate the document.

Instead:

- compute a preview choice for the hovered conflict
- render a temporary preview in `Result` for that conflict span only
- discard it immediately when hover leaves or changes

This preview state should live separately from the committed buffer state.

## Interaction Model

### 1. Source-pane hover

When hovering a conflict in `Incoming`:

- preview `Incoming` in `Result`
- show a single `Accept Incoming` action below that source conflict

When hovering a conflict in `Current`:

- preview `Current` in `Result`
- show a single `Accept Current` action below that source conflict

### 2. Result-pane hover

When hovering a conflict in `Result`:

- show `Base`, `Incoming`, `Current`, and `Both`
- hovering one of those actions previews it immediately
- clicking one of those actions commits it to `Result`

`Both` always inserts:

1. `Incoming`
2. `Current`

### 3. Selection is secondary

The current selected-hunk model can remain for keyboard navigation, but it should no longer drive the primary mouse workflow.

Primary mouse behavior should be conflict-local and hover-based.

## Layout Rework

### 1. Add two merge-pane dividers

The merge layout should support:

- left divider between `Incoming` and `Result`
- right divider between `Result` and `Current`
- minimum pane widths so no pane collapses into unusable space

### 2. Persist divider positions

Session state should add persisted merge layout fields.

Recommended representation:

- store divider positions as normalized fractions
- clamp them on restore
- derive the rightmost pane width from the remaining space

This should be added to:

- `PersistedEditorTabState`
- merge-tab save and restore code in `WorkspaceShellPersistence.cpp`

## Performance Plan

The merge editor rework should preserve the spirit of the large-file work in `fdd109f`.

The performance rules are:

- no full `result_viewport.LoadContent(...)` on hover
- no full `BuildMergeDisplayModel(...)` rebuild on hover
- no full-file syntax tokenization on tab open for merge source panes
- no synchronous recomputation proportional to whole-file size on pointer move
- no `ReadFileText(output_path)` on every per-conflict accept

Concrete changes:

- move `Incoming` and `Current` tokenization to progressive or viewport-scoped population, mirroring compare-tab behavior
- reuse `TextViewport` syntax and large-file behavior for `Result`
- refresh only the changed result span and downstream conflict-span offsets after accept
- separate static merge metadata from live editable buffer state
- cache persisted-output equality baseline instead of rereading output on each change

## Persistence Changes

The current `merge_hunk_choices` persistence is not enough once `Result` is editable.

We should split persistence concerns:

- persist pane layout
- persist navigation state
- persist committed conflict choices if they are still useful for rebuilding metadata
- decide explicitly whether dirty merge buffers should persist as full text snapshots or only through save prompts

Recommended first implementation:

- keep the existing save/discard prompt behavior for dirty merge buffers
- persist divider positions, selection, and scroll state
- persist committed conflict choices only as auxiliary metadata
- do not rely on hunk choices alone to reconstruct user edits

If unsaved merge-result restore becomes important, persist the full dirty `Result` buffer separately as a follow-up.

## Visual Cleanup

The first visual cleanup pass should be small and isolated:

- center button labels horizontally and vertically
- stop button labels from painting an opaque background over the button fill
- keep button hit boxes stable while the deeper merge rework is in progress

This should be done before or alongside the structural merge changes because it is user-visible and low risk.

## Implementation Phases

### Phase 1. Button cleanup and layout groundwork

- fix merge-toolbar button label rendering
- center button text
- add merge-pane divider geometry and drag handling
- persist divider positions

### Phase 2. Remove `Auto` and split static vs live merge state

- remove `MergeChoice::Auto`
- remove all `All Auto` code paths
- change merge-tab bootstrap to resolve only non-conflicts automatically
- introduce explicit conflict-span tracking for the result buffer

### Phase 3. Make `Result` a true editable pane

- render the center pane through `TextViewport`
- route text input, selection, caret, and scrolling to the merge result pane when active
- keep merge-specific overlays separate from base editor rendering

### Phase 4. Add hover preview and accept actions

- source-pane hover previews
- source-pane contextual accept button
- result-pane contextual choice actions
- transient preview state that never dirties the buffer by itself

### Phase 5. Performance hardening

- progressive tokenization for source panes
- partial refresh after accept
- remove remaining full-file recomputation from normal pointer movement and single-conflict accepts
- validate large-file behavior against the recent performance work

### Phase 6. Test coverage

- merge-model bootstrap tests without `Auto`
- accept-one-conflict tests that preserve surrounding manual edits
- tracked-span update tests for insertions and deletions around conflicts
- hover-preview tests proving no committed state changes
- session restore tests for divider positions and merge navigation state
- large-file regression coverage for merge-tab open and repeated accept operations

## Acceptance Criteria

This rework is complete when:

- merge-toolbar buttons render correctly
- `All Auto` no longer exists anywhere in the product or merge model
- `Result` is directly editable in the merge tab
- hovering a source conflict previews the change immediately
- clicking accept patches only that conflict in the current edited result
- hovering a result conflict exposes `Base`, `Incoming`, `Current`, and `Both`
- merge panes are resizable and their widths restore across sessions
- large-file merge open and per-conflict accept no longer feel dominated by whole-file rebuilds

## Risks To Watch

- tracked conflict spans can become hard to maintain if edits cross multiple conflict regions
- embedding an editable viewport in only the center pane requires careful input routing and caret rendering
- session restore semantics become less trivial once merge tabs can contain manual unsaved edits
- syntax-highlighting work must stay incremental or the rework will reintroduce large-file regressions

The implementation should prefer correctness of `Result` editing and bounded per-conflict update cost over preserving the exact current merge-tab internals.
