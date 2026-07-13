# MicroIDE Known Tech Debt

Reviewed on 2026-07-13.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
Keep current priorities summarized first; deep-audit backlogs may be longer when they are intended
as intake for later agents. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

These are the tech-debt items that are still open and present in the tree, plus the
verified won't-do decisions worth recording so they are not re-filed. Fixed items do
not live here — the 2026-07-12 deferred-backlog sweep (which cleared the pass 5–24
backlog) is archived at
`guidelines/tech-debt/archive/2026-07-12-deferred-backlog-sweep.md`, and per-item
detail lives in the `Deferred backlog sweep — Batch A…I` commits.

### Still open (deferred, lower value / larger / latent)

- **Git: sidebar stage/unstage/discard and commit `RefreshDerivedState` still run git
  synchronously on the shell thread.** Only the contained wins landed (rename-probe
  gate, summary cache). Investigated 2026-07-13: no hard invariant is violated (the
  code uses `project::Git*Path` free functions, so the workspace-subprocess/direct-
  `GitRepository` lints do not fire), and the remaining sites are fast one-shot,
  user-initiated local-index writes (single-digit ms on normal repos) off the hot
  render/typing paths. The full off-thread dispatch needs per-op in-flight guards,
  a completion mailbox, generation guards against stale completions landing on
  async-reordered entries, and reconcile/refresh reordering across 4–5 call sites —
  high regression surface (discard data-loss, editor-tab reconcile ordering) for
  marginal speed benefit. Kept deferred; if pursued, reuse `CommitWorkflowService`'s
  completion-mailbox + `operation_generation` pattern.
- **Persistence: no parent-directory fsync after the atomic rename.** A deliberate
  speed/durability tradeoff: the atomic rename already prevents a torn read; only a
  crash inside the fs flush window can lose the newest session write. Left as-is.

#### 2026-07-13 multi-pass bug-campaign residue (deferred, latent / larger / behavior-risk)

- **`RuntimeSyntaxRegistry::FindFirstRegex` skip-mask full-tail rescan.** For a region
  carrying a `skip` regex, every cursor step copies the whole remaining tail into
  `masked_buf` and re-runs `FindAllRegex(skip)` over it — an O(n²)→O(n) opportunity.
  Deferred: making it incremental reworks the recursive masked-scan contract and risks
  changing match results at zero-length/UTF-8 boundaries. Only hit by regions that
  declare a `skip`.
- **`RuntimeSyntaxRegistry` rules compile without `PCRE2_MULTILINE` yet match over
  mid-line substrings**, so a `^`-anchored rule can match at a segment boundary rather
  than true line start (med-confidence correctness). A correct fix needs `PCRE2_NOTBOL`
  threading plus generated-table verification.
- **`DiagnosticsRender::DiagnosticUnderlineRect` runs `VisualColumnForTextColumn` twice
  (start+end) per diagnostic per line per frame** → O(diagnostics × column). Needs a
  shared per-line visual-column cache; deferred as a non-trivial visual-column-semantics
  change.
- **Compare `AlignHunkLines` multiplicative DP cap.** The hunk-alignment cap
  (`kMaxHunkAlignmentMatrixCells`, 65 536) and per-pair token-LCS cap
  (`kMaxIntralineLcsMatrixCells`, 65 536) multiply: a pathological ~256×256 modified-line
  hunk can reach ~4.3e9 token comparisons synchronously on the UI thread. Memoization
  gives nothing (every cell is required); any tighter cap flips pinned behavior. Bounded
  perf cliff, not a bug.
- **`SurfaceTextureCache` transient `SDL_CreateTexture` failure leaves the
  `in_flight_or_failed_` marker set**, permanently suppressing a valid decoded image
  until `Clear()` — inconsistent with the sibling `renderer == nullptr` path that erases
  the marker. Dropping the marker changes retry policy (risk: re-decode-every-frame on a
  genuinely un-creatable texture), so deferred as a retry-semantics judgment call.
- **Multi-caret overlapping *selections* are not merged** (`TextViewportMultiCaret`
  dedups only on equal caret position). The reverse-walk apply would double-edit if two
  carets held overlapping selections. Not reachable via normal UI (Ctrl+D / box-select
  produce non-overlapping/empty selections); a defensive overlap-merge carries corruption
  risk if wrong.
- **`ShapingActions::ToggleBlockComment` is one-way** — it always wraps in `open`…`close`
  and never detects/strips an existing block comment, so calling twice nests
  `/* /* x */ */`. Correct un-toggle needs surrounding-marker detection + partial-selection
  handling; flagged for a spec'd change, not a blind fix.
- **`CommitWorkflowService::DispatchCommit` captures `&state` (a `CommitWorkflowState`
  inside `current_project_state`) across the background executor + mailbox.** A project
  switch that moves/destroys that state while a commit is in flight dangles the reference;
  `operation_generation_` guards logical correctness but not lifetime. Pre-existing
  threading design; a correct fix needs a lifetime redesign beyond a local edit.
- **`WorkspaceShellSettingsOverlay::StepSetting` uses an ad-hoc truthiness test for
  plugin-contributed Bool settings** (`== "true"/"1"/"on"`) instead of `SettingFlagEnabled`,
  so a non-canonical truthy default like `"yes"` no-ops on first toggle. Cosmetic.
- **Compare/Merge mouse row hit-test selects row 0 for clicks in the ~6px band directly
  above `rows_y`** (`WorkspaceCompareMouseCoordinator` / `WorkspaceMergeMouseCoordinator`;
  truncation toward zero yields `0`, not a rejection). Minor, identical in both paths.
- **Editor wheel scrolls the active viewport regardless of which split pane the pointer is
  over** (`WorkspaceEditorMouseCoordinator::HandleWheel` uses the whole `editor_surface`).
  Looks like intended "active viewport" behavior; matching the pane-under-cursor is a
  feature change.
- **Latent, currently-unreachable index guards:** `CompareTabReview::CompareTabSelectedModelRowRef`
  computes `model.rows.size() - 1` with no empty guard (caller guards), and
  `ComparePresentationModel::CompareInline{Left,Right}Spans` index `model.rows[...]`
  unchecked (all callers pass in-range indices). Reference-returning contracts make a safe
  fallback awkward; left as-is.

#### 2026-07-13 deep subsystem audit backlog (intake for later bug-fix agents)

This section is intentionally broad and actionable. It was produced by reading across the tree, not
by fixing anything. Most items need a focused reproducer before implementation; the suggested tests
are the preferred first step so lower-cost follow-up agents can confirm the failure and lock in the
behavioral contract before changing code.

##### Startup, app plumbing, and file operations

- **Startup silently accepts multiple positional project paths and opens the last one.**
  `ParseAppStartupOptions` overwrites the saved positional argument when another positional token is
  encountered. Repro: run `microide /tmp/repo-a /tmp/repo-b`; `/tmp/repo-b` wins with no warning even
  though the user likely made a command-line mistake. Fix direction: reject a second positional path
  with the usage text, or explicitly document/support a project-list mode. Add an
  `AppStartupOptions` regression that passes two roots and expects failure.
