## Context

This change crosses several subsystems because the current rough edges share the same root cause: durable editor and workspace contracts are still incomplete. The editor still behaves as a single-caret, no-soft-wrap surface; `DirectoryTree`, `ProjectFileScanner`, and the project watcher all treat `.gitignore` as a hard hide-and-skip boundary; compare and merge layout still clamp divider motion through fixed minima; and AI provider access is built around `WorkspaceProviderBridgeManager`, which assumes a long-lived bridge subprocess per provider agent.

There is also evidence that some state boundaries are not yet as strict as the current architecture wants them to be: project-specific divider fractions, wrap-related settings, ignored-tree expansion state, and provider-session behavior need to live with project-owned services rather than accreting on the shell. The proposal therefore treats this as one coherence pass across editor behavior, project-scope ownership, tree enumeration, and provider runtime architecture.

## Goals / Non-Goals

**Goals:**
- Add multiple cursors and soft wrap as first-class editor behaviors without regressing typing, scrolling, or hit-testing responsiveness
- Keep ignored files and directories accessible from the project tree while keeping them out of background indexing and tool pipelines by default
- Move project-specific UI/data behavior behind project-owned services and persistence records rather than shell-global state
- Reduce compare and merge visual contrast while preserving neutral foreground text readability
- Allow pane dividers to move to content-derived minima instead of arbitrary conservative clamps
- Replace the mandatory provider-bridge model with a transport-neutral host-owned provider runtime that can use direct providers without a bridge binary
- Delete obsolete helper methods, adapters, and compatibility paths once the new seams are active

**Non-Goals:**
- Terminal soft-wrap changes or terminal-layout redesign
- Column/block selection in the first multiple-cursor slice; the initial scope is discrete carets and selections
- Making ignored descendants participate in search, AI context, blame, git scans, or background watchers by default
- Replacing host-owned AI workflows with plugin-owned implementations
- Allowing panes to collapse below content viability; the change removes arbitrary clamps, not all minima

## Decisions

### D1. Editor state gains an explicit multi-selection model plus wrapped layout cache

The editor will keep one logical text buffer and one ordered `SelectionSet` per viewport, with a distinguished primary selection plus zero or more secondary selections/carets. Edit commands operate over the full set in one transaction, and undo/redo stores each multi-caret edit as a single user action. Caret motion remains logical-first; the view layer maps logical positions onto wrapped display rows.

Soft wrap will be implemented through a cached wrapped-line map keyed by buffer revision, viewport width, font metrics, tab width, and wrap mode. Rendering, hit-testing, vertical caret motion, and scrollbar geometry all consume the same wrapped-line map so that wrap decisions are consistent everywhere.

Ligatures are disabled for editor-family text surfaces as part of the same presentation layer change. Code, compare, and merge text must render one codepoint sequence at a time; markdown/chat rendering is unaffected.

**Why this shape:** multiple cursors and soft wrap are both view-layer multipliers on one logical buffer. Splitting them into separate ad hoc state models would make undo, hit-testing, and scrolling inconsistent.

**Alternatives considered:**
- Extend the current single-caret state with secondary ad hoc fields. Rejected because it makes selection invariants and undo grouping brittle.
- Implement soft wrap as a render-only concern. Rejected because caret motion, hit-testing, and scroll math would diverge from what the user sees.

### D2. Ignored-node visibility is separated from background eligibility

The project tree and the background file index will stop sharing one “visible means indexed” rule. Instead, the tree model will track three concepts independently:
- node visibility in the tree
- whether a node is ignored by `.gitignore`
- whether descendants are materialized yet

Ignored files remain visible leaf nodes. Ignored directories appear as collapsed opaque nodes whose immediate children are enumerated only when the user expands that node. Expansion materializes one level at a time and may cache the result for the visible tree, but ignored descendants remain excluded from search, AI context, blame, git refresh, and background indexing unless a specific file is opened explicitly.

Opening an ignored file is a direct path-open operation and does not “promote” the whole ignored subtree into the global index.

**Why this shape:** the user wants access to the full on-disk tree without paying the cost of treating ignored content as part of the active project corpus.

**Alternatives considered:**
- Keep hiding ignored nodes. Rejected because it blocks legitimate access to files such as `.env.local` and ad hoc inspection inside ignored directories.
- Fully index ignored directories after the first expansion. Rejected because it recreates the very startup and background-cost problem the change is meant to avoid.

### D3. Project-owned services become the sole owner of project-scoped UI state

Any state that changes per project, survives project switching, or influences project-local workflows will be owned by project state/services and persisted through the existing persistence pipeline. That includes wrap mode, ignored-directory expansion state, per-project compare/merge divider fractions, and any provider-session state that should not leak across projects. The shell keeps only host-global concerns such as window/event wiring and service lifetime.

