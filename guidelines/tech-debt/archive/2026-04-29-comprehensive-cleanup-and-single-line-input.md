# Comprehensive tech-debt cleanup + single-line input model

- Date: 2026-04-29
- Area: workspace, editor, architecture
- Source: `comprehensive-tech-debt-cleanup`; commit `0aa44cb`; `openspec/changes/archive/2026-04-29-comprehensive-tech-debt-cleanup/`
- Related:
  - Specs: `openspec/specs/workspace-architecture/spec.md`, `openspec/specs/persisted-state-format/spec.md`, `openspec/specs/shared-edit-primitives/spec.md`

## Summary

The 2026-04-29 comprehensive cleanup closed the load-bearing workspace-architecture debts (items
1–4, 7) plus a sanitizer-surfaced TSAN race, and shipped the shared single-line text-input model.

## Resolution

### Closed debt from the comprehensive cleanup

The following previously tracked debts were closed on 2026-04-29 by `comprehensive-tech-debt-cleanup`:

- item 1 (`WorkspaceShell` ownership bottleneck): closed
  - `WorkspaceShell.h` now satisfies the architectural size contract
  - legacy `WorkspaceActionContext` file names were removed from the tree
- item 2 (coordinator separation still superficial): closed for this phase
  - architectural lint now hard-fails key boundary regressions
- item 3 (active editor viewport ownership migration): closed
  - stale shell-level viewport alias paths were removed
- item 4 (render and hover shell reach): closed for this phase
  - render-path architectural constraints are enforced by lint
- item 7 (single-line shell text input model): closed
  - shared single-line editor and key-handler model is now shipped
- `WorkspaceLspClient` TSAN race (reported during sanitizer bring-up): closed
  - request/callback ownership synchronization was fixed and verified with TSAN runs in the
    sanitizer matrix

### §7 Single-line shell text input model

Addressed on 2026-04-29 by introducing shared `SingleLineEditor` and `SingleLineKeyHandler` for
migrated single-line surfaces. No further migration was required for task closure.

Relevant code:
- `src/editor/SingleLineEditor.h`
- `src/editor/SingleLineKeyHandler.h`
- `tests/SingleLineEditorTests.cpp`

### Open follow-ups after the 2026-04-29 cleanup (all subsequently closed)

The cleanup shipped the durable contracts in the three specs above. The follow-ups it left were
later closed:

1. `WorkspaceShellTestAccess.h` trim: closed in the comprehensive tech-debt and perf-harness pass —
   the header is a small scoped aggregator with a hard architectural size gate, and category-(a)
   wrappers were migrated to direct shell APIs / event helpers.
2. `WorkspaceShell*.cpp` companion files keeping behavior on the shell namespace: addressed by the
   2026-05-20 companion-sprawl slices (see `2026-05-20-textviewport-and-shell-decomposition.md`).
   Policy retained: any new behavior lands on a service, not a new `WorkspaceShell*.cpp` companion.
3. Legacy persistence importer: closed in `codebase-cleanup-perf-and-debt`;
   `WorkspacePersistenceLegacyFormat.*` was deleted and persistence stays on structured records only.
4. Architectural-lint coverage gap: closed — discovered render-unit scanning is active,
   plugin/coordinator size checks are hard-fail, and the shell test-access header has an explicit cap.
5. Oversized coordinator translation units: closed — coordinator units were decomposed and the
   coordinator TU-size rule is hard-fail.
6. Project-content/indexing architecture (item 5): event-driven watcher + background executor landed;
   workspace wiring closed by `deferred-work-and-throughput-pass` (see
   `2026-05-19-search-index-event-watch.md`).
