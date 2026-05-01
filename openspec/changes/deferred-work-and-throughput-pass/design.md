## Context

The previous performance pass (2026-04-29) closed the render-path and plugin-reload hotspots. Three classes of work remain that the baseline did not address:

1. **Blocking main-thread I/O** — git subprocess calls (`status`, `blame`, `log`) still run synchronously when the sidebar is activated; LSP `textDocument/didOpen` delivery is non-blocking by policy but is not enforced by any architectural lint rule. Any future contributor can re-introduce a blocking call.
2. **Per-frame recomputation of rarely-changing geometry** — `ComputeLayout()` runs unconditionally every frame; the visible-line range is recomputed independently by each render phase instead of being computed once and propagated.
3. **Scan-before-display latency** — the file finder and project search both trigger a full corpus scan before showing results, even though the corpus has not changed since the last scan. PCRE2 JIT compilation is re-triggered per search invocation.

The fix for each class follows the same principle: compute the slow thing once and reuse the result, amortizing the cost across many frames or events; never let the slow thing block the hot path.

## Goals / Non-Goals

**Goals:**
- Main thread never blocks on subprocess I/O, LSP socket reads, or file-system scans during any user-initiated operation
- `ComputeLayout()` is called at most once per frame, and zero times when geometry has not changed
- File finder overlay shows results immediately from the in-process index; no scan on open
- Search results begin arriving at the UI before the full corpus is scanned
- CPU is provably near-zero during true idle, confirmed by the `idle_soak_30s` harness scenario
- All techniques use only `std::thread`, atomic flags, `std::shared_mutex`, and SDL wake events — no GPU, no platform-specific threading primitives beyond the `FileIndexWatcher` OS-event wrappers

**Non-Goals:**
- GPU-accelerated rendering of any kind
- Multi-threaded render dispatch
- Rewriting the text layout engine or SDL backend
- Changing the plugin Lua runtime threading model

## Decisions

### D1 — FileIndexWatcher: thin platform abstraction over native directory events

A new `src/platform/FileIndexWatcher.{h,cpp}` wraps three native APIs under a single interface:

| Platform | Native API | Notes |
|---|---|---|
| Linux | `inotify` recursive watch | One inotify instance per project root; subdirectory watches added/removed as directories are created/deleted |
| macOS | `FSEvents` stream | `kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagFileEvents` for near-real-time delivery |
| Windows | `ReadDirectoryChangesW` | Overlapped I/O on a dedicated thread; `FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE` |

The watcher runs on a dedicated background thread. On each batch of events it locks a `std::shared_mutex`, updates the in-process `ProjectFileIndex` (sorted vector of `ProjectFile{path, mtime, size}`), and posts an SDL user event to trigger a UI refresh. File finder and search acquire a shared lock and read the index directly — zero scan latency.

**Why not extend the existing `FileWatcher`?** The existing watcher is per-file-handle oriented (watching specific config files); the new watcher needs recursive directory-tree tracking and bulk-update delivery, which are a different API shape.

**Poll fallback:** if native events are unavailable (sandboxed environment, NFS mount, kernel version) the watcher falls back to a snapshot-diff poll at the same interval as the existing `WorkspacePluginAssetMonitor`. This preserves the cross-platform contract without native-event support being a hard requirement.

### D2 — Background git dispatch using existing AsyncProcessState pattern

All git calls currently use synchronous `platform::Subprocess::Wait()` from the main thread. The fix wraps each call in the same `AsyncProcessState` pattern already established by `PluginAsyncStateInterop`:

- A single-thread executor per project (not a pool) serializes `git status`, `git blame`, and `git log` calls, preventing git object-store saturation under concurrent requests.
- Results are delivered via SDL user event; the sidebar and gutter consumers read them on the next frame.
- Project switch posts a cancellation token; in-flight results are discarded on delivery if the token has been invalidated.

**Why a single-thread executor rather than a thread pool?** Git's object store is not designed for high-concurrency access from the same working tree. Serializing calls per project matches git's own internal locking strategy while still not blocking the main thread.

### D3 — Layout dirty tracking and VisibleLineRange in FrameToken

`WorkspaceShellRenderFrame` gains a `layout_dirty_` flag (default `true`) set by resize, divider-drag, and sidebar/panel-toggle event handlers. `PrepareFrameOnce` skips `ComputeLayout()` when the flag is clear. The recomputed `WorkspaceLayout` is stored in a `LayoutCache{layout, version}` field on the coordinator.

`FrameToken` gains a `visible_line_range` POD field populated by `PrepareFrameOnce` once from `EditorTabService::ActiveViewport()`. All render phases that currently recompute the range independently consume it from the token instead.

The dirty flag defaults to `true` and is reset after `ComputeLayout()` completes. In ASAN builds an assertion fires if the first `RenderClip` call arrives before `PrepareFrameOnce` has run at least once (guards against flag-initialization bugs at startup).

### D4 — PCRE2 JIT compilation with bounded LRU cache

