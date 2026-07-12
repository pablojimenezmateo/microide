# 2026-07-12 deferred-backlog sweep (cross-subsystem bug-hunt passes 5–24 closeout)

- Date: 2026-07-12
- Area: terminal, editor/folding, git, compare/merge, plugin host, LSP/DAP, control,
  keybindings, tab/session, persistence, util, platform, file index
- Source: the 2026-07 cross-subsystem bug-hunt passes (5–24) plus the pass-12/13
  "deferred from" lists, all of which had recorded a backlog in
  `dev-docs/project/known-tech-debt.md`
- Commits: `Deferred backlog sweep — Batch A … I` (nine focused commits), each with
  regression coverage; the full suite plus a final ASAN/UBSAN/TSAN pass stay green.

## Summary

The deferred backlog accumulated across bug-hunt passes 5–24 was triaged in full
against the current tree and cleared. Each fixable item landed with a regression
test. Genuine non-defects were confirmed as won't-do; the four platform-only
Windows/macOS items stay deferred (cannot compile/validate on the Linux-primary
tree). The four larger design items opted into for this sweep — primary-screen
terminal scroll regions, folding collapse-preservation across edits, deep-collapsed-
diff syntax highlighting, and the file-index initial-scan clobber — all shipped.

## Resolution — by subsystem

- **Terminal (Batch A/B).** VT/FF index preserving the column (like IND) instead of
  zeroing it; a charset designation aborts on an embedded ESC/CAN/SUB; an alt-screen
  LF/index below a custom region at the physical bottom no longer scrolls the whole
  screen; the `[process exited]` marker restores the primary buffer first when the
  child dies on the alt screen; `ParseCsiParametersInto` reuses a thread_local buffer
  (no per-CSI heap alloc); basic SGR foregrounds (30–37) track the bold flag for
  brightness (order-independent, reverted by SGR 22) while explicit bright (90–97)
  stays fixed; terminal URL hit-testing maps the grid column to a byte offset.
  **Primary-screen scroll regions**: DECSTBM / SU / SD / IL / DL now honored on the
  primary buffer (gated on a custom region so the common full-screen path is
  unchanged); `CSI r` clamps an out-of-range bottom instead of discarding; a C0
  control embedded mid-CSI is executed then the sequence continues; resize re-expands
  the saved primary/alternate full-window regions.
- **Editor / folding (Batch C).** Multi-caret soft-tab aligns each caret to its own
  next tab stop; multi-caret indent/outdent preserve every caret; MoveLine carries a
  whole-line selection whose exclusive end sits at column 0 of the line after the
  block; a collapsed fold survives a line-count-changing edit above it (the remap
  shifts previous collapsed openers/closers by the net edit delta at/after the anchor).
- **Git (Batch D).** Single-file unstage/discard skips the whole-index staged-rename
  probe for the common non-rename case; `DiscardAll` trashes files with no committed
  content (untracked + staged-new) instead of `git clean`-deleting them; the commit
  staged summary is cached against the git generation;
  `StagedDiffContainsConflictMarkers` returns optional<bool> ("could not determine" vs
  "clean"); `PatchApplyService` runs its blame/compare/clean-tab invalidations whenever
  the patch actually applied; a rename source is badged Deleted in the tree status map;
  `LowerLine` no longer inflates the lowercase counters in case-sensitive mode.
- **Compare / merge (Batch E).** Syntax highlighting reaches hunks deep in a collapsed
  large-file diff (the tokenizer window is derived in model space from the visible
  presentation rows, with a continued-redraw catch-up); the merge conflict separator
  tolerates a non-default `merge.conflictMarkerSize`.
- **Plugin host (Batch F).** The surface anchor line rejects values that would wrap the
  uint32_t cast; `PluginSurfaceStore` gains `RetargetPathPrefix`/`ClearPathPrefix`,
  wired into the rename/delete coordinator so inline surfaces follow a rename and are
  dropped on delete.
- **LSP / DAP (Batch G).** `didChange`/`didChangeIncremental` advance the document
  version only on a successful enqueue; an over-reporting adapter's empty "load more"
  page clears `more_available`; replacing a debug container's child list recursively
  erases the old subtree from the node maps.
- **Control / keybindings / tab / persistence / util / platform (Batch H).**
  `ParseKeyChord` passes SDL's canonical uppercase scancode name; virtual-document
  reloads walk every editor group; session restore sanitizes the layout pixel floats;
  `xdg-open` gets a finite timeout; ParseFloat/Double reject leading space/`+`/hex-
  float; the control client shares one deadline across send + read;
  `ExecuteControlCommand` reports panel feedback only if the command changed it.
- **File index (Batch I).** The initial-scan and native-event workers no longer race to
  lose a file changed during the scan window — `SetCallback` buffers incrementals until
  the initial (wholesale-replace) batch lands, then replays them in order. Deleted the
  dead `FileIndex::files(ProjectFileScanMode)`.
- **Already fixed by a later pass (closed, no code):** the plugin contribution per-kind
  cap; the Close-onto-deferred-neighbor cursor/scroll restore; the commit-summary
  per-keystroke throttle; the compare Ctrl-mask ascii dispatch.

## Not changed (recorded in `known-tech-debt.md`)

Verified non-defects (combining-mark-after-wide dead code, `BlendColors` clamp,
porcelain-v2 rename record, test-only plugin sync-query overloads, unreachable
disabled-runtime reset, 1×1 compare pairing, primary DECSTBM origin-mode) stay
won't-do. Four Windows/macOS items stay platform-deferred. A small set of
lower-value / larger / latent items (full git off-thread dispatch, a few LOW
render/plugin/persistence/undo items) stay deliberately deferred — see the current
`known-tech-debt.md`.

