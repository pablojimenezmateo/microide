# MicroIDE Known Tech Debt

Reviewed on 2026-07-09.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
It is deliberately short. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

These three were surfaced by the 2026-07 cross-subsystem bug-hunt passes and
**deliberately deferred** — each is either entangled with a semantics decision or
a latent API-contract hazard with no live trigger, so a rushed fix risked a
regression worse than the defect. Recorded here so they are not silently lost.

- **Terminal `ED` (`CSI 2J`) scrollback loss is entangled with primary-screen
  absolute-row CUP.** `TerminalSession::EraseInDisplayLocked` (`src/terminal/TerminalSessionScreen.cpp:304`,
  dispatched from `src/terminal/TerminalSessionCsi.cpp:138`). On the primary screen,
  `CSI H` maps to absolute row 0 (no viewport-top offset — only alt-screen + origin
  mode offsets), which entangles `2J` erase semantics with cursor origin. A naive
  "preserve scrollback on 2J" change breaks `clear(1)`, which relies on the current
  absolute-row behaviour. A correct fix needs viewport-relative CUP rework first,
  which is out of scope for a bug-hunt pass. **Blocked on:** deciding viewport-relative
  vs absolute CUP on the primary screen, then reworking `2J`/`clear` together.

- **Branch-review cross-generation `ChangedSinceReviewed` status is effectively
  unreachable.** `src/compare/BranchReviewStateService.cpp` /
  `src/compare/BranchReviewStateTypes.h` (`BranchReviewMarkerStatus::ChangedSinceReviewed`).
  The status can only be produced if a target's snapshot generation advances while a
  reviewed marker survives, but the current generation/identity bookkeeping never
  reaches that combination, so the branch is dead. Resolving it is a **product
  decision** on snapshot-generation semantics and touches the persisted
  `BranchReviewTargetIdentity`, so it is not a mechanical fix. **Blocked on:** defining
  the intended "changed since I reviewed it" semantics, then either wiring the
  producer or removing the status (and migrating persisted state).

- **`PieceTree::LineView` same-index re-read can dangle a previously returned view.**
  `src/editor/PieceTree.cpp:327`. When a line spans multiple pieces, `LineView`
  returns a `string_view` into `line_view_cache_[index]`; a second `LineView(index)`
  call for the same index on the non-contiguous path does `cached.clear()` +
  `CopyRange`, reallocating that slot and invalidating the earlier view. No live
  caller holds a `LineView` result across a re-read of the *same* index today, so this
  is a latent API-contract hazard, not a live crash — but it is a hot path, so the
  fix (e.g. documenting single-live-view-per-index, or returning owned storage on the
  slow path) must stay allocation-free in the common contiguous case. **Blocked on:**
  nothing external — deferred only to avoid perturbing a hot path without a measured
  guard; pick up when the LineView contract is next touched.

The 2026-07-09 fourth-pass sweep also surfaced these lower-severity items, deferred
as benign/cosmetic or as design constraints rather than live defects:

- **Terminal background-color erase (BCE) is inconsistent across erase paths.**
  `TerminalSession::EraseInDisplayLocked` (whole-line `cells.clear()` / `assign`) and
  the scroll-region fills (`ScrollRegionUpLocked`/`ScrollRegionDownLocked` insert
  default `TerminalLine{}`) drop the current SGR background, while `EraseInLineLocked`
  honours it via `current_style_`. So `\x1b[44m\x1b[2J` on the alt screen renders with
  the default background whereas `\x1b[44m\x1b[K` paints blue. Fixing it means filling
  erased lines with `columns_` styled cells instead of the empty-line fast path, which
  touches the memory model the terminal-scroll perf work relies on — measure before
  changing. **Blocked on:** a perf-neutral styled-blank-line representation. Cosmetic
  (BCE is an optional xterm feature) so low priority.

- **LSP incremental `didChange` LF-joins replacement text on CRLF documents.**
  `src/workspace/LspService.cpp:928-940` sends `applied_edit->replacement_text` (built
  LF-joined in `TextViewportUndoHistory.cpp`) even for a CRLF buffer, whose `didOpen`
  used the real CRLF ending. The server mirror's edited regions drift toward LF. Traced
  as benign: LSP positions are line/character (never byte offsets), replacement text
  never contains a stray `\r`, edit ranges align to line-content boundaries so a
  `\r\n` pair is never split, and the next full sync (save/clean transition) reconciles.
  Not fixed because the correction adds per-keystroke string work on the hot
  incremental-sync path to fix an inconsistency with no demonstrable wrong-position
  outcome (speed is the priority). **Blocked on:** a demonstrated corruption case, or
  doing the transform only when `line_ending() == CRLF` if a case ever surfaces.
  (Re-confirmed by an independent 2026-07-09 fifth-pass hunter as a real server-mirror
  desync, but it again could not exhibit a concrete wrong-*position* outcome, and no
  LF→CRLF string helper exists, so the guarded send-site re-encode stays deferred
  pending a demonstrated corruption case — the disciplined call given speed is priority.)
  (Note: the sibling "cross-language go-to-definition uses the source server's encoding"
  suspicion was investigated and is NOT a bug — a `definition` response's `character`
  is in the responding/source server's encoding regardless of the target file, which is
  exactly what `AssistService::NavigateToLspLocation` uses.)