`pcre2_jit_compile()` is called immediately after `pcre2_compile()`. A `PatternCache` (flat hash map, max 64 entries, LRU eviction on overflow) lives in the search engine, keyed by `(pattern_string, flags)`. The cache is process-wide, not project-scoped, so patterns shared across project switches benefit from the cached compiled form.

JIT is optional: if `pcre2_jit_compile()` returns `PCRE2_ERROR_JIT_BADOPTION` (no executable-stack permission) the engine logs the failure once at startup and continues in interpreted mode. The capability degrades gracefully; no code path requires JIT.

### D5 — Search result streaming via batched SDL wake events

The search worker thread sends a batch of results via SDL user event after accumulating every 20 matches (configurable; `MICROIDE_SEARCH_BATCH_SIZE` build define). The results are written into a `std::shared_mutex`-protected `SearchResultBuffer` on the worker side; the UI acquires a shared lock on each frame and renders whatever is present. An atomic `cancel_` flag is checked between files; the worker exits on the next file boundary after cancellation.

Batch size 20 is the initial value, chosen to balance first-result latency against wake-event frequency. The `search_first_result` harness scenario measures the effect and a baseline is committed with the change; tuning is expected after measurement.

### D6 — Adaptive idle rendering via IdleHint

`Application` tracks `last_input_frame_` and an `in_flight_background_task_count_` counter (incremented/decremented by background services via a narrow API). `PrepareFrameOnce` returns an `IdleHint` enum:

| Hint | SDL call | Condition |
|---|---|---|
| `Full` | `SDL_PollEvent` loop | Input pending, or background work in flight |
| `CaretOnly` | `SDL_WaitEventTimeout(caret_remaining_ms)` | Only caret animation pending |
| `Idle` | `SDL_WaitEvent` | No input, no caret, no background work |

`in_flight_background_task_count_` is manipulated only by services (git, search, file-index watcher, LSP) posting a +1 on task start and -1 + SDL wake event on task completion. The counter never goes negative (ASAN assertion). The `idle_soak_30s` harness scenario verifies that the hint reaches `Idle` within 30 s after startup with no user input.

## Risks / Trade-offs

**[Risk] inotify watch limit on Linux** — Linux defaults to 8192 inotify watches per user. A project with many subdirectories can exhaust this. → Mitigation: the watcher counts watches created and falls back to poll when the limit is approached (same `sysctl fs.inotify.max_user_watches` detection used by other editors). Log a one-time warning.

**[Risk] FSEvents latency on macOS** — `kFSEventStreamCreateFlagNoDefer` reduces latency but increases wake-up frequency. → Mitigation: coalesce events with a 50 ms debounce on the watcher thread before posting the SDL event. Latency budget (index visible within 500 ms) is still met.

**[Risk] Layout cache miss on new UI operations** — A new sidebar or panel that toggles without setting `layout_dirty_` would show stale geometry. → Mitigation: `layout_dirty_` defaults to `true`; all sidebar/panel/divider event handlers are audited in the same change. An ASAN-mode assertion fires if geometry is consumed before being computed.

**[Risk] Git serialisation adds queue depth under rapid-fire blame requests** — Scrolling a file quickly triggers many blame requests; with a single-thread executor these queue up. → Mitigation: the blame dispatcher is already debounced (only the most-recent in-flight request is live; older ones are discarded on arrival). The executor queue is bounded to 4 items; excess requests are dropped in favour of the latest.

**[Risk] PCRE2 JIT not available** — Some hardened Linux kernels disallow the executable stack needed for PCRE2 JIT. → Mitigation: fallback to interpreted mode is automatic and transparent. The `search_first_result` harness scenario will still pass (it measures wall time, not JIT presence).

**[Risk] Search batch size tuning** — 20 results per wake event is a guess. Too small means excessive wake events; too large means high first-result latency. → Mitigation: the batch size is a build define; the `search_first_result` harness scenario drives the tuning decision before the baseline is committed.

## Migration Plan

1. `FileIndexWatcher` is additive; the file-finder scan trigger is removed once the watcher signals `IndexReady`. The old scan code path is deleted in the same change (not feature-flagged), so there is no dual-path to maintain.
2. Background git dispatch can be gated behind `MICROIDE_BG_GIT=1` during initial testing if a blocking regression is found; the default is on.
3. Layout dirty tracking is a refactor inside `WorkspaceShellRenderFrame`; no observable behaviour changes.
4. PCRE2 JIT and search streaming are internal search-engine changes; the search API surface is unchanged.
5. Adaptive idle requires no migration; the existing zero-delay `SDL_PollEvent` loop is replaced by the `IdleHint`-driven path.

## Open Questions

- **PatternCache scope**: process-wide LRU is the starting choice. If memory pressure is observed from large compiled patterns on projects that use many unique regexes, scope the cache to the active project and invalidate on switch.
- **Search batch size**: 20 is the initial guess. Measure against `search_first_result` baseline before committing.
- **Blame debounce interval**: currently inherited from the existing implementation. If the harness shows that rapid scroll still saturates the git executor queue, increase the debounce from the current value.
