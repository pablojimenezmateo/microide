## Context

The current shell exposes critical navigation and mode controls through low-visibility click targets and action-centric labels, especially in the status bar and layout-mode controls. The proposal calls for a full UX consistency pass across status bar, menus, and settings copy while preserving host ownership and existing architecture invariants (view-model-driven render TUs, service-backed state, and deterministic layout behavior).

This change crosses multiple host-owned UI surfaces:
- status-bar segments and click routing (`workspace-status-bar`)
- layout mode actions and menu presentation (`responsive-shell-layout`)
- sidebar headers and action rows across project/source-control surfaces
- settings overlay copy/labels (`settings-overlay-surface`)
- shared naming/discoverability expectations for command surfaces (new capability)

## Goals / Non-Goals

**Goals:**
- Make shell actions discoverable before click by exposing intent and current state in visible labels.
- Ensure mode controls are state-oriented (checked/unchecked) rather than verb-oriented toggles where appropriate.
- Expose source-control state directly in status bar semantics so users understand why status interactions matter.
- Remove misleading microcopy in settings/help surfaces and align phrasing to behavior.
- Keep all rendering host-owned and implemented through existing service and view-model seams.

**Non-Goals:**
- No redesign of editor text rendering, diff/merge interaction models, or plugin-owned UI extensions.
- No introduction of new plugin APIs for status-bar/menu rendering.
- No large visual theme overhaul; this is a clarity and interaction-contract revision, not a skin/theme rewrite.

## Decisions

1. **State-first language for persistent controls**
   - Decision: Prefer state labels (for example `Compact mode`) with menu checkmarks instead of action verbs (for example `Toggle compact mode`) where the control represents a persistent state.
   - Rationale: Mirrors established UX patterns (`Word Wrap`) and reduces cognitive translation from action to state.
   - Alternative considered: Keep action verbs and add tooltips only.
   - Why rejected: Tooltips do not solve discoverability for keyboard users or glanceable menu scanning.

2. **Status bar communicates both destination and reason**
   - Decision: Status-bar source-control segment must include branch identity and repository cleanliness/dirty state in the visible label, with consistent click affordance to Source Control.
   - Rationale: Converts hidden behavior into an understandable workflow cue and supports glanceable project health.
   - Alternative considered: Keep current click routing but improve tooltip text only.
   - Why rejected: Hidden click zones remain non-obvious without visible context.

3. **Discoverability contracts remain host-owned and testable**
   - Decision: Introduce a dedicated capability for command labeling/discoverability requirements and enforce via view-model and action metadata rather than ad hoc render strings.
   - Rationale: Keeps UI wording and affordance logic centralized, reviewable, and regression-testable.
   - Alternative considered: One-off copy edits in each surface.
   - Why rejected: Encourages drift and inconsistent semantics over time.

4. **Settings/help copy must be behavior-descriptive**
   - Decision: Replace misleading labels like `Tip` when text is normative behavior guidance, and require copy taxonomy (`Tip`, `Note`, `Warning`, plain description) to match intent.
   - Rationale: Prevents trust erosion from mislabeled guidance and improves readability for first-time users.
   - Alternative considered: Minimal text edits with no taxonomy.
   - Why rejected: Similar inconsistencies are likely to recur without explicit rules.

5. **Sidebar headers use a consistent two-row composition**
   - Decision: Host-owned sidebars with a primary selector/title and global actions SHALL use a consistent structure: first row for selector/title, second row for actions (for example `Refresh`, `Collapse`, `Stage All`, `Discard All`).
   - Rationale: Mixed one-row/two-row patterns for similar surfaces increase visual noise and reduce scan speed.
   - Alternative considered: Keep per-surface custom header layouts.
   - Why rejected: Inconsistency accumulates and makes the shell feel unpredictable.

## Risks / Trade-offs

- **[Risk] UI naming churn may conflict with user muscle memory** -> Mitigation: keep command IDs stable where possible and scope changes to labels/tooltips; preserve shortcuts.
- **[Risk] More verbose status labels could increase horizontal pressure** -> Mitigation: define compact-mode truncation/fallback rules and keep critical state first.
- **[Risk] Inconsistent adoption across surfaces** -> Mitigation: add cross-surface acceptance criteria and regression tests for status bar, menu labels, and settings copy taxonomy.
- **[Risk] Scope creep from "full UX revision" phrasing** -> Mitigation: bound this change to shell surfaces and interaction contracts listed in proposal/spec artifacts, including explicit sidebar header cohesion rules.

## Migration Plan

1. Add/modify OpenSpec requirements for status bar, responsive layout controls, settings copy taxonomy, and discoverability contracts.
2. Implement view-model and action-label updates in host-owned services/render builders, including sidebar header row structure alignment.
3. Update menu and settings text resources and related behavior/tooltips.
4. Add or update regression tests for label semantics, click routing, and status-state rendering.
5. Validate compact-mode behavior and status-bar segment truncation under narrow widths.

Rollback strategy: revert label and view-model behavior changes while preserving stable command IDs and persistence keys.

## Open Questions

- Should compact-mode status labels prioritize branch cleanliness over full branch name at extreme widths?
- Do we want one global UX copy style guide document in `docs/` as follow-up, or keep requirements localized to specs for now?