- **DAP multi-thread semantics are single-thread-simplified.** Two spots assume the
  app's single-thread debugging focus: `DebugServiceCallbacks.cpp` records a verified
  breakpoint at the *requested* line rather than the adapter's relocated `breakpoint.line`
  (the store is keyed by requested line, so it structurally cannot represent a
  relocation — the async `breakpoint`-event path uses the real line, so the two can
  disagree), and `DebugSession.cpp:258-264` treats a `continued` event as a full resume,
  ignoring `allThreadsContinued`/`threadId`. Both are correct for single-threaded targets
  and incorrect per spec for multi-threaded ones. **Blocked on:** a decision to support
  per-thread stop/resume state, which would re-key `BreakpointStore` and add per-thread
  run state.

The 2026-07-09 fifth-pass sweep surfaced these, deferred as latent/broad rather
than live defects (the pass fixed nine concrete bugs outright — terminal SGR mouse
release, IL/DL alt-screen margins, CPR pending-wrap column, resize tab-stop loss;
the editor horizontal-scroll caret double-count; the LSP pre-init shutdown drift and
dropped integer diagnostic code; the plugin string-array raw read; and the
ProjectFileScanner symlink-cycle crash):

- **Plugin metamethod reads can longjmp over live C++ destructors (same invariant,
  different trigger than `luaL_error`).** Plugin-controlled tables are harvested with
  the metamethod-capable `lua_getfield`/`lua_geti` while non-trivial C++ locals are
  alive — representative sites `src/plugin/PluginDiagnosticsInterop.cpp` (`ReadDiagnosticTable`,
  with `std::vector<Diagnostic>`/`std::filesystem::path` live), the `Read*Field` helpers
  in `src/plugin/PluginLuaInterop.cpp`, and the provider/registration parsers. A plugin
  that puts a raising `__index` metamethod (`setmetatable`+`error` are in the exposed
  base lib) on a harvested table makes the field read `longjmp` to the enclosing
  `LuaRuntime::PCall`, skipping those destructors (heap leak + UB) — the same hard
  invariant `luaL_error` is banned to protect. Reachable, but leak-only and requires a
  self-sabotaging plugin. NOT a clean drop-in fix: a blind `lua_getfield`→`lua_rawget`
  swap would break well-behaved plugins that legitimately use an `__index` metatable
  for field defaults; the correct fix is to harvest each plugin table inside a nested
  protected call (`lua_pcall`) so a metamethod raise is caught before it crosses the
  C++ frame. (The one *sequence* read where raw is unambiguously correct —
  `ReadStringArrayField` — was converted to `lua_rawgeti` this pass.) **Blocked on:**
  a protected-harvest helper (or a decision that raw field reads are acceptable),
  applied across ~13 interop TUs.

- **In-buffer regex "find next" is uncancellable on a pathological single-line file.**
  `src/project/ProjectSearchService.cpp:50` (`FindNextRegexMatch` empty-match advance
  loop). Cancellation is checked once per *line* (`:367`), never inside the per-offset
  empty-match loop, so a pattern that matches empty at every offset (e.g. `x?`) against
  a very large single-line file (up to the 512 MiB read cap) spins ~N PCRE2 `Match`
  calls one byte at a time before returning — PCRE2's `match_limit`/`depth_limit` bound
  a single match, not the *count* of separate `Match` calls, so `Stop()` cannot
  interrupt it. Responsiveness/soft-DoS bounded by line length, not corruption.
  **Blocked on:** a measured per-line iteration/token check that does not slow the hot
  search path (speed is the priority), so deferred rather than rushed.

- **`PieceTree` `add_` buffer offset is truncated to `uint32` with no guard.**
  `src/editor/PieceTree.cpp` (`AppendToAdd`). `RebuildFromOriginal` deliberately guards
  the `original_` buffer against exceeding `UINT32_MAX`, but the append-only `add_`
  buffer (which every `InsertText` grows and which is compacted only on a full
  `Reset`/`ResetFromText`) has no equivalent guard: once cumulative inserted bytes in
  one session reach 4 GiB, `start = static_cast<uint32_t>(add_.size())` wraps and later
  pieces reference wrong offsets → silent text corruption. Latent — needs a multi-GiB
  cumulative-insert session with no intervening reload, so not practically demonstrable
  today. **Blocked on:** nothing external; add a parity guard (compact/rebuild or refuse
  the insert) when `add_.size()` would exceed the 32-bit range, next time PieceTree is
  touched.

The **timing-dependent test flakiness** item (added 2026-06-21) is now closed. The
single-check-after-a-fixed-`sleep_for` anti-pattern was audited across every
`sleep_for` site under `tests/` and remediated:

- `ControlChannelService/SocketSelfHealsAfterExternalDeletion` — poll-drains
  `ConsumeControlCallbacks()` until the descriptor reappears (commit `ae2523bb`).
- `ProjectSearchService/StopDiscardsLateUpdates` — now drains `TakePendingUpdate()`
  over a bounded window and asserts no update ever surfaces after `Stop()`, instead of
  checking once after a fixed 25 ms wait (`tests/ProjectSearchServiceTests.cpp`).
- `Subprocess` async read/write concurrency fixture — now poll-waits on an
  `std::atomic<bool> reader_running` signal so a read is genuinely in flight before the
  timed write, instead of sleeping a fixed 100 ms (`tests/SubprocessTests.cpp`).

The candidates flagged for follow-up review (`FileIndexWatcherTests`,
`ExternalRepoChangeTests`, `ProjectChangeTests`, `WorkspaceDapClientTests`,
`WorkspaceLspClientTests`, `TerminalSessionTests`) were audited and already use the
bounded poll-until-condition shape; no changes were needed. The durable convention —
**poll a condition until a bounded deadline for every cross-thread assertion; never
assert state immediately after a fixed `sleep_for`** — is recorded in
`guidelines/testing.md`.

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
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, and
the 2026-06-15/16 render/app/util/terminal closeouts — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
