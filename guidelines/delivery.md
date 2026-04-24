# Delivery Guide

Purpose: conventions for commits, reviewable slices, and documentation updates in `microide`.

## Development Philosophy

Work iteratively, but optimize for coherent engineering slices rather than tiny cosmetic diffs. Broad refactors are acceptable when they produce a cleaner and more correct architecture.

The default loop is:

1. implementation
2. tests
3. docs
4. commit

## Commits

- Keep each commit logically unified. A reviewer should be able to explain the architectural move or behavior change from the commit message and diff.
- Prefer a broad, correct refactor over a minimal patch that preserves a bad boundary.
- Avoid mixing unrelated cleanup into behavior work unless the cleanup is required to make the change correct.
- Mention major refactors plainly in the commit message.
- Keep the tree buildable and testable when practical, especially for multi-commit work.

## Reviewability

- Separate independent concerns when they can stand on their own.
- When a change spans multiple areas, group files by the behavior or boundary they implement, not by incidental proximity.
- If a subsystem move changes ownership, update the docs in the same slice so the new boundary is discoverable.
- Delete stale docs, examples, and compatibility shims once the new path is established.

## Completion Checklist

Before considering a change done:

- run the targeted build and tests for the changed area
- add or tighten regression coverage for meaningful bug fixes
- update durable docs when architecture, commands, or shipped behavior changed
- confirm the final result matches the repo priorities in `AGENTS.md`
