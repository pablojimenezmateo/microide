# WorkspaceShellTestAccess Audit (Task 9.1)

> **Status: archived (2026-05-23).** Historical record from the comprehensive tech-debt cleanup.
> Category-(a) wrappers were removed; the header is now a scoped aggregator with a size gate.
> Current policy: [`guidelines/testing.md`](../../guidelines/testing.md),
> [`dev-docs/project/known-tech-debt.md`](../project/known-tech-debt.md) (TestAccess follow-up closed).

- File: `src/workspace/WorkspaceShellTestAccess.h`
- Top-level line count: 35 (scoped include fragments hold implementation methods)
- Method count across `src/workspace/testaccess/*.inc`: 274
- Used methods: 274
- Obsolete methods: 0

## Category Assignment
- `a` duplicates a service-public API: migrated away in this pass and deleted from `TestAccess`.
- `b` test-only state/interaction access: remaining methods.
- `c` obsolete: none remaining.

## Category A Removed In This Pass
- Event forwarding wrappers (replaced with direct `WorkspaceShell::HandleEvent(...)` test helpers):
  - `HandleKeyEvent`
  - `HandleWindowMouseLeave`
  - `HandleWindowFocusEvent`
  - `HandleMouseButtonDown` (both overloads)
  - `HandleMouseButtonUp`
  - `HandleMouseMotion`
  - `HandleMouseWheel`
- Direct public API wrappers (call sites switched to direct shell calls):
  - `RequestQuit`
  - `ConsumeQuitRequested`
  - `ConsumeWindowAction`
  - `NextAnimationDelayMs`
  - `WindowHitTest`
  - `WindowDragRegionContains`
  - `UiScale`
  - `CurrentCaretDirtyRect`
  - `ConsumePendingRenderInvalidation`

## Category C Cleanup
- Removed obsolete methods during task 9.3 pass.
- Current scan finds no `TestAccess` methods with zero external/internal references.
