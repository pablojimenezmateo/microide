## 1. FileIndexWatcher Platform Abstraction (D1)

- [x] 1.1 Define `FileIndexWatcher` interface in `src/platform/FileIndexWatcher.h` with `Watch(root_path)`, `Unwatch()`, `SetCallback(IndexUpdateBatch)`, and `IsNative()` (returns false on poll fallback); document the threading contract (callback fires on watcher thread)
- [x] 1.2 Implement Linux `inotify` backend: recursive directory watch with subdirectory watches added/removed as directories are created or deleted; fan-out inotify events into `IndexUpdateBatch` (created/deleted/renamed entries with path + mtime)
- [x] 1.3 Implement macOS `FSEvents` backend: `kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagFileEvents`; 50 ms coalesce debounce on the stream callback thread before emitting `IndexUpdateBatch`
- [x] 1.4 Implement Windows `ReadDirectoryChangesW` backend: overlapped I/O on a dedicated thread; `FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE`; translate notifications to `IndexUpdateBatch`
- [x] 1.5 Implement poll-fallback mode (snapshot-diff at the same interval as `WorkspacePluginAssetMonitor`); activate automatically when inotify watch limit is exhausted or native events unavailable; emit a one-time `SDL_Log` warning with the failure reason
- [x] 1.6 Add unit tests for the watcher: start watch on a temp directory, create/delete/rename a file, assert the callback fires with the correct `IndexUpdateBatch` within 600 ms; cover the poll-fallback path with a mock clock
- [x] 1.7 Add the watcher to `CMakeLists.txt` with platform-conditional source selection and no new external dependencies

## 2. In-Process ProjectFileIndex (D1 continued)

- [x] 2.1 Refactor `src/project/FileIndex.{h,cpp}` to hold a sorted `std::vector<ProjectFile>` (path + mtime + size) protected by `std::shared_mutex`; add `ApplyBatch(IndexUpdateBatch)` (exclusive lock) and `Snapshot() -> std::vector<ProjectFile>` (shared lock) methods
- [x] 2.2 Wire `FileIndexWatcher` startup to project open in the workspace coordinator: on `InitializeCurrentProject`, construct watcher for the project root, connect its callback to `ProjectFileIndex::ApplyBatch` + SDL wake event post
- [x] 2.3 Wire watcher teardown to project close and project switch: call `Unwatch()` and reset the index before the new project's watcher starts
- [x] 2.4 Update the file-finder overlay open path to query `ProjectFileIndex::Snapshot()` instead of triggering a directory traversal; remove the old scan entry point from the file-finder call site
- [x] 2.5 Update the project search worker to obtain its file list from `ProjectFileIndex::Snapshot()` at search start instead of calling a directory-traversal helper
- [x] 2.6 Add integration tests: cold open with a pre-seeded directory, assert file-finder query returns expected files without any scan; add/remove a file, assert the index updates and the file-finder reflects the change

## 3. Background Subprocess Isolation — Git (D2)

- [x] 3.1 Introduce `src/project/ProjectBackgroundExecutor.{h,cpp}`: single-thread executor per project (one `std::thread` + `std::deque` work queue + `std::mutex` + `std::condition_variable`); supports `Post(task)`, `PostLatest(key, task)` (replaces any pending task with the same key — used for blame debounce), `Cancel()`, and `Shutdown(deadline)`
- [x] 3.2 Migrate `GitOperations::Status()` call site in the git-sidebar coordinator to dispatch through `ProjectBackgroundExecutor`; deliver the result to the sidebar via SDL user event; sidebar enters "refreshing" state on the dispatch frame and renders the result on the wake frame
- [ ] 3.3 Migrate `GitOperations::Blame()` call site to use `PostLatest("blame", ...)` so rapid scroll triggers discard superseded blame requests; deliver result via SDL user event to the gutter renderer
- [ ] 3.4 Migrate `GitOperations::Log()` call site to dispatch through the executor; deliver result via SDL user event
- [ ] 3.5 On project switch, call `ProjectBackgroundExecutor::Cancel()` before constructing the new project's executor; verify that results delivered after cancel are discarded (checked against the project-scoped cancel token, not a shell-level flag)
- [ ] 3.6 Add integration tests: open project, activate git sidebar, assert main thread never blocks on git subprocess; switch projects mid-operation, assert old result is discarded and new project starts clean

## 4. Background Subprocess Isolation — Lint Enforcement (D2 continued)

- [x] 4.1 Add `CheckNoSynchronousSubprocessWaitInWorkspace` rule to `tests/ArchitectureInvariantsTests.cpp`: hard-fail if any file under `src/workspace/` contains a direct call to `Subprocess::Wait()`, `waitpid()`, `WaitForSingleObject()`, or equivalent blocking-wait primitives; verify by introducing a bait call (confirm failure) then removing it (confirm pass)
- [x] 4.2 Add `CheckLspDidOpenIsNonBlocking` rule to `tests/ArchitectureInvariantsTests.cpp`: hard-fail if `textDocument/didOpen` or `textDocument/didChange` notification sends appear in a synchronous path reachable from `EditorTabService::ActivateTab`; document the expected async dispatch pattern in the rule comment
- [x] 4.3 Audit existing LSP `textDocument/didOpen` dispatch path against the new lint rule; fix any synchronous send found; add a regression test asserting that tab hydration completes before the LSP server has acknowledged the notification

