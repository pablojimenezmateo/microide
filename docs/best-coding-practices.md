# Best Coding Practices

This document captures durable practices for keeping `microide` maintainable as the codebase grows.

## Keep Ownership Clear

- Give each translation unit a narrow, obvious responsibility.
- Keep reusable deterministic helpers separate from stateful UI orchestration.
- Prefer adding a focused subsystem file over extending a catch-all file.
- If a "shared" module starts collecting unrelated helpers, split it again before it becomes a second dumping ground.

## Keep Coordination Thin

- Central coordinator code should delegate to subsystem-specific code instead of owning every implementation detail.
- Decide where new state belongs before adding it. If ownership is unclear, the design is not ready.
- Prefer explicit inputs and outputs over hidden coupling through global or cross-cutting mutable state.

## Keep External Dependencies Behind Services

- Keep search, git, trash, and other OS or tool integrations behind small service interfaces.
- UI code should consume structured results rather than parse command output directly.
- If a fast external path and a built-in fallback both exist, test the shared contract and both execution paths.

## Prefer Small, Safe Refactors

- Refactor in behavior-preserving steps that compile cleanly.
- Remove dead helpers and duplicate logic as soon as the new boundary is established.
- When a refactor reveals reusable deterministic logic, promote it into a testable helper instead of leaving it buried in UI code.
- Treat refactor logs as temporary working notes; move the lasting guidance into stable docs.

## Test By Domain

- Split tests by subsystem or behavior domain instead of growing a single monolithic file.
- Keep shared test helpers small and reusable.
- Put pure helper logic under direct unit coverage.
- Use committed fixtures for compare, git, search, and other workflows that are hard to reason about from inline strings alone.
- If test failures are hard to localize, the suite boundaries are too coarse.

## Treat Build Hygiene As Part Of The Design

- Build directories and local scratch trees do not belong in commits.
- Keep compiler warnings enabled and treat new warnings as bugs.
- Update build files when the source layout changes so the structure in the repository matches the structure in the build.

## Review Checklist

- Does this change reduce or increase coupling?
- Is the owning subsystem obvious?
- Are helper functions deterministic and named by behavior?
- Are error paths explicit and easy to debug?
- Did the targeted build and tests run after the change?