- **Windows terminal launch does not quote or pass `lpApplicationName` for custom shells.**
  In `src/platform/TerminalBackend.cpp`, the Windows PTY backend builds `command_line = shell` and
  passes it to `CreateProcessW(nullptr, mutable_command.data(), ...)`. A custom shell under a path
  with spaces such as `C:\Program Files\PowerShell\7\pwsh.exe` can be parsed as `C:\Program.exe` or
  fail to launch. Fix direction: pass `lpApplicationName` as the shell path and keep the command line
  mutable, or share a Windows quoting helper with the subprocess implementation. Needs a Windows-only
  terminal backend test seam or a narrowly isolated command-line builder test.
- **Windows terminal backend has unsynchronized reader/stop state.** The same Windows-only class uses
  plain `bool running_`, plain `bool stop_requested_`, and `process_info_.hProcess` from both
  `Stop()` and `ReaderMain()`. Closing a terminal while the child exits can race the reader's
  `WaitForSingleObject(process_info_.hProcess, INFINITE)` against `CleanupProcessHandles()`, and
  `running()` can read a data-raced bool. Fix direction: mirror the POSIX branch's synchronization
  discipline with atomics or a mutex-protected handle snapshot; join before close; make `running()`
  race-free. Validate on Windows with repeated open/close and child-exits-immediately scenarios.
- **`FsOps::MovePath` cross-device fallback can leave a duplicate destination after remove failure.**
  The fallback path does `CopyPath(source, target)` and then `RemovePath(source)`. If the copy
  succeeds but removal fails due to permissions, sharing violations, or a read-only parent, the
  function returns failure while the destination copy remains. For trash this can leave hidden
  trash contents for a file the user still sees in place; for rename/move it can poison retries with
  an already-existing target. Fix direction: return a richer partial-success result or roll back the
  destination on remove failure. Add a fixture that forces source removal failure after a successful
  copy, probably by injecting file ops rather than depending on platform permissions.
- **File-index git metadata filtering is case-sensitive.** `IsGitMetadataRelativePath` only excludes
  a first component exactly equal to `.git`. On Windows and default macOS filesystems, `.GIT`,
  `.Git`, or watcher casing drift refer to the same metadata directory and can leak repository
  internals into the file finder or search index. Fix direction: apply platform case-folding to the
  first path component, using the existing host-platform override for tests. Add a test that indexes
  `.GIT/config` under a Windows override and expects exclusion.
- **Git path arguments are not forced literal.** Many `GitRepository` / git service commands pass
  user-controlled relative paths after `--`, but Git pathspec magic still applies after `--`.
  Files named with pathspec prefixes such as `:(top)`, `:(glob)`, or other magic can stage, discard,
  blame, diff, or history-query the wrong path. Fix direction: use `--literal-pathspecs` where
  supported or prefix user paths with `:(literal)`. Add repository fixtures with filenames beginning
  `:(literal-test)` and cover stage, discard, blame, compare, and history.

##### Persistence and durable file I/O

- **Atomic save through a relative symlink whose target is missing can replace the symlink instead of
  writing the intended target.** `WriteTextFileAtomically` resolves symlinks through
  `weakly_canonical`; when a symlink exists but its relative target does not, canonicalization can
  fail and the fallback path is the symlink itself. That turns the symlink into a regular file on
  save rather than creating the target. Existing save integrity tests cover a symlink with an
  existing target, not a missing target. Fix direction: when `read_symlink` succeeds, resolve a
  relative link against the link's parent lexically even if the target does not exist; only fall back
  to the link path when the path is not a symlink or the symlink read fails. Add a
  `SaveDataIntegrity` case for `link -> subdir/missing.txt`.
- **Persistence backup fallback can resurrect stale state after a primary read failure.**
  `PersistedRecordReader::ReadFile` blocks backup fallback for unsupported versions, but ordinary
  `ReadFailed` errors still fall through to the backup read. A transient permission error, short
  read, or storage fault on the primary can make the app load an older backup and later overwrite
  the primary with stale state. Fix direction: classify primary read failures into "primary absent,
  backup allowed" vs "primary present but unreadable/corrupt, backup must not be saved over without
  user-visible recovery". Add tests for present primary with denied read or injected read failure and
  valid stale backup.
- **Atomic-save metadata application failure appears non-fatal.** The save path preserves mode/owner
  metadata before the final rename, but callers do not appear to surface a partial permission-copy
  failure. On privileged/unprivileged boundary cases a save can succeed with changed permissions or
  ownership, which matters for executable scripts, shared config, and files in group-writable repos.
  Fix direction: audit `DurableFile` metadata helpers and decide which failures are fatal; at minimum
  report a warning when the content write succeeded but metadata preservation failed. Add an injected
  metadata-failure test if direct `chown`/`chmod` reproduction is platform-fragile.
- **Session/config recovery has no "do not overwrite recovered-from-backup" marker.** Even when
  backup fallback is intentionally allowed, the loaded record is indistinguishable from a clean
  primary load for later persistence. A stale but syntactically valid backup can become the new
  primary without a recovery banner. Fix direction: have `PersistenceService` carry a
  `loaded_from_backup` bit and suppress automatic save or write a new primary only after a fresh
  state mutation that is not derived from the stale backup. This pairs with the previous item.

##### Editor, text model, snippets, and search semantics

- **Multi-caret deduplication ignores selection ranges.** `ApplyMultiCaretEdit` deduplicates sites by
  caret position, not by normalized selection range. Two carets at the same caret position with
  different anchors can discard one replacement nondeterministically, and primary-caret recovery can
  point at the wrong surviving site. Normal UI paths usually avoid this, but test-access, plugins,
  and future multi-cursor commands can construct it. Fix direction: normalize and merge by full
  selection range, preserve the primary deterministically, and reject overlapping non-identical
  selections before edit application. Extend `EditorMultiCaretTests`.
- **Snippet commit restores pre-snippet secondary carets at stale positions.** Snippet expansion saves
  secondary carets, clears them during snippet editing, and restores them on commit. If the snippet
  replacement changed the buffer before those saved carets, the restored carets point at old offsets.
  Repro: create secondary carets, expand a snippet in the primary selection, tab through fields, and
  commit; old secondaries reappear without remapping. Fix direction: either discard secondary carets
  for snippet sessions or remap them through the initial replacement and every snippet field edit.
  Add a snippet test with pre-existing secondaries before the expansion point.
- **Newline insertion while a snippet is active can leave stale snippet ranges.** `SnippetTryInsertText`
  returns false for `\n`/`\r` so the normal editor path can handle newline insertion, but it does not
  explicitly commit or cancel the active snippet first. If the fallback edit succeeds, the snippet
  session can retain ranges computed for the pre-newline document. Fix direction: commit or cancel
  the snippet before returning false for multi-line input; test Enter inside an active placeholder.
- **Snippet parser likely mishandles escaped placeholder delimiters.** The snippet parser walks until
  the next raw `}` or `|` delimiter and does not appear to honor escaped braces, pipes, or commas in
  default/choice text. VS Code-style snippets containing `\}`, `\,`, or `\|` can produce truncated
  placeholders or wrong choices. Fix direction: define the supported snippet grammar explicitly, then
  parse escapes rather than treating delimiters as unconditional terminators. Add parser-only tests
  for escaped delimiter fixtures.
- **Case-insensitive editor replace and project search are ASCII-only despite UTF-8 editing.**
  `TextViewportEditEngine` and `ProjectSearchService` lower through ASCII helpers. Searching or
  replacing case-insensitively for `é`/`É`, `ä`/`Ä`, Greek, Cyrillic, or Turkish dotted/dotless I
  will miss expected matches. Fix direction: either implement Unicode case folding for literal
  case-insensitive paths or explicitly constrain the UI label to ASCII case-insensitive matching.
  Add editor replace and project-search tests with simple non-ASCII pairs.