Divider motion will use content-derived minima instead of broad fixed clamps. Sidebar width, editor split extents, compare divider placement, and merge divider placement will be limited only by the minimum space required to keep the participating surfaces meaningful: gutter plus text column for compare/merge panes, viable editor viewport width for split panes, and viable sidebar row presentation for the project tree.

**Why this shape:** project switching is the natural boundary for these behaviors. If the state leaks through shell-global members, users will continue to see one project’s presentation choices bleed into another.

**Alternatives considered:**
- Leave divider/layout state on the shell because it is “UI state.” Rejected because the shell outlives project activation and is the wrong ownership boundary.
- Remove minima entirely. Rejected because zero-area panes are not useful and would break hit targets and scrollbar math.

### D4. AI provider access moves from bridge-first to runtime-first

The durable contract exposed to chat and inline completion will become a host-owned provider runtime interface rather than a mandatory bridge subprocess manager. Direct API-backed providers will run through host-managed runtime adapters by default. Sidecar or subprocess-backed providers remain supported, but only as one transport strategy behind the runtime contract, not as the required architecture for all providers.

The runtime interface covers: auth check, model discovery, streaming chat/completion output, tool-call round-trips, cancellation, and capability reporting. Callers do not branch on whether the provider is direct HTTP, in-process SDK-backed, or sidecar-backed.

This change also creates a clean exit path from `WorkspaceProviderBridgeManager`-specific call sites: the existing newline-delimited JSON bridge protocol can survive as one adapter while the rest of the workspace stops depending on bridge-specific naming and lifecycle assumptions.

**Why this shape:** the current bridge-first model adds process management complexity and naming confusion even for providers that can be handled directly and safely by the host.

**Alternatives considered:**
- Keep the bridge as the only supported provider contract. Rejected because it imposes avoidable runtime overhead and architectural coupling.
- Force all providers in-process immediately. Rejected because some providers benefit from isolation or already exist behind a sidecar protocol.

### D5. Diff/merge presentation adopts explicit low-contrast palette tokens

Compare and merge fills will move to explicit palette tokens with lower saturation and lower opacity, while foreground text stays neutral and independent of the row fill. The change is intentionally modest: it should reduce color cast and eye strain without obscuring added/removed/conflicted status.

The palette change stays in the shared diff/merge presentation path so editor/compare/merge do not fork text color logic.

**Why this shape:** the current problem is not missing status color; it is excess fill influence on legibility.

**Alternatives considered:**
- Remove background fills entirely. Rejected because it would weaken scanability of diff hunks.
- Per-surface palette tuning. Rejected because the shared compare/merge pipeline should keep one consistent meaning for decorations.

## Risks / Trade-offs

- **[Risk] Multi-caret plus wrap complexity increases editor-state invariants** → Mitigation: define logical-position and display-position helpers centrally and back them with focused regression coverage before broad integration work.
- **[Risk] Users may expect expanded ignored directories to participate in search or AI context automatically** → Mitigation: keep ignored-state badges/labels explicit and preserve the rule that expansion is for browsing only, while direct file open still works normally.
- **[Risk] In-process providers increase host responsibility for networking and failures** → Mitigation: preserve sidecar transport as an optional adapter and keep the runtime asynchronous with the same wake-event delivery rules.
- **[Risk] Smaller pane minima can expose layout bugs in narrow states** → Mitigation: derive minima from concrete surface needs, add resize regression tests, and keep resettable default fractions.
- **[Risk] Legacy-path deletion can touch many files** → Mitigation: stage the cutover by introducing the new runtime/service seam first, then remove the bridge-specific and shell-scoped helpers once callers are migrated.

## Migration Plan

1. Add the new editor, ignored-tree, and provider-runtime contracts while preserving existing user-visible behavior where possible.
2. Migrate editor, project tree, diff/merge, and AI callers onto the new seams and project-owned persistence records.
3. Delete legacy helpers, bridge-first entry points, and shell-scoped accessors once all call sites are cut over.
4. Update durable docs/specs and capture targeted performance evidence for wrapped typing/scrolling, ignored-tree open/expand, and provider-runtime responsiveness.
5. If persisted field names or provider configuration records change, retain only the minimal one-shot import/upgrade path needed by `PersistenceService`; do not leave dual write paths in place.

## Open Questions

- Should ignored-directory expansion remain a cached snapshot until manual refresh, or should visible ignored branches gain a shallow on-demand refresh trigger while still staying out of the global watcher/index pipeline?
- Do we want an explicit user toggle for including the currently open ignored file in non-AI tooling later, or is “open/edit works, background tools still ignore it” the durable long-term rule?