## Follow-up (same day) — compare staging fidelity

The two most user-facing deferred items from the sweep were then closed in a
focused follow-up commit, each with regression coverage:

- **Compare: CRLF working-tree files failed generated-patch context matching.**
  `BuildCompareModelProfiled` now classifies each side's line terminator
  (`CompareModel::left_uses_crlf` / `right_uses_crlf`, from the first newline), and
  `PatchGenerator` re-emits the trailing `\r` on every body line whose originating
  side is CRLF (context/`-` from the left/pre-image side, `+` from the right/post-
  image side; suppressed on a no-newline final line). git stores the `\r` as blob
  content, so staging/discarding a hunk of a CRLF file now byte-matches under `git
  apply` and the staged blob preserves the file's endings exactly. Tests:
  `PatchApply/CrlfContextLines` (strengthened), `PatchApply/StageCrlfFileApplies`
  (end-to-end real-repo stage of a CRLF file).
- **Compare: ignore-whitespace narrowed staging.** `PatchApplyService` now refuses
  every apply operation (stage/unstage/discard, hunk and line scope) while
  `build_options.ignore_whitespace` is set, via `RejectIgnoreWhitespaceApply` at the
  shared `Request*` choke point that both the keyboard and menu/coordinator paths
  funnel through. The refusal surfaces a specific, actionable message
  ("Turn off Ignore Whitespace…") instead of silently misstaging a diff whose
  whitespace-only differences are folded into Unchanged rows. Test:
  `PatchApply/RejectsIgnoreWhitespace`.

## Follow-up (2026-07-13) — remaining deferred backlog closeout

The rest of the still-open deferred items were investigated (one agent per item)
and either fixed with regression coverage or resolved as won't-do. Only the git
off-thread dispatch (larger change, no invariant violated) and the deliberate
no-parent-dir-fsync tradeoff remain deferred; see `known-tech-debt.md`.

Fixed, each with a regression test:
- **Git background-task double-count** — deleted `GitRepositoryService`'s manual
  counting (`WakeCallbacks::increment/decrement_background_task_count*`); the
  `ProjectBackgroundExecutor` queue hooks are now the single owner of the global
  counter. Removed the dead `app/BackgroundTaskCounter.h` include. Tests:
  `GitRepositoryService/AsyncRefreshCountsGlobalCounterOnce`,
  `.../SyncRefreshLeavesGlobalCounterUntouched`.
- **Merge preview overlay** — choice lines cached on the tab
  (`EnsureMergePreviewLines`, keyed by conflict/choice/revision) so hover no longer
  reallocates per frame, and the preview now renders through the tab-aware
  `BuildVisibleLine` + `BuildDecoratedRow(layout)` path (column-aligned with the
  tab-expanded result viewport) instead of a codepoint slice. Tests:
  `Merge/EnsureMergePreviewLinesCachesByKey`, `Merge/PreviewLayoutIsTabAware`.
- **Plugin dead MCP scaffolding** — deleted the never-dispatched
  `ContributedMcpTool`/`McpToolRuntime`/`McpToolRegistration`/`ParseMcpToolRegistration`/
  `RegisterMcpTool`/`InvokeMcpTool` stubs. New architecture lint
  `CheckNoUnwiredMcpScaffolding` guards against re-introduction.
- **LSP tracked-request indicator** — `request_in_flight` bool → `request_in_flight_count`
  int (represents concurrent interactive requests), and the UI backstop is now derived
  from the transport deadline (`kLspRequestTimeout` + margin) with a `static_assert`
  pinning the two together.
- **DAP init-race** — pre-`initialize`-response messages are buffered and replayed only
  after capabilities are stored, so a non-conformant adapter emitting `initialized`
  first no longer reads default capabilities. Test:
  `WorkspaceDapClient/EmitsInitializedBeforeResponseSeesCapabilities`.
- **Editor: DirectoryTree key pruning + nested undo groups** — `Refresh` prunes
  expanded/collapsed keys for deleted directories (`PruneDeletedDirectoryKeys`), and
  `FlushActiveUndoGroup` skips the push while an enclosing group is still active (no
  double-fold). Tests: `DirectoryTree/PrunesDeletedDirectoryKeysOnRefresh`,
  `TextViewport/NestedUndoGroupsDoNotDoubleCount`.
- **Platform: `.trashinfo` O_EXCL reservation** — the Linux trash path reserves the
  metadata name atomically with `O_EXCL` before writing/moving, closing the
  concurrent-trash overwrite race. Test:
  `Project/TrashReservationDoesNotOverwriteExistingMetadata`.
- **Search: `FileFinder::Refresh` allocation** — ranks via lightweight index refs and
  deep-copies only the capped (`kMaxResults=512`) visible prefix; the full match set is
  still tracked (uncapped) for forward-typing narrowing. Tests:
  `FileFinder/CapsBroadResultCount`, `FileFinder/NarrowsToEntryBeyondDisplayCap`.

Resolved as won't-do (see `known-tech-debt.md` for the reasoning): the render glyph-atlas
overhang blit (unimplementable via SDL surface blits, net-negative on the hot path for
an invisible artifact), the plugin `process.run` OOM-longjmp / provider-query guard (both
verified non-defects), and the persistence divider-fraction "clamp" (already neutralized
by `PrimitiveReader::ReadF32` replacing non-finite floats with 0.0 plus the render clamp).