- **Closed-file LSP workspace edits silently clamp out-of-range positions and then save.**
  `ApplyLspEditsToClosedFilesOnDisk` maps LSP positions by clamping lines and columns to the scratch
  viewport. A server bug that sends a range beyond EOF mutates the last line instead of rejecting the
  edit. Open-buffer edits likely share similar forgiving behavior. Fix direction: validate LSP ranges
  before applying them; reject the whole file's edit group, or at least the bad edit, with telemetry
  or a status message. Add an LSP workspace-edit test where the server sends line 999 for a one-line
  closed file and assert no save occurs.
- **LSP and plugin workspace edit appliers do not appear to reject overlapping edits up front.**
  Both paths sort edits highest-position-first so ordinary non-overlapping edits apply correctly, but
  overlapping ranges can still double-apply in an order-dependent way. Language servers are supposed
  to avoid overlap; plugins are less controlled. Fix direction: normalize each target file's edit
  ranges, reject or merge overlaps deterministically, and make the failure visible to the code action
  caller. Add tests for two edits covering intersecting byte ranges in the same line.

##### Workspace orchestration, LSP, code actions, and plugin-facing edits

- **Server-initiated `WorkspaceEdit` ignores `documentChanges` and resource operations.**
  `ApplyServerWorkspaceEdit` flattens only URI-keyed text `changes`; LSP edits that include
  `documentChanges`, `TextDocumentEdit`, `CreateFile`, `RenameFile`, or `DeleteFile` are partially
  or wholly dropped. Rename/code-action providers commonly use those shapes. Fix direction: extend
  the LSP parser and applier to support versioned text-document edits and resource operations through
  the existing `FileOperationService`, with project-root containment checks. Add fixtures for a
  server rename that both edits imports and renames a file.
- **Server workspace edits return false when every edit is filtered, without surfacing why.**
  `ApplyServerWorkspaceEdit` skips non-file URIs and paths outside the project/open buffers; if that
  leaves no edits it returns false. From the user's perspective a code action may appear to do
  nothing. Fix direction: return an enum or diagnostic reason (`unsupported URI`, `outside project`,
  `unsupported resource operation`, `invalid range`) and show a terse status message. Tests should
  assert the specific failure reason, not just `false`.
- **Plugin runtime path resolution for editor edits is lexical, not containment-enforced at read time.**
  `LuaEditorApplyEdits` resolves an optional path with `ResolveRuntimePath`, then relies on the host
  callback to decide whether the file is editable. That is correct only if every callback target
  re-checks project containment and open-buffer identity. Fix direction: audit
  `ApplyPluginWorkspaceEdit` and related callbacks, then either enforce containment in
  `ReadOptionalPathField` using `ContainPath` or document why open-buffer-only dispatch is sufficient.
  Add a plugin test that tries `editor.apply_edits({ path = "../outside.txt", ... })`.
- **Plugin `editor.apply_edits` silently truncates edit arrays at 100,000 entries.** The cap protects
  memory, but a plugin applying 100,001 edits receives a normal host result for the truncated edit
  set. That can corrupt generated edits, formatters, or refactors. Fix direction: if raw length
  exceeds the cap, fail the request with an explicit error instead of truncating. Add a Lua plugin
  regression that passes `kMaxApplyEdits + 1` and expects failure/no partial edit.
- **Plugin numeric line/column reads accept fractional and very large values by truncation.**
  `ReadIndexField` reads Lua numbers as `double` and casts any value `>= 1.0` to `size_t`. Values
  like `1.9`, `inf`, or beyond exact integer precision can land on surprising lines or overflow
  differently across platforms. Fix direction: require integer-valued finite numbers in plugin API
  parsers, reject non-finite/fractional inputs, and add API tests for `1.5`, `math.huge`, and huge
  integer strings.
- **Plugin data-directory discovery trusts lexical roots.** `DataDirectories` builds candidates from
  plugin roots and subdirectories with `lexically_normal`. If a plugin root is a symlink that changes
  after discovery, data-directory identity may drift from the root containment policy used by runtime
  filesystem operations. Fix direction: either canonicalize plugin roots once at discovery and store
  that identity, or route data-directory derivation through the same fresh canonical containment
  helper as `ContainPath`. Add a symlink-root test that swaps the symlink target between discovery
  and data-dir access.

##### Git, compare, merge, and review state

- **Compare picker history and outgoing-base pickers run expensive git queries on the UI thread.**
  `CompareInteractionCoordinator::OpenPickerForPath` calls `CollectGitFileHistory`, and
  `OpenOutgoingBasePicker` calls `CollectGitBranches` plus `CollectGitRecentCommits`, synchronously
  while opening overlays. Large repos or slow storage can freeze the shell before the picker appears.
  Fix direction: dispatch through `ProjectBackgroundExecutor` with generation guards, show a loading
  picker state, and drop stale completions when the overlay closes or the project changes. Add a fake
  git service test that blocks until the overlay has rendered.
- **Copying a compare patch can clear the clipboard on failure.** `CopyCompareHunkPatch` and
  `CopyCompareFilePatch` write `patch.value_or("")` to the clipboard. If patch generation fails, the
  user's previous clipboard is replaced with an empty string and no visible failure. Fix direction:
  only write on success and surface a status message on failure. Add a compare test that injects patch
  generation failure and asserts the clipboard is unchanged.
- **Branch review "delete/unreview missing target" creates state.** `BranchReviewStateService`
  methods such as `MarkFileUnreviewed`, `MarkHunkUnreviewed`, and `DeleteNote` call
  `FindOrCreateTarget`. Unreviewing or deleting a note for an unknown target can create an empty
  target, bump revision, and persist noise. Fix direction: use a mutable find-only helper for
  delete/unmark paths. Add tests that call each operation on a missing target and expect no revision
  change and no new target.
- **Branch review pruning ignores recent updates inside a target.** Per-target file/hunk/note vectors
  are pruned by insertion order, while review and note updates refresh timestamps without moving the
  touched entry to the back or sorting by timestamp. A recently re-reviewed file can still be pruned
  because it was inserted early. Fix direction: prune by `reviewed_at_unix_ms` /
  `updated_at_unix_ms`, or maintain LRU ordering on every touch. Add tests that fill past the cap,
  touch the oldest entry, and ensure it survives pruning.
- **Merge "mark resolved" for delete conflicts removes the working file before the full operation is
  proven.** In the delete branch, the file is removed and the buffer is marked clean before later git
  validation/staging can fail. A stale index, permission problem, or git error can leave the user's
  merge file gone even though the resolution did not complete. Fix direction: validate all preconditions
  before deletion, stage as a transaction, and restore the buffer/file or keep it dirty if staging
  fails. Add a merge fixture that injects stage failure after deletion and asserts the file content is
  preserved or recovered.
- **Merge result generation may normalize final newline / line-ending intent across conflict choices.**
  The merge model works in split lines and later rejoins text. If current, incoming, and base differ
  in trailing newline or line-ending convention, choosing one side may not preserve that side's exact
  file ending. Fix direction: write tests for conflicts where one side has no final newline and/or CRLF
  inside the conflict, then either preserve per-choice endings or document the normalizing behavior.