## 5. Layout And Geometry Cache (D3)

- [x] 5.1 Add `layout_dirty_` bool flag (default `true`) to the workspace render coordinator; set it in the window-resize event handler, the divider-drag event handler, and all sidebar/panel-toggle event handlers; clear it after `ComputeLayout()` completes in `PrepareFrameOnce`
- [x] 5.2 Guard `ComputeLayout()` in `PrepareFrameOnce` with the dirty flag: skip the call when `!layout_dirty_`; verify with a perf-trace that `ComputeLayout` is absent from the trace for a typing frame with no geometry change
- [ ] 5.3 Add `visible_line_range` (start line, end line, viewport) POD field to `FrameToken`; populate it once from `EditorTabService::ActiveViewport()` in `PrepareFrameOnce`; set to sentinel when no editor tab is active
- [ ] 5.4 Update all render phases that currently call `EditorTabService::ActiveViewport()` independently to read `visible_line_range` from the `FrameToken` instead; remove the now-redundant viewport calls
- [ ] 5.5 Add `TabStripGeometryCache` struct (tab widths, positions, overflow offset) keyed by `{tab_count, window_width, active_tab_index}`; populate on first use and after any key change; store alongside the tab-strip render state
- [ ] 5.6 Add ASAN-mode assertion in `Application::WorkspaceRenderClip` that `PrepareFrameOnce` has been called in the current frame (guards against layout-not-computed bugs at startup and after window show/hide)
- [ ] 5.7 Add architectural lint rule to `tests/ArchitectureInvariantsTests.cpp`: hard-fail if render TUs covered by the existing lint call `ComputeLayout()` or access `context_.window_size` directly (geometry is mediated through `FrameToken`)
- [x] 5.8 Add unit tests: assert `ComputeLayout` call count is 0 when dirty flag is clear for 10 consecutive frames; assert count is 1 on the frame after a resize event

## 6. PCRE2 JIT Compilation And Pattern Cache (D4)

- [x] 6.1 Add `pcre2_jit_compile(pattern, PCRE2_JIT_COMPLETE, NULL)` call immediately after `pcre2_compile()` in the search engine; capture the return code; if it indicates JIT unavailable, emit `SDL_Log` once at engine init and continue in interpreted mode
- [x] 6.2 Implement `PatternCache` as a flat hash map (pattern string + flags → `Pcre2Handle`) with LRU eviction at 64 entries; use a doubly-linked list of keys ordered by last-use for O(1) eviction; thread-safe with a `std::mutex` (cache is read by the search worker thread)
- [x] 6.3 Wire `PatternCache` into the search engine: on search start, look up pattern in cache; on miss, compile + JIT + insert; on hit, reuse the cached handle without recompiling
- [x] 6.4 Unit test: compile the same pattern N times (N > 1), assert `pcre2_compile` was called exactly once; unit test: insert 65 patterns, assert cache size stays at 64 and the LRU entry is evicted; unit test: JIT error path emits the diagnostic exactly once across multiple searches

## 7. Incremental Search Result Streaming (D5)

- [ ] 7.1 Add `SearchResultBuffer` holding a `std::vector<SearchResult>` protected by `std::shared_mutex` and a `search_id` (monotonic counter to distinguish results from cancelled searches)
- [ ] 7.2 Modify the search worker to write to `SearchResultBuffer` in batches of `MICROIDE_SEARCH_BATCH_SIZE` results (default 20; compile-time define) and post an SDL user event after each batch
- [ ] 7.3 Add atomic `cancel_` flag to the search context; check it between files; exit the worker loop immediately when set
- [ ] 7.4 Update the search overlay UI to acquire a shared lock on `SearchResultBuffer` and render whatever results are present on each frame triggered by a search wake event; do not wait for the `search_done` signal before showing partial results
- [ ] 7.5 Update the cancellation path (new search initiated, overlay dismissed): set `cancel_`, increment `search_id`, clear the buffer under exclusive lock, discard any wake events carrying the old `search_id`
- [ ] 7.6 Add integration tests: search on a multi-file fixture, assert the UI receives a partial result batch before the worker has processed all files; cancel a search, assert the worker stops within one file boundary; empty-result search returns the empty-results state promptly

## 8. Adaptive Idle Rendering (D6)