- **Compare profiling counters can underflow.** `BuildCompareModelProfiled` computes residual
  row-assembly time by subtracting sub-step durations from total duration. If clock jitter or nested
  measurement overhead makes sub-steps exceed total, an unsigned residual can become an enormous
  number and pollute perf diagnostics. Fix direction: saturating subtract for derived timing fields.
  Add a tiny helper test rather than trying to force clock behavior.

##### Project search, indexing, blame, and repository state

- **Project search per-worker memory cap can multiply into multi-gigabyte transient use.**
  `ReadFileForTextSearch` permits large files and the service can run multiple workers. The per-file
  cap is bounded, but N workers can each hold a large file plus match vectors at the same time. On a
  repo with many huge generated files, search can violate the low-footprint product goal. Fix
  direction: lower the per-worker read cap, stream line scanning, or add a global in-flight byte
  budget. Add a stress test with injected readers so it does not allocate real gigabytes.
- **Project search cancellation still finishes a full current-file scan before stopping.**
  The service checks cancellation between files and at some result boundaries, but a pathological
  regex or very large literal target can occupy a worker until the current file completes. Fix
  direction: thread cancellation through line iteration and regex search helpers. Add a cancellation
  test with an injected slow reader/searcher and assert prompt stop latency.
- **Git blame parser can allocate too much from hostile or corrupted porcelain output.** The parser
  clamps individual counts, but a stream containing many attribution headers with large counts can
  still populate a very large line map unrelated to the requested visible window. Real git should not
  emit this, but fuzz targets can and a compromised helper should not exhaust memory. Fix direction:
  cap total parsed lines per response to the requested window plus a small margin. Extend blame parser
  fuzz/regression coverage.
- **Git blame visible-window arithmetic has overflow edges.** Snapshot range calculations add
  `visible_start_line + visible_line_count - 1` and similar result bounds without clear saturating
  guards. Normal UI sizes are small, but public/test seams can pass huge values and make the service
  believe no request is needed or mark stale data as covering the viewport. Fix direction: add
  saturating helpers for one-based line interval math and tests using near-`size_t::max` inputs.
- **Repository state caps can hide important changes silently.** Git status and search result caps are
  necessary, but some paths appear to truncate without a first-class "truncated" state in every UI
  consumer. A conflict or high-priority result after the cap can be invisible. Fix direction: audit
  status, search, and file-index caps for a propagated `truncated` flag and visible banner; tests
  should construct cap+1 entries and verify the UI reports incomplete data.

##### Terminal and ANSI behavior

- **Windows terminal `Write()` can block the shell thread indefinitely.** The PTY backend writes the
  full byte span synchronously to the ConPTY input pipe. If a child process stops reading stdin or the
  pipe blocks during a large paste, the caller thread can hang. The POSIX side usually routes through
  non-blocking PTY behavior; the Windows path needs the same bounded-write strategy. Fix direction:
  buffer writes on the terminal worker thread with a cap and drop/abort policy, or use overlapped I/O.
  Validate by pasting a large payload into a Windows program that does not read stdin.
- **OSC 52 clipboard support is capped only by the global 8 KiB escape buffer, not by decoded payload
  policy.** The cap avoids unbounded growth, but it also means a legitimate OSC 52 payload slightly
  over the limit is silently discarded the same way as hostile data. Conversely, anything under the
  cap can update the host clipboard without a policy gate. Fix direction: decide whether terminal
  clipboard writes require an allow/deny setting, and expose a clear status for rejected oversized
  OSC 52 payloads. Add terminal parser tests for just-under and just-over limit payloads.
- **OSC 7 working-directory reports are accepted without project containment policy.** The terminal
  session records the shell's advertised cwd. If later workspace features use this cwd for file
  operations, task defaults, or links, a process can point it outside the project. Fix direction:
  keep OSC 7 as display-only unless a caller explicitly containment-checks it before filesystem use.
  Add a regression around any feature that consumes `current_working_directory()`.
- **Terminal parser recovery after overlong CSI/OSC drops the whole sequence without an event.** That
  is good for speed and memory, but difficult to diagnose when an interactive program emits a long
  but valid sequence. Fix direction: increment a cheap dropped-sequence counter on overflow and expose
  it in debug/status surfaces so terminal compatibility bugs are observable without logging hot paths.

##### Rendering, UI state, and view models

- **Texture cache failure marker policy is inconsistent across failure modes.** The existing residue
  notes that `SDL_CreateTexture` failure leaves the in-flight/failed marker set. The broader issue is
  that decode failure, null renderer, create-texture failure, and later renderer reset do not share a
  documented retry contract. Fix direction: define a single state machine (`not requested`,
  `in flight`, `failed retryable`, `failed permanent`, `ready`) and make `Clear()` / renderer changes
  transition explicitly. Add tests with fake renderer failures if possible.
- **Plugin surface texture cache keys depend on caller-provided/content hashes without collision
  verification.** A hash collision or stale hash can reuse a texture for different bytes. The
  probability is low for a strong hash, but plugin-provided data is an extension boundary. Fix
  direction: store dimensions plus a cheap secondary fingerprint, or treat plugin-raster hashes as
  cache hints and verify byte length/dimensions before reuse. Add a unit test with forced same-hash
  different raster payloads through the cache seam.
- **Row-level render hot paths rely on discipline rather than mechanical enforcement for new string
  materialization outside the currently linted set.** The hard invariant covers named render TUs, but
  adjacent UI helpers can still assemble labels per frame before data reaches the builder. Fix
  direction: extend architectural lint to catch `std::string` construction / formatting helpers in
  all render-adjacent TUs that execute every frame, or annotate allowed cold paths. Add a fixture file
  to the lint test so the rule does not silently shrink.
- **Mouse hit-test behavior differs across editor, compare, merge, and sidebar surfaces.** The residue
  already records compare/merge row-0 band behavior and editor wheel-active-pane behavior; a broader
  audit should consolidate pointer-to-surface targeting into one tested helper. Fix direction: create
  shared hit-test utilities that return an explicit `std::optional<RowHit>` / `SurfaceHit`, then route
  compare, merge, editor, and sidebar through them. Add boundary tests for one pixel above, on, and
  below every row band.

##### Settings, configuration, and user-visible feedback

- **Boolean setting parsing is split across registries and overlays.** The settings overlay truthiness
  residue is one symptom; any plugin/default-setting surface that compares strings directly can drift
  from `SettingFlagEnabled`. Fix direction: make a single typed bool parse/format helper the only
  route for bool settings, and lint for direct `"true"` / `"1"` setting comparisons outside tests.
  Add settings registry tests for `yes`, `on`, `1`, `false`, and invalid values.
- **Several failure paths return `false` with no user-facing status.** Examples found during the audit:
  filtered LSP workspace edits, failed compare patch generation, truncated plugin edit arrays, and
  invalid plugin edit coordinates. Silent no-ops are cheap but make correctness bugs look like user
  error. Fix direction: introduce a small `OperationResult` / status-message convention for
  user-triggered actions that can fail after validation, and wire it through the overlay/status bar
  rather than logs. Start with compare copy and LSP code actions because they are direct user actions.
- **Caps and truncation limits are inconsistent in how they fail.** Search, status, plugin code
  actions, plugin apply-edits, terminal escape parsing, and branch review state all use caps, but some
  truncate, some drop, and some silently fail. Fix direction: standardize cap behavior by category:
  security caps should fail closed with an error; display caps should preserve data and mark UI
  truncated; performance caps should expose telemetry. Add a doc/spec note once the policy is chosen.

##### Test and tooling gaps exposed by this audit

- **Windows/macOS-only defects accumulate because platform code lacks compile-and-shim tests on Linux.**
  The known platform-only list now includes Windows async subprocess races, Windows terminal races,
  Windows timeout handling, Windows ignore matching, and macOS FSEvents races/filter bypasses. Fix
  direction: extract pure builders/state machines from platform TUs so Linux tests can cover command
  quoting, state transitions, and ignore decisions; keep OS API calls behind tiny adapters.
- **No cheap fake git/executor seam for UI-thread blocking regressions.** Compare picker and sidebar
  git operations are hard to test for "overlay appears before git returns" without sleeping real git.
  Fix direction: define a narrow git-history/branch provider interface or injectable executor for
  compare interactions. Tests can then block the provider and assert render state remains responsive.
- **No single fuzz target covers plugin/workspace edit range normalization.** Persistence, regex, and
  blame have fuzz-oriented guidance, but LSP/plugin edit appliers accept untrusted-ish ranges and text
  from language servers and plugins. Fix direction: add a fuzz target that generates edit lists,
  applies them to small UTF-8 documents through the shared normalization helper, and asserts no crash,
  no invalid UTF-8 split when possible, and deterministic results for non-overlapping edits.
- **Known-tech-debt entries frequently outlive their original reproduction context.** Several old
  entries are good summaries but omit exact test seams. Future bug-fix agents should, when touching an
  item, first add a failing test named after the debt entry and then either delete the entry on fix or
  update it with the new blocker. This is a process debt item, not a source-code change.

#### 2026-07-13 deep subsystem audit backlog — additional tranche

This is a second pass over less-central code paths and second-order failure modes. Treat these as
bug-investigation tickets: add the reproducer first, then fix or explicitly demote the item if the
test proves it is unreachable.

##### Persistence, JSON, and protocol framing

- **Persisted-record writer leaves a stale backup after every successful write.** The writer renames
  the old primary to `*.bak`, renames the new temp into place, then returns success without deleting
  or refreshing the backup. That is useful for recovery, but it means backup contents can be many
  sessions old and still get trusted by `ReadFile` after a future primary failure. Fix direction:
  define the backup contract explicitly: either keep a previous-good generation with metadata and
  visible recovery, or delete backup after successful primary write if backup is only for rollback.
  Add tests that write A, B, C, corrupt C, and verify whether recovery is allowed to return B or must
  fail closed.
- **Persisted-record write failure after backup rename can leave the primary restored best-effort
  only.** If `RenameReplacing(temp, path)` fails after the primary was moved to `*.bak`, the rollback
  call `RenameReplacing(backup_path, path)` is best effort and its failure is ignored. A rename
  failure on flaky storage can leave no primary and only a backup, with no surfaced "manual recovery
  required" state. Fix direction: return a richer error category for rollback failure and preserve
  enough context for the caller/status bar. Add an injected `RenameReplacing` seam so the test can
  force both rename and rollback failure deterministically.
- **Persisted-record reads allocate the full 256 MiB cap before validating header/body size.**
  `ReadAllBytes` reads the whole file into memory, then `DecodeRecordFile` checks magic, version, CRC,
  and body. A hostile 256 MiB file in the config/state path can allocate the cap on startup just to be
  rejected. Fix direction: read and validate the fixed header first, reject impossible body lengths,
  then stream or bounded-read only the declared body. Add a test with a large bogus file that proves
  startup/read memory stays below a small threshold using an injected reader.
- **JSON parsing has no aggregate array/object entry cap.** The parser bounds recursion depth, and DAP
  list parsers cap selected arrays after parsing, but `ParseJson` can still materialize a flat
  millions-entry array/object inside one message before any domain cap runs. A DAP/LSP peer can spend
  memory and CPU in the generic JSON layer even if the later parser truncates. Fix direction: add a
  parser-level node/byte budget or message-size-specific parse context, and make LSP/DAP clients pass
  protocol frame caps. Add JSON parser tests for a huge flat array and huge object.
- **JSON object duplicate keys use "last wins" with no signal.** `ParseObject` assigns
  `obj[key->AsString()] = value`, silently replacing earlier keys. That is legal-ish in many parsers,
  but DAP/LSP messages with duplicate `seq`, `command`, `success`, or `body` fields can be interpreted
  differently from peers. Fix direction: decide whether duplicate object keys should reject protocol
  JSON or be recorded as a diagnostic. Add a DAP/LSP parser test with duplicate `success` and `body`
  keys.
- **JSON serialization silently converts non-finite doubles to `null`.** This prevents invalid JSON,
  but protocol callers may believe they sent a numeric value. A malformed internal value could turn a
  DAP/LSP numeric request field into `null` without an error path. Fix direction: keep this fallback
  for generic serialization if needed, but reject non-finite numbers at protocol argument builders
  where the caller expects a number. Add unit tests for `Make*Arguments` with bad numeric inputs once
  those builders expose typed validation.
- **Protocol message-size policy is split between JSON, DAP, LSP, and control surfaces.** JSON can
  parse a payload as long as it reaches it; DAP/LSP framing has separate Content-Length handling; the
  control socket has its own line buffer cap. Fix direction: document and enforce a single inbound
  message budget per protocol, with parse budgets below transport caps. Add tests that verify
  over-budget DAP/LSP/control messages fail before JSON allocation.

##### DAP and debug workflow

- **DAP launch/configuration callbacks capture `this` without a session-generation guard.**
  `DebugSession` async callbacks check state in some places, but launch, configurationDone, pause,
  restart, evaluate, variables, scopes, setVariable, and breakpoint response callbacks all capture the
  session object directly. If a session is stopped/replaced while responses remain queued, a stale
  callback can mutate the new state or call host callbacks after termination. Fix direction: add a
  monotonically increasing session token bumped on `Start`/terminal transition and validate it in
  every DAP response callback. Add a fake DAP client test that queues a response, terminates/restarts,
  then drains the old callback.
- **DAP `RequestStop` can leave the UI active if `terminate` is sent but the adapter never answers.**
  For adapters advertising `supportsTerminateRequest`, `RequestStop` sends `terminate` and waits for
  `terminated`/process exit; it does not appear to set a timeout or transition optimistically. A hung
  adapter can leave the session in Running/Stopped with stop requested. Fix direction: start a bounded
  terminate timer, then disconnect/kill the adapter and transition to Terminated or Failed. Add a fake
  adapter test that accepts `terminate` and never emits `terminated`.
- **DAP `configurationDone` rejection records an error but still transitions to Running.** The
  callback sets `last_error_` when the response fails, then moves `Configuring`/`Initializing` to
  `Running` regardless. Some adapters reject configurationDone because launch/configuration is invalid;
  the UI can show a running session that will not run. Fix direction: classify configurationDone
  failure as terminal unless a known adapter requires tolerance. Add a DAP client test with a failed
  configurationDone response.
- **DAP resume commands optimistically clear stopped UI without checking request success.** `Continue`,
  step commands, reverse commands, and the deferred no-thread resume path send a request and set state
  to Running immediately. If the adapter rejects `continue`/`next`/`stepIn`, the execution highlight
  and variable state are cleared while the target remains stopped. Fix direction: either wait for
  response success before clearing, or restore stopped state on failure. Add fake adapter tests for
  rejected continue and rejected step.