- [x] 8.1 Add `in_flight_background_task_count_` atomic int to `Application` (or a narrow `BackgroundTaskRegistry` service); add `IncrementTaskCount()` and `DecrementTaskCount() + PostWakeEvent()` methods; add ASAN assertion that the count never goes negative
- [x] 8.2 Wire `FileIndexWatcher` to call `IncrementTaskCount()` when initial index build starts and `DecrementTaskCount()` when `IndexReady` fires
- [x] 8.3 Wire `ProjectBackgroundExecutor` git dispatch to call `IncrementTaskCount()` on `Post()` and `DecrementTaskCount()` on result delivery via SDL user event
- [x] 8.4 Wire search worker to call `IncrementTaskCount()` on search start and `DecrementTaskCount()` on worker exit (whether complete or cancelled)
- [x] 8.5 Add `IdleHint` enum (`Full`, `CaretOnly`, `Idle`) as the return type of `PrepareFrameOnce`; derive it from: `in_flight_background_task_count_ > 0 → Full`; `caret_visible && caret_blink_pending → CaretOnly`; otherwise `Idle`
- [x] 8.6 Replace the zero-delay `SDL_PollEvent` loop in `Application` with an `IdleHint`-driven strategy: `Full → SDL_PollEvent`; `CaretOnly → SDL_WaitEventTimeout(caret_remaining_ms)`; `Idle → SDL_WaitEvent`
- [x] 8.7 Add unit tests: assert `IdleHint == Idle` when task count is 0 and no caret is active; assert `IdleHint == Full` while a task is in flight; assert the counter never reaches -1 (ASAN assertion fires on underflow)
- [ ] 8.8 Run the `idle_soak_30s` harness scenario and verify the event-loop statistics show near-zero wake rate after all background work settles

## 9. Perf Harness Scenarios And Baselines

- [x] 9.1 Add file-finder fixture: a flat project tree with 10 000 synthetic files under `tests/perf/fixtures/file_finder_large/`; commit the index-metadata alongside (do not require git history)
- [x] 9.2 Author `tests/perf/scenarios/file_finder_cold.cpp`: builds the in-process index from the fixture, simulates file-finder open, measures time-to-first-result, asserts ≤ 50 ms on reference host
- [x] 9.3 Add git fixture repository (bare, pre-seeded with 1 000 tracked files) under `tests/perf/fixtures/git_status_project/`
- [x] 9.4 Author `tests/perf/scenarios/git_sidebar_activate.cpp`: activates the git sidebar on the git fixture project, measures time from activation to first rendered git-status frame, asserts ≤ 200 ms on reference host
- [x] 9.5 Author `tests/perf/scenarios/search_first_result.cpp`: initiates a search on the 10 000-file fixture with a pattern that matches one file near the end of the corpus, measures time-to-first-result, asserts ≤ 100 ms on reference host
- [x] 9.6 Update `tests/perf/scenarios/idle_soak_30s.cpp` to additionally assert that the file-index watcher thread and git executor thread generate zero SDL wake events during the 30-second soak period after startup work completes
- [x] 9.7 Capture baselines for all new and updated scenarios on `perf-runner-v1`; commit `tests/perf/baselines/file_finder_cold.json`, `git_sidebar_activate.json`, `search_first_result.json`, and an updated `idle_soak_30s.json`; tag the change record with `perf-baseline: initial capture for deferred-work-and-throughput-pass`

## 10. Documentation And Final Validation

- [x] 10.1 Update `docs/active-work.md` with the new shipped capabilities: event-driven file index, background git dispatch, layout cache, search streaming, adaptive idle
- [x] 10.2 Update `docs/known-tech-debt.md` to mark item 5 (search/index snapshot model) as resolved by this change; update any related items whose status has changed
- [x] 10.3 Update `AGENTS.md` § Do-Not-Regress Patterns with four new invariants: (a) no synchronous subprocess wait on main thread in workspace code, (b) LSP didOpen is non-blocking, (c) `ComputeLayout` skipped when dirty flag is clear, (d) SDL event loop never uses zero-delay poll at idle
- [x] 10.4 Update `docs/perf-harness.md` with the three new scenarios and the extended `idle_soak_30s` coverage
- [x] 10.5 Run the full default test suite: `cmake --build build/microide && ctest --test-dir build/microide --output-on-failure`
- [ ] 10.6 Run the ASAN preset: `cmake --preset microide-asan && cmake --build build/microide-asan && ctest --test-dir build/microide-asan --output-on-failure`
- [ ] 10.7 Run the UBSAN preset: `cmake --preset microide-ubsan && cmake --build build/microide-ubsan && ctest --test-dir build/microide-ubsan --output-on-failure`
- [ ] 10.8 Run the TSAN preset (primary validation for all new background-thread paths): `sudo sysctl vm.mmap_rnd_bits=28 && cmake --preset microide-tsan && cmake --build build/microide-tsan && ctest --test-dir build/microide-tsan --output-on-failure`; confirm zero data-race reports on all new watcher, executor, pattern-cache, and search-buffer paths
- [ ] 10.9 Capture `MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1` output from the idle-soak workflow and attach to the change record showing the event-loop reaches `SDL_WaitEvent` after background work settles