- **DAP `Pause()` picks the first thread from a late thread-list response with no running-state guard.**
  `Pause` requests threads while Running; if the target stops, terminates, or another session action
  happens before the reply, the callback still sends `pause` to the first returned thread. Fix
  direction: capture a session/state token and require `state_ == Running` at callback time. Add a
  test where a stop event arrives before the threads response.
- **DAP thread-list refresh applies while Running after a continued event.** `RefreshThreadList`
  guards only `stop_epoch_`, not state. A thread-list response requested during a stop can land after
  a full resume without an epoch bump and repopulate the Call Stack thread selector. Fix direction:
  mirror `RequestStackTrace` and require `state_ == Stopped` before invoking `on_threads`. Add a fake
  client test with stop -> refresh threads -> continued -> threads response.
- **DAP breakpoint responses are matched by send order, not stable breakpoint identity.** The
  `setBreakpoints` response mirrors input order in the spec, but adapters that omit or reorder
  unverified breakpoints can mark the wrong local breakpoint if matching is positional only. Fix
  direction: match returned breakpoint line/source when present, fall back to order only for missing
  line data, and surface unmatched adapter breakpoints. Add a response fixture with reordered
  breakpoints.
- **DAP source paths from adapter events are trusted as filesystem paths.** Breakpoint/source events
  parse `source.path` directly into `std::filesystem::path`. If later callbacks open/focus the path
  without containment or existence checks, a debug adapter can navigate the editor outside the project.
  Fix direction: audit every DAP path consumer, then route through a "debug external path" policy:
  allow read-only focus with a banner, or require project containment. Add tests for a breakpoint event
  with `/etc/passwd` or a temp path outside the workspace.
- **DAP value-size limits are gdb-name heuristics.** `CommandLooksLikeGdb` matches any basename
  containing `gdb`, and false negatives restore the freeze risk while false positives send gdb-only
  REPL commands to another adapter. Fix direction: prefer adapter capability/type configuration or a
  user setting, then keep basename heuristic as fallback. Add tests for `arm-none-eabi-gdb`,
  `gdbserver-adapter`, and `notgdb-but-name-contains-gdb`.
- **DAP variables/scopes list truncation has no visible "more data omitted" state.** Parsers cap at
  10,000 entries; the debug tree likely renders what arrives as complete. A hostile adapter is the
  security case, but a legitimate giant array can also be silently incomplete. Fix direction: propagate
  a `truncated` bit from protocol parsing into the debug pane and request paging when adapter counts
  are available. Add parser/UI tests for 10,001 variables.

##### Plugins and extension data

- **Plugin decoration publish can allocate and sort huge valid payloads up to four times per call.**
  Each decoration kind allows 100,000 entries, so one `decorations.set` can materialize up to 400,000
  host records before store merge/render costs. The cap is protective but still high for a
  shell-owned render path. Fix direction: set per-file and per-owner budgets based on visible use
  cases, fail closed with a plugin-visible error, and expose truncation/failure in plugin diagnostics.
  Add a plugin decoration stress test at cap and cap+1.
- **Plugin decoration paths use lexical resolution, not the stronger containment helper.** Like
  `editor.apply_edits`, `decorations.set` resolves `raw_path` with `ResolveRuntimePath`. Decorations
  do not write files, but publishing decorations for outside-project paths can consume store memory
  and later follow rename/delete retargeting logic. Fix direction: use `ContainPath` for file-bound
  plugin contributions or explicitly reject decorations for paths outside the active project. Add a
  plugin test with `../outside.cpp`.
- **Plugin decoration whole-line entries can omit columns, but non-whole entries do not validate
  `start_col <= end_col`.** If a plugin sends `start_col` greater than `end_col`, downstream render or
  merge code must normalize or it can produce empty/negative-width styling assumptions. Fix direction:
  reject inverted decoration ranges at interop parsing time. Add a decoration parser test for inverted
  columns.
- **Plugin code-action/provider result truncation is silent.** Completion, code action, test, SCM,
  annotation, and auth harvesters clamp arrays with `std::min(rawlen, cap)`, then return success.
  Providers that exceed a cap get partial results with no plugin-visible error, making generated code
  actions/tests appear missing. Fix direction: fail the provider result when raw length exceeds the
  cap for correctness-critical surfaces, or return a `truncated` flag to the UI. Add provider tests
  for cap+1 on completions and tests.
- **Plugin language-provider location paths are not containment-checked.** Definition/reference
  providers call `resolve_runtime_path` and accept any non-empty resolved path. A plugin can navigate
  the user outside the project, possibly intentionally, but the product boundary is not specified.
  Fix direction: decide whether plugin language locations may be external; if yes, mark them as
  external and open read-only; if no, use `ContainPath`. Add provider tests for absolute outside paths
  and `../` paths.
- **Plugin provider string fields have no per-field length limits.** Labels, documentation, detail,
  code action titles, test names, and SCM labels can be very large while still under Lua memory budget,
  then move into UI/render view models. Fix direction: clamp or reject plugin-provided display strings
  at interop boundaries, with separate limits for labels vs documentation. Add tests with multi-MB
  completion documentation and code-action title.
- **Plugin callbacks can synchronously call back into host surfaces during lifecycle teardown.**
  Lifecycle helper code invokes Lua callbacks while host registries are being updated elsewhere in the
  plugin lifecycle. If a callback publishes decorations/diagnostics or registers commands during
  teardown, ordering assumptions can break. Fix direction: audit lifecycle callback phases and set an
  explicit "no registration/no publish" mode during teardown callbacks. Add a plugin teardown test
  that attempts to publish from `on_project_close`.

##### Project change, file finder, and indexing

- **Project change coalescer can drop per-file reload/diagnostic updates once the cap flips to tree
  rescan.** Past 1,024 pending file changes it sets `tree_rescan_requested` and ignores later file
  changes. The rescan updates the index, but open-buffer external-change banners, diagnostics clearing,
  and other per-file side effects may not run for the dropped tail. Fix direction: when collapsing to
  a tree rescan, also mark "unknown file changes happened" so downstream open buffers perform a cheap
  mtime sweep. Add a coalescer/workspace test with cap+1 modified open files.
- **Project change coalescer delete-then-create semantics lose the new absolute path for Created.**
  A Deleted followed by Modified becomes Created and updates `absolute_path`; a Deleted followed by
  Created falls through to `existing->kind = change.kind` and updates the path, which is okay, but a
  Created followed by Deleted collapses to Deleted and leaves the old absolute path. Any downstream
  consumer that uses `absolute_path` for delete cleanup may see a stale path after rename-like floods.
  Fix direction: define per-kind payload validity and clear irrelevant absolute paths on delete. Add
  unit tests for all two-event combinations.
- **File finder recent-file de-dup keys are platform-sensitive strings.** Recents are de-duplicated
  and looked up by `recent.string()` while cached entries also use path strings. On Windows/macOS,
  case-insensitive paths and separator normalization can show duplicates or fail to match a recent
  whose casing changed. Fix direction: use the same normalized path key as editor tabs/decorations for
  recent lookup. Add host-platform-override tests with `Src/Main.cpp` vs `src/main.cpp`.
- **File finder fuzzy matching is byte/ASCII-based for UTF-8 paths.** Query lowercasing and
  subsequence scoring use ASCII byte comparisons. Non-ASCII filenames are indexed but not matched in
  expected case-insensitive ways, and combining characters can rank oddly. Fix direction: either
  document ASCII matching or add Unicode case-folding/grapheme-aware scoring. Add finder tests for
  `Résumé.cpp` and query `resume` / `rés`.
- **File finder keeps the full uncapped matched-index set for narrowing.** This preserves correctness
  for later typing, but an empty/one-character query over a huge index stores every match index. The
  index itself is capped elsewhere, yet this can still be a noticeable allocation on the shell thread.
  Fix direction: keep a compressed bitset/range representation for broad matches or disable narrowing
  cache until the query length passes a threshold. Add a benchmark with the maximum index size and
  one-character query.
- **Full project scan does not propagate traversal-budget truncation to `FileIndex::truncated()`.**
  `CollectProjectFiles` stops at `kTreeTraversalEntryBudget`, but returns only the collected vector.
  `FileIndex::Refresh` then clears `truncated_` because it has no scan metadata. A too-large tree can
  look complete after a full rescan. Fix direction: return `{files, truncated}` from
  `CollectProjectFiles` and surface it through `FileIndex`. Add a scanner test with a tiny injected
  budget.
- **Project scanner hidden filtering checks only the current filename before ignore rules.** In
  `ExcludeHidden` mode, a hidden directory is skipped before loading its child ignore file, which is
  good for speed, but a negated rule inside parent `.gitignore` cannot re-include a hidden subtree for
  file finder/search. Fix direction: decide whether `ExcludeHidden` is a hard UI mode or a
  gitignore-like filter; if hard, document it; if not, let negated ignore rules override hidden
  filtering. Add scanner tests for `.hidden/file.cpp` with `!.hidden/`.
- **Windows native tree watcher cannot watch more than `MAXIMUM_WAIT_OBJECTS - 1` roots and falls back
  coarsely.** Large root lists from recursive watch preparation can exceed the wait-object limit and
  force polling. This is acceptable but not surfaced clearly to users, and polling may be expensive on
  large trees. Fix direction: shard Windows watch handles across worker threads or surface a
  "polling fallback" state. Add a Windows seam test for 65 watch roots.

##### Terminal, input, and ANSI behavior

- **Terminal UTF-8 encoder accepts invalid Unicode scalar values.** `AppendUtf8` encodes any
  `char32_t` above `0xFFFF` as a four-byte sequence, including values above `U+10FFFF`. If SDL or a
  test seam supplies an invalid codepoint, the terminal sends invalid UTF-8 to the child process. Fix
  direction: reject codepoints above `0x10FFFF` and surrogate range values before encoding. Add
  `TerminalSessionInputEncoding` tests for `0xD800` and `0x110000`.
- **Terminal legacy mouse encoding clamps rows/columns beyond 223 to the edge cell.** Without SGR
  mouse mode, coordinates beyond the legacy range are clamped to 223 rather than dropping the event.
  Applications running in large terminals can receive clicks at the wrong edge location. Fix
  direction: for legacy mode, drop events outside encodable range or force SGR-only reporting when the
  terminal size exceeds 223 columns/rows. Add mouse encoder tests for column 300 without SGR.
- **Terminal bracketed paste can synchronously write arbitrarily large payloads.** `PasteText`
  formats the entire paste into one string and calls `SendBytes`. Large clipboard content can allocate
  and block the UI/backend write path. Fix direction: chunk paste output with a bounded queue and
  cancellation/stop behavior, especially on Windows where `WriteFile` blocks. Add a paste stress test
  with a fake backend that blocks after N bytes.
- **Terminal pending query replies silently truncate at 64 KiB.** The cap prevents a reply flood from
  freezing the UI, but a program issuing many legitimate color/DA/DSR queries can receive a partial
  reply stream with no reset marker. Fix direction: when the cap is hit, drop whole replies rather
  than partial bytes and increment a debug counter. Add tests that fill the buffer exactly to the cap
  and then issue one more query.
- **Terminal OSC title updates accept unbounded semantic churn.** The escape buffer caps a single OSC,
  but a process can still send thousands of short title changes per second, causing repeated launch
  label changes and view-model churn. Fix direction: coalesce title/cwd OSC updates per frame or only
  publish when the value changes after debouncing. Add a terminal output test that feeds repeated OSC 0
  updates and asserts bounded state-change notifications.

##### Rendering, UI, and user feedback

- **Debug pane row hit-testing intentionally maps the top band to row zero, unlike other miss
  behavior.** Tests pin this for debug pane, while compare/merge top-band behavior is logged as a
  residue. The inconsistency makes pointer bugs hard to reason about across panes. Fix direction:
  choose one shared policy for all row lists and migrate tests deliberately. Add a shared row-hit-test
  helper before changing behavior.
- **Overlay scroll wheel paths use integer ticks and can skip small high-resolution wheel deltas.**
  SDL wheel events can carry fractional/precise deltas depending on device. Coordinators that cast to
  integral ticks can ignore trackpad micro-scrolls or behave differently across platforms. Fix
  direction: accumulate fractional wheel deltas per surface and consume whole rows. Add coordinator
  tests with repeated 0.25-row deltas.
- **Several UI lists clamp selection after content changes but do not preserve item identity.** File
  finder, code actions, command palette, debug threads, and similar overlays reset/clamp by index.
  Async refreshes can move the selected item under the cursor/keyboard. Fix direction: preserve
  selection by stable id/path/command where available, falling back to index only when identity is
  absent. Add overlay tests where a refresh inserts an item before the selected one.
- **Status/error messages lack severity and lifetime policy.** Many subsystems need to surface
  "operation failed", "truncated", or "fallback used"; the status bar currently has ad hoc call sites.
  Fix direction: define a small status-message service with severity, source, expiry, and replace
  rules. This is a prerequisite for several silent-failure fixes above.

### Won't-do — verified non-defects

- **Terminal: combining mark after a double-width glyph.** Dead code —
  `TerminalCell::bytes` is 4 bytes and wide(≥3)+combining(≥2) always exceeds it, so the
  mark is dropped regardless of which cell it targets. Revisit only if the cell buffer
  widens.
- **`ColorMath::BlendColors` missing output clamp.** Provably in-range for all lerp
  inputs; a branch on this hot color path violates speed-first. Left as-is.
- **Git porcelain-v2 `'2'` rename record consuming the next record.** Working as
  intended; the truncation-only residue is dropped harmlessly.
- **Plugin sync `Query*` overloads reallocation hazard.** Production uses the `*Async`
  variants (allow_registration=false); the sync overloads are test-only.
- **`ResetForDisabledRuntime`/`ShutdownForDisabledRuntime` subset-clear.** Unreachable
  — `enabled()` only goes true→false once at startup before any plugin loads.
- **Compare `AlignHunkLines` 1×1 pairing ignores similarity.** By design — a
  1-del/1-add hunk always renders as a single Modified row (pinned by
  `TestCompareManyTokenLineBoundsAlignmentDp`).
- **Terminal DECSTBM home ignores the scroll-region top under origin mode on the
  primary buffer.** Consistent with this terminal's primary CUP semantics; a
  design-consistent choice, not a live divergence.
- **Render: `AsciiGlyphAtlas::BlitInto` straight-copy erases an overhang glyph's
  spill.** The proposed OR/max-coverage merge is unimplementable via SDL surface
  blits (`SDL_SetSurfaceBlendMode` rejects custom blend modes; `BLENDMODE_BLEND`
  premultiplies and thins the common case), and a hand-rolled per-pixel max-alpha
  merge in the hottest text-compositing path is a net negative under speed-first for
  an artifact invisible with every monospace font microide ships. Revisit only if an
  overhang/proportional font is ever routed through this atlas.
- **Plugin: `process.run`/`run_async` OOM-longjmp over live C++ locals;
  provider-query loops dereference `provider.state` before the null-plugin guard.**
  Verified non-defects. `process.run` already defers every deliberate raise to the
  `.inc` wrapper via `PushMessage` + `kPendingError`; the only remaining `lua_*`
  calls are unavoidable success-path table pushes shared by ~20 sibling interop
  functions (the invariant's own SAFE idiom). The provider-query guard is
  belt-and-suspenders: teardown erases every runtime entry for a state before the
  state is nulled (single plugin-worker thread), so `find_plugin_by_state` never
  returns null for a live iterated entry (pinned by
  `TestPluginHostSetupFailureTearsDownRegisteredProviders`). Reordering fixes no
  reachable failure and has no constructible regression.
- **Persistence: per-tab compare/merge divider fractions "unclamped" on restore.**
  Already neutralized upstream: `PrimitiveReader::ReadF32` replaces any non-finite
  persisted float with `0.0` at the binary-read source, and the render-time
  `std::clamp(fraction, min, 1-min)` brings finite out-of-range values (including the
  `0.0`) into a valid pane split. No NaN ever reaches layout arithmetic, so no
  restore-time sanitize is needed.

### Won't-do — platform-only, cannot compile/validate on this Linux host

Real defects, but writing untested Windows/macOS code risks a worse regression than the
latent bug. Kept documented with fix direction for a maintainer on that platform.

- **Windows `AsyncSubprocess`** declares `state_mutex` but never locks it and uses a
  non-atomic `running` → HANDLE UAF race. Fix: mechanically mirror the POSIX branch
  (lock around every state access, re-fetch the HANDLE under the lock, atomic bool).
- **Windows `RunSubprocess` ignores `options.timeout_ms`** (`WaitForSingleObject
  INFINITE`). Fix: timed wait + kill/reap, mirroring the POSIX deadline path.
- **Windows `IgnoreMatcher::ParseRule`** normalizes the glob through `lexically_normal`,
  corrupting a backslash-escaped literal. Benign on Linux.
- **macOS FSEvents incremental events bypass the ignore filter** (no directory prune, no
  per-file `filter.Includes`). Mirror the Linux inotify gate.
- **macOS FSEvents `FileIndexWatcher` `run_loop` publish race + `CFRunLoopStop` timing
  deadlock.** `run_loop` (plain `CFRunLoopRef`) is written by the worker thread but read
  unsynchronized by `StopNative()`, and `native_active` is set `true` before the worker
  runs, so a quick `Watch()`→`Unwatch()` can hit the null-`run_loop` guard, skip
  `CFRunLoopStop`, and hang `worker.join()` forever inside `CFRunLoopRun()`. Fix: atomic
  handoff of `run_loop` plus a `CFRunLoopRunInMode`-with-stop-flag loop so a stop issued
  before the run loop starts is not lost.
- **Windows `DirectoryTree::RelativeKeysExcludingRoot` `root_key` is not lowercased** while
  the stored expansion keys are (via `NormalizePathKey`), so the `key == root_key` guard
  never matches on `_WIN32`. Harmless (the `rel == "."` check still excludes the root);
  fixing needs the `#ifdef _WIN32` lowercasing replicated and tested on Windows.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.
- **`UNITY_BUILD` on `microide_tests`** (build-speed). Rejected 2026-06-29: 116 of 121 test TUs
  define helpers in anonymous namespaces, with confirmed same-name collisions across files
  (`MakeService`, `MakeViewport`, `MakeFixtureRoot`, …). CMake's unity batching concatenates files
  into one TU without isolating their unnamed namespaces, so those become colliding
  `(anonymous namespace)::` symbols → redefinition errors. Making it compile would mean rewriting
  ~116 files (per-file `UNITY_BUILD_UNIQUE_ID` namespace wrapping) for a speculative win the PCH pass
  below already captured the safe part of. Do not retry without first solving the anonymous-namespace
  collision mechanically. The shipped win instead was **precompiled headers** (stable std + SDL set,
  shared by `microide`/`microide_tests`/`microide_perf`): clean `microide_tests` build 141.3 s → 120.0 s
  (~15%), suite still green, runtime byte-for-byte unchanged.

## Shipped build-speed wins (2026-06-29, after the PCH pass)

- **Shared `microide_core` object library.** `MICROIDE_CORE_SOURCES` (≈373 TUs) is now an
  `OBJECT` library compiled once and spliced (`$<TARGET_OBJECTS:microide_core>`) into
  `microide`/`microide_tests`/`microide_perf`, instead of being re-listed in each target. This
  removed the previous double compile of the entire core. The prerequisite was making core
  **`MICROIDE_TESTING`-free**: ABI-neutral seams (`*ForTesting` methods, `TestAccess`/test friends,
  the `before_cache_apply_hook`, `test_sent_bytes_`) are now compiled unconditionally, and the
  genuinely behavioral forks (placeholder-vs-real terminal startup, project-init default terminal,
  `SendBytes` capture) are gated at **runtime** via `terminal::SetUsePlaceholderTerminalsForTesting`
  (the test/perf `main()` enables it), mirroring the existing `SetHostPlatformOverrideForTesting`
  precedent. Core must stay free of `#ifdef MICROIDE_TESTING`, or the shared object set diverges by
  ABI between binaries. Measured: clean build of `microide` + `microide_tests` 148 s → 91 s (~38%,
  ccache disabled for the comparison). `WorkspaceShellTestAccess.h` keeps its guard — it is a
  test-only header, never compiled into core.
- **ccache + ld.lld + split-dwarf**, all conditionally enabled in `CMakeLists.txt` (no-ops when the
  tool is absent; toggle with `-DMICROIDE_USE_CCACHE=OFF` / `-DMICROIDE_USE_LLD=OFF`). lld is skipped
  under LTO so the `microide-perf` preset keeps its default linker. `tools/run-checks.sh` exports
  `CCACHE_SLOPPINESS=pch_defines,time_macros` so PCH TUs cache.
- **Scoped inner-loop build.** `tools/run-checks.sh tests` and the documented loop build
  `--target microide_tests` (the only binary `ctest` invokes), skipping `microide` and the bench
  binaries.
- **Splitting `microide_tests` into multiple filtered `add_test` invocations for `ctest -j`**
  (test-run speed). Rejected 2026-06-29: partitioning ~121 files' worth of tests by name-substring
  filters is a silent-coverage hazard — a test matching no subset is dropped from the run with no
  signal, trading a correctness guarantee for ~2× wall-clock on an already-acceptable 47 s suite. The
  in-binary substring filter (`TestRunnerCli`) already covers focused local iteration. Do not retry
  without a mechanically-proven complete-and-disjoint partition (e.g. generated from the registry, not
  hand-maintained filter strings).

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, the
2026-06-15/16 render/app/util/terminal closeouts, and the 2026-07-12 deferred-backlog sweep (the
cross-subsystem bug-hunt passes 5–24 closeout) — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
