# MicroIDE Performance Findings

Last reviewed on 2026-04-22 after startup profiling focused on project-open overhead.
Updated on 2026-04-22 with Git status and syntax definition deferral optimizations.
Updated on 2026-04-22 with asynchronous LSP server initialization to prevent UI blocking.
Updated on 2026-04-23 with deep-dive static analysis of render-path and edit-path bottlenecks.
Updated on 2026-04-23 with syntax, viewport, terminal, and output-panel cache fixes from that review.
Updated on 2026-04-23 with terminal foreground run coalescing, buffer-search caching, SDL text-cache lookup cleanup, and syntax-rule partitioning.
Updated on 2026-04-23 with unchanged plugin-syntax reload skipping via source fingerprinting and
generated syntax registry reuse on cold plugin syntax reloads.
Updated on 2026-04-23 with second whole-project static pass confirming all previous fixes and
identifying four new render-path bottlenecks in review-comment rendering and editor pane layout.
Updated on 2026-04-23 with review-comment line indexing and O(1) render marker lookups.

This note captures concrete bottlenecks that were found in the current codebase, what was already
fixed, and what still remains worth doing.

Updated on 2026-07-01 with lazy per-definition syntax-rule compilation, removing the dominant
~75 ms startup cost and deleting the ineffective syntax-warmup thread.

Updated on 2026-07-02 with compare/merge render per-row allocation elimination, a cached
project-search sidebar status line, and the release binary switching to LTO.

## Fixed 2026-07-02 (compare/merge render allocations + sidebar status cache)

### Compare/merge surface render allocations (per visible row, per frame)

Problem:

- `WorkspaceShellCompareRender.cpp` / `WorkspaceShellMergeRender.cpp` (the compare/merge surface
  render TUs) escaped every render-hot-path allocation lint because those rules select files by the
  `WorkspaceShellRender*` filename prefix and these two did not match it. They accumulated the exact
  per-row allocations the lint bans elsewhere: a fresh `editor::DecoratedTextRow` (3 vectors) built
  per visible row per pane per frame; `TruncateToWidth` returning a `std::string` even when the text
  already fit; and a per-row review-marker summary assembled with a copy plus a 4-way `+` concat.

Implemented:

- Reuse mutable scratch `DecoratedTextRow` members (`compare_left/right_scratch_row_`,
  `merge_incoming/current_scratch_row_`) mirroring `EditorViewRenderer::scratch_row_`;
  `BuildDecoratedRow` clears the vectors first, so capacity is retained across rows.
- Add `TextRenderer::TruncateToWidthView` / `WorkspaceShell::TruncateLabelView`: the fits-case
  returns the input view unchanged (zero alloc), the truncated case writes into a `thread_local`
  scratch. Hot compare/merge label draws use the view variant.
- Bake the review-marker prefix into `ComparePresentationRow::display_summary_text`, composed once
  at presentation build and by `ApplyBranchReviewPresentationMarkers`.
- Build the merge scrollbar-marker input vector only on cache miss; select the merge status line as
  a `string_view`; draw the single-char diff marker via `string_view`.
- Rename the two TUs to `WorkspaceShellRenderCompare.cpp` / `WorkspaceShellRenderMerge.cpp` so the
  lint set covers them permanently, and add `CheckCompareMergeRenderUsesScratchRows` plus the
  literal+ident concat ratchet to lock the wins in.

Measured (same-machine A/B, deterministic p50 allocation counts, `perf-runner-v1`):

- `merge_scroll_interleaved_hunks`: 334,736 → 308,158 allocations (−8%)
- `compare_scroll_selection`: 319,482 → 295,185 allocations (−8%)
- `compare_scroll_large_fixture`: 359,204 → 359,038 (flat — that scenario's allocation budget is
  dominated by window syntax tokenization / the editable right pane, not the decorated-row path)

No regressions. Wall-time rebaseline was deferred: the capture host was under heavy background load
this session (all scenarios read ~2× their committed wall baselines uniformly, including scenarios
that touch none of the changed code), so the deterministic allocation counts are the trustworthy
signal and the committed wall baselines were left untouched pending a quiet-machine rebaseline.

### Project-search sidebar status line (per frame while visible)

Problem:

- The Search sidebar's status/hint line was reassembled every frame from 5+
  `JoinHintSegments`/`BuildCountStatus`/`BuildShownOfTotalStatus` calls (each allocating) whenever
  the panel was visible, and search-progress wakes repaint it constantly.

Implemented:

- Compose it once in `RenderViewModelBuilder::BuildSidebarSurface` behind a thread-local cache keyed
  on the inputs that change the text; expose `SidebarSurfaceViewModel::project_search_status_text`
  (a view into the cache) and draw it via the allocation-free `TruncateLabelView`. The text is fully
  determined by the key (it embeds neither the query nor the error string, only whether they are
  empty), so a key hit is always a correct reuse.

## Fixed In This Pass

### Lazy per-definition syntax-rule compilation (startup)

Problem:

- `RuntimeSyntaxRegistry::EnsureInitialized` eagerly PCRE2-compiled every rule regex across all 157
  built-in language definitions (~8,800 patterns) when the `BuiltInRegistry()` magic-static first
  ran. A `MICROIDE_STARTUP_TRACE` capture showed this single scope at **~75 ms of the ~94 ms**
  startup-to-first-frame path (≈80%).
- A background "warmup" thread in `Application::Initialize` was meant to hide this cost, but
  `WorkspaceShell::Initialize` forced `EnsureInitialized()` ~0.1 ms later, so the main thread
  blocked on the same magic-static barrier and the full 75 ms stayed on the critical path.
- A session opens files of only a handful of languages, yet all 157 definitions' rules were
  compiled. Only the detection regexes (`filename`/`header`/`signature`, ~200 total) are needed for
  `DetectFiletype`.

Implemented:

- Built-in definitions' rule regexes are now compiled lazily on first highlight of that language.
  `Rule` retains `const char*` back-pointers into the static generated tables and `mutable`
  `CompiledRegex` fields; `Registry` owns a `std::vector<std::once_flag>` guard table (sized in
  `PartitionDefinitionRules`); `EnsureDefinitionCompiled` runs at the top of `HighlightLineScoped`
  under `std::call_once`. Detection regexes stay eager. Plugin/runtime definitions stay eager so
  their regex errors still surface at reload time.
- The `Registry` becomes move-only (once_flags are non-copyable); `BuildRegistry`'s empty-runtime
  path clones the built-in registry element-wise via `AppendRegistryWithOffset` instead of
  copy-assigning it.
- The now-pointless `syntax_registry_warmup_` thread (spawn, joins, member, `<thread>` includes) was
  removed from `Application`.

Impact (`MICROIDE_STARTUP_TRACE=1 SDL_VIDEODRIVER=dummy … --safe-mode`):

- `RuntimeSyntaxRegistry::EnsureInitialized`: **~75 ms → ~3 ms**
- `Application::Initialize`: **~82 ms → ~10 ms**
- startup-to-first-frame: **~94 ms → ~18–24 ms**
- Also lowers memory (only used languages compile) and removes a thread spawn/join per launch.

Thread-safety: `std::call_once` serializes concurrent compiles (background prefetch worker vs.
main-thread render cache-miss) and publishes the compiled regexes with a happens-before edge,
independent of `RegistryMutex`. Verified clean under TSan/ASan/UBSan, with a new concurrent
same-language highlight regression test.

Note: committed perf baselines (`tests/perf/baselines/cold_startup_*.json`) are captured on
reference hardware only and were left unchanged; the improvement should be re-baselined there with a
`perf-baseline:` tag.

### Syntax highlight hot-path allocation and cache invalidation cleanup

Problem:

- syntax highlighting allocated PCRE2 match data on every regex use and allocated match vectors per
  rule application
- viewport edits cleared all checkpoints and per-line syntax state, so the first repaint after a
  keystroke could rebuild highlight state much farther than necessary
- visible-line layout caching treated caret-only movement as a text-layout miss

Implemented:

- `RuntimeSyntaxRegistry` now reuses thread-local match data per compiled pattern and reuses one
  match buffer per pattern-rule pass instead of allocating per regex call
- `TextViewport` now invalidates derived highlight state from the edited line forward instead of
  clearing the whole document
- highlight checkpoints are now built lazily per needed checkpoint instead of synchronously
  rebuilding the full checkpoint array on first post-edit access
- visible-line cache keys no longer include the caret column; caret placement is recomputed from
  cached line layout at query time
- direct viewport coverage now verifies cursor-only movement still hits the visible-line cache and
  tail edits do not rebuild far checkpoints

Impact:

- syntax highlighting removes a large class of per-frame heap churn on highlighted lines
- ordinary edits no longer force whole-document checkpoint rebuilds before the next painted line
- left/right caret movement now reuses cached text layout for the active line

### Terminal visible-range and output-snippet caching

Problem:

- terminal panel rendering deep-copied the visible terminal lines every frame even while idle
- output-panel code snippets reran the full syntax highlighter every frame for visible snippet rows

Implemented:

- `TerminalSession` now caches the last requested visible line-range snapshot and invalidates it
  only when transcript lines actually change
- `WorkspaceOutputChannels` now parses output reference lines and code-context snippets on append,
  and caches snippet highlighting by resolved path instead of recomputing it every frame
- focused tests now cover cached terminal visible snapshots and cached output-channel snippet
  parsing/highlighting

Impact:

- idle terminal rendering avoids repeated visible-range transcript copies for unchanged content
- output panels with code context stop re-highlighting the same visible snippets on every repaint

### Remaining deep-dive render-path fixes

Problem:

- terminal foreground rendering still issued one draw per visible cell even when long spans shared
  one foreground color
- buffer search lowercased every visible line every frame while active
- `SdlTtfTextBackend` still allocated a `std::string` for cache lookup on every draw call
- syntax rule application still scanned all rules for each region instead of iterating only the
  relevant subset

Implemented:

- terminal panel rendering now coalesces foreground draws into color runs instead of drawing one
  cell at a time
- `EditorViewRenderer` now caches per-line buffer-search match ranges by viewport, layout revision,
  line index, and lowered query
- `SdlTtfTextBackend` now uses a transparent structured cache key so cache hits do not allocate a
  temporary `std::string`
- runtime syntax definitions now prepartition pattern and region rules by parent region at registry
  build time so highlight passes iterate only relevant rule subsets

Impact:

- terminal transcript rendering issues materially fewer text draw calls on uniform-color output
- active buffer search no longer lowercases unchanged visible lines on every repaint
- SDL text-cache hits now avoid one temporary heap string per draw call
- syntax highlighting reduces rule-filter overhead on every highlighted segment

### Startup project-open eager scans

Problem:

- project-open work eagerly refreshed Git sidebar entries even when the active sidebar was not the
  Git view
- `FileFinder::SetIndex()` eagerly consumed `FileIndex::files()`, forcing a full project file scan
  at startup before the file-finder overlay was opened

Implemented:

- `WorkspaceShell::SetProjectRoot` no longer runs synchronous git status collection or
  unconditional full Git sidebar refresh on project open
- directory-tree git badges reuse the async Git working-tree snapshot through
  `DirectoryTree::ApplyGitStatuses()` instead of `CollectGitStatuses` on set-root
- after the first paint on project open, a scoped async `TreeBadges` refresh materializes
  tree markers without collecting outgoing-branch files or blocking startup
- automatic background refreshes use `StatusOnly` scope (branch/dirty metadata for the
  status bar) and skip tree-badge materialization
- opening Source Control still triggers a full async refresh, including outgoing entries
- `FileFinder` defers index-cache materialization until a real file-finder query refresh is
  requested
- project-shell coverage asserts tree badges stay clean synchronously at open, then
  materialize after the first-paint hook without opening Source Control
- project-shell coverage includes file-finder open-and-select behavior with deferred index
  cache build
- perf harness initializes without an implicit project root; editor scenarios open fixtures
  explicitly so unrelated git refresh work does not contaminate allocation measurements

Impact:

- startup traces avoid the eager file-index scan on project open
- on the same local startup-trace command, `WorkspaceShell::SetProjectRoot` dropped from the
  previous `~370 ms` hotspot range to `~17-20 ms`
- tree git markers appear shortly after first paint instead of only after opening Source Control
- Git sidebar behavior remains correct when users switch into the Git view

### Text measurement hot path

Problem:

- `TextRenderer::MeasureWidth()` forwarded every request to the backend
- `TextRenderer::TruncateToWidth()` remeasured growing prefixes linearly
- chrome layout, truncation, blame overlays, and other repeated labels paid the same width cost
  over and over

Implemented:

- width caching inside `TextRenderer`, keyed by string and invalidated when backend or presentation
  scale changes
- logarithmic UTF-8-aware truncation instead of linear prefix probing
- dedicated renderer tests that fail if repeated labels stop hitting the cache or truncation falls
  back to many width probes

Impact:

- repeated UI labels, menu items, tab titles, blame text, and truncation paths now avoid redundant
  backend sizing work

### Terminal event flooding and transcript snapshots

Problem:

- terminal reader threads pushed a wake event for every read chunk
- several shell paths still cloned the full terminal transcript even when they only needed the
  selected rows or the current invocation rows

Implemented:

- terminal wake events are now coalesced until the shell consumes one update
- terminal selection copy, primary-selection sync, last-command transcript capture, and pending
  command capture now snapshot only the needed row ranges
- terminal-session coverage now includes wake-event coalescing

Impact:

- noisy commands generate less SDL event pressure
- large terminal scrollback no longer causes avoidable allocations in the remaining command-copy and
  selection paths

### Retained scene redraws and explicit invalidation

Problem:

- the app previously repainted the whole shell directly to the window backbuffer on every redraw
- caret blink ticks paid for a full shell render even though only one small visual region changed
- most handled UI events still implicitly fell back to full-scene redraws because redraw ownership
  lived in the app loop instead of the shell

Implemented:

- `Application` now keeps a retained scene texture for the shell
- redraws can target only a clipped dirty rect on that texture
- caret-blink updates now repaint only the active editor or terminal caret rect instead of the full
  shell
- the app-shell event contract now carries handled state plus redraw invalidation
- `WorkspaceShell` now owns redraw requests for chrome, overlay, prompt, sidebar, editor, and
  bottom-panel surfaces instead of forcing the app to guess

Impact:

- the common idle animation path now does materially less work
- menu hover, prompt interactions, editor typing, terminal input, terminal wake updates, and
  similar high-frequency paths now stay on the retained-scene partial redraw path
- redraw ownership is explicit instead of heuristic
- active editor typing now repaints only the focused editor pane, terminal typing repaints only the
  panel content area, and terminal wake updates stay on a bottom-panel partial redraw instead of
  falling back to the full window

### Narrower compare and merge invalidation

Problem:

- compare and merge editor interactions still tended to invalidate the full editor surface even when
  only the editable or result pane changed
- narrowing redraws too aggressively can be incorrect when dirty-state indicators or terminal tab
  titles also change

Implemented:

- compare keyboard navigation inside the editable right pane now redraws only that pane when the
  historical left pane does not need to change
- merge result-pane keyboard navigation now redraws only the result viewport instead of the whole
  merge surface
- normal editor and compare or merge edit paths that can toggle dirty state now request the tab
  strip separately so tab indicators stay correct without unioning pane redraws into a much larger
  bounding box
- terminal wake updates intentionally remain bottom-panel wide because shell output can still change
  terminal tab titles

Impact:

- compare and merge navigation stay on a narrower redraw path without regressing correctness
- dirty-state indicators remain explicit and correct instead of being refreshed incidentally by
  over-broad invalidation

### Multi-rect retained redraws and row-band invalidation

Problem:

- a single union dirty rect was still too coarse once the shell started invalidating narrow editor
  bands and disjoint chrome areas explicitly
- tab-strip dirty indicators and row-local editor redraws could only be represented as one bounding
  box, which erased the locality benefit
- compare row selection and merge conflict selection still repainted larger surfaces than needed

Implemented:

- `RenderInvalidation` now carries a small set of dirty rects instead of collapsing every event
  into one bounding box
- the retained-scene renderer now replays shell rendering once per dirty clip rect before
  presenting, so disjoint updates stay disjoint
- retained partial redraw clips now grow by a small font-derived bleed margin so tight caret or
  row-band invalidations do not cache clipped glyph fringes at the dirty-rect edge
- normal editor edits now invalidate the affected line band, or the changed line to the bottom of
  the active pane when line insertion or deletion shifts everything below it
- editor dirty-state transitions also invalidate the local blame-shadow neighborhood so stale
  inline blame text is cleared when a clean tracked buffer becomes dirty
- compare selection changes now invalidate only the affected row bands, and compare edits redraw
  only the changed rows or the changed row-to-bottom region when row alignment shifts
- merge selection and hover changes now invalidate only the affected conflict bands, while merge
  result edits redraw from the changed line to the bottom of the merge surface when downstream rows
  can shift

Impact:

- explicit dirty-state chrome updates no longer force bounding-box redraws through unrelated panes
- editor, compare, and merge interactions now keep more updates on narrow row-band paths
- the retained redraw model is now expressive enough to stay correct without broadening the
  semantic dirty regions that higher-frequency paths depend on

### Correct ASCII text rendering

Problem:

- the per-glyph ASCII shortcut in `SdlTtfTextBackend` did reduce some `SDL_ttf` work, but it also
  reimplemented glyph placement badly enough to corrupt editor identifiers and prompt text
- code like `function resolveInputPath(...)` could render with visibly wrong intra-word spacing,
  clipped stems, or uneven gaps between neighboring glyphs

Implemented:

- `SdlTtfTextBackend::DrawString` and `DrawStringOn` now always use the proper whole-string
  `SDL_ttf` rendering path instead of composing ASCII text glyph-by-glyph
- the shared rendered-string cache was expanded so backing out the glyph shortcut does not
  immediately regress every hot text path into a cache-thrash scenario
- ASCII width measurement now uses fixed-cell width directly instead of calling into `TTF_GetStringSize`
- terminal row rendering now paints visible cell backgrounds before glyphs, coalescing identical
  background runs so prompt text and transcript ASCII cells do not get clipped by the next cell's
  background fill
- retained-scene redraws now preserve block-cursor and other narrow partial redraw transitions by
  padding the clip rect with the backend's measured glyph bleed

Impact:

- editor, compare, merge, and terminal ASCII text now matches `SDL_ttf` layout again instead of an
  approximation
- identifier-heavy code views no longer show the widened or crushed glyph gaps introduced by the
  glyph shortcut
- rendered-string caching still absorbs repeated whole-string draws while a better atlas or batching
  design remains open
- terminal prompt and transcript rendering now preserve glyph edges that extend slightly beyond a
  single fixed cell

### Redraw ownership for view and tab transitions

Problem:

- retained redraws were still relying on whichever input path happened to call a workspace mutation
- direct state changes such as opening a tab, switching the active tab, switching projects, or
  swapping sidebar modes could update shell state without invalidating every affected surface
- some tests and user-visible flows showed the real failure mode clearly: stale tree pixels behind
  the source-control sidebar, or tab-strip labels lagging until another interaction forced a redraw

Implemented:

- sidebar mode transitions now invalidate themselves instead of relying on menu or mouse fallbacks
- active-tab mutations now explicitly invalidate breadcrumb, tab-strip, editor, and tree-sidebar
  surfaces when the active document changes
- compare or merge tab activation and project catalog switches now also own their redraw requests
- retained-render regression coverage now compares partial redraws against clean full redraws for
  sidebar-mode switches and file-open tab transitions

Impact:

- tree clicks, sidebar tool switches, compare or merge tab opens, project switches, and similar
  transitions now repaint immediately under the retained renderer instead of waiting for an
  unrelated event
- redraw ownership is more local to the state mutation, which makes the retained-scene path less
  brittle as more call sites reuse those mutations

## Recent Optimization Pass (2026-04-22)

### Git status collection deferred to on-demand

Problem:

- DirectoryTree::SetRoot called RebuildEntries(true) unconditionally at startup
- This ran `git status --porcelain=v1 -z --untracked-files=all` at startup
- Took 14.10 ms even when the Git sidebar wasn't visible

Implemented:

- DirectoryTree::SetRoot now calls RebuildEntries(false), deferring git status collection
- Added DirectoryTree::RefreshGitStatuses() public method for explicit refresh
- SetProjectRoot calls RefreshGitStatuses when Git sidebar mode is active
- SidebarCoordinator::ShowGit() calls RefreshGitStatuses before rendering git sidebar
- Git statuses are now collected only when the Git sidebar is displayed or explicitly refreshed

Follow-up (2026-05-20):

- set-root still avoids synchronous git status collection; tree badges now materialize from a
  scoped async refresh dispatched after the first paint on project open
- `ShowGit()` and explicit refresh paths request full async Git sidebar snapshots; automatic
  background refreshes stay status-only until tree badges are materialized

Impact:

- Application::Initialize: 82.76 ms → 56.90 ms (31% improvement)
- Total startup to FirstRender: 99.39 ms → 71.49 ms (28% improvement)
- Removed 14.10 ms from startup critical path
- DirectoryTree::SetRoot: 16.18 ms → 1.45 ms

### Startup LSP prewarm deferred on no-syntax plugin reload

Problem:

- startup project restore uses `ReloadPluginsForCurrentProject(false)` to skip syntax-definition
  rebuilds, but still called `NotifyPluginsAboutOpenBuffers`
- that path called `LspClientForViewport` for restored buffers, which ran full runtime-syntax
  language detection and could also start LSP servers during startup
- this showed up as a startup hotspot in traces (`NotifyPluginsAboutOpenBuffers` dominated by
  `LspClientForViewport`, around `~22-30 ms` on local headless runs)

Implemented:

- `NotifyPluginsAboutOpenBuffers` now accepts an `open_lsp_documents` toggle
- `ReloadPluginsForCurrentProject(false)` keeps plugin `on_buffer_open` hooks but skips LSP
  document prewarm
- `LspManager` now exposes `HasRegisteredServers`, and `LspClientForViewport` exits early when no
  LSP servers are registered
- open-buffer iteration no longer eagerly opens deferred `needs_restore` views just to emit buffer
  notifications

Impact:

- on the same local startup trace command, `NotifyPluginsAboutOpenBuffers` dropped from
  `~22-30 ms` to `~0.05 ms`
- `WorkspaceShell::ReloadPluginsForCurrentProject` dropped from `~23-32 ms` to `~2.10 ms` on the
  no-syntax startup path
- sampled `Application::Initialize` dropped from `39.51 ms` to `10.51 ms`
- LSP behavior remains correct on demand (file open and LSP command paths), and targeted plugin/LSP
  regression tests pass

### Identified remaining startup bottleneck: syntax definition reloading

Diagnostic traces added to WorkspacePluginRuntime::Reload show that ReloadDefinitions is the
primary remaining bottleneck, accounting for 44.27 ms (78% of total plugin reload time).

The BuildRegistry function rebuilds the entire syntax registry for all plugin-provided syntax
definitions on every startup. This is still a necessary operation when plugins or definitions
change, but opportunities exist to optimize further through:

- Caching compiled syntax definitions to disk
- Only reloading syntax if definitions actually changed
- Deferring syntax reload to after first render (if syntax highlighting isn't immediately needed)
- Parallelizing syntax definition processing across multiple definitions

Relevant code:

- `src/editor/RuntimeSyntaxRegistry.cpp` (BuildRegistry, ReloadDefinitions)
- `src/workspace/WorkspacePluginRuntime.cpp` (Reload)

### Plugin syntax reloads now skip unchanged rebuilds

Problem:

- even after earlier startup work, plugin runtime reload still rebuilt runtime syntax definitions
  whenever syntax reloads were enabled, even if the discovered `syntax/*.lua` files were identical
  to the previous load
- that made project open and `plugins-reload` pay Lua parsing plus syntax-registry rebuild cost on
  every syntax-enabled reload path

Implemented:

- `SyntaxDefinitionLoader` now computes a stable source fingerprint over discovered syntax
  definition files
- `WorkspacePluginRuntime` now caches that fingerprint and skips syntax-definition load and
  registry rebuild entirely when the syntax sources are unchanged
- `WorkspaceShell` now invalidates editor, compare, and merge syntax state only when the runtime
  syntax registry actually changed
- plugin reload regression coverage now verifies an unchanged syntax reload does not bump the
  runtime syntax registry revision

Impact:

- repeated startup or `plugins-reload` paths no longer pay syntax-registry rebuild cost when
  plugin syntax files have not changed
- cold syntax loads and real syntax edits remain the main remaining startup-sensitive path

### Cold plugin syntax reloads reuse the generated registry

Problem:

- when plugin syntax definitions did change, `BuildRegistry` still recompiled every generated
  built-in syntax regex before appending the small set of plugin definitions
- that made real plugin syntax edits pay the full generated-registry PCRE2 compile cost even
  though the generated snapshot is immutable for the lifetime of the process

Implemented:

- `RuntimeSyntaxRegistry` now builds the generated syntax registry once, partitions its rules once,
  and copies it into new runtime registries on cold plugin syntax reloads
- `CompiledRegex` now uses shared ownership for compiled PCRE2 code, so copying generated rules and
  definitions does not recompile regexes or duplicate compiled regex storage
- plugin syntax definitions still compile fresh on real changes, then the cached generated registry
  is appended with corrected rule and definition offsets
- regex regression coverage now verifies copied compiled patterns remain matchable and can create
  independent match data

Impact:

- first-load or real-change plugin syntax reloads now compile only plugin syntax plus cheap generated
  metadata copies instead of recompiling the entire built-in syntax snapshot
- remaining syntax-load work is dominated by plugin Lua parsing and plugin regex compilation; disk
  caching or parallel plugin parsing should only be promoted if profiling still shows material cost

## Second Performance Pass (2026-04-23)

Static analysis sweep across all render-path source files after the first deep-dive fixes shipped.
None of these have been measured in a live trace yet — treat the new findings as a prioritized
investigation queue alongside the still-open item from the first pass.

### Status of first-pass findings

All nine of the Deep-Dive Findings items that were actionable are now confirmed fixed:

- Items 1–5 (PCRE2 thread-local match data, FindAllRegex output buffer, lazy highlight
  checkpoints, partial cache invalidation from edit line, caret column removed from cache key):
  code verified in `RuntimeSyntaxRegistry.cpp` and `TextViewport.cpp`/`TextViewport.h`.
- Items 7, 8, 9, 10, 11, 13 (output-channel snippet caching, terminal foreground run coalescing,
  buffer-search match caching, SDL text cache heterogeneous lookup, syntax rule pre-partitioning,
  from_chars for output-line numeric parsing): code verified in respective files.

Item 6 (`SnapshotLineRangeCached` generation counter) is now fixed — see below.
Item 12 (`optional<SyntaxState>` memory reduction) is now fixed.

### Fixed from first pass: `SnapshotLineRangeCached` generation counter

Status: fixed.

`TerminalSession` now maintains a `uint64_t snapshot_generation_` counter and exposes
`SnapshotLineRangeIfChanged`. The bottom-panel render path keeps the active terminal tab's last
visible line snapshot and skips the deep copy whenever both the visible range and terminal
generation are unchanged. Idle terminal frames now reuse the previous `TerminalLine` vector.

Implemented:

- Writer paths advance `snapshot_generation_` after output, resize, reset, alternate-screen
  restore, stop cleanup, and process-exit marker changes.
- `TerminalTabState` stores the last visible terminal line snapshot plus the first row and visible
  row count, so range-only changes can force a refresh without pretending content changed.
- Regression coverage verifies unchanged generations skip the copy and output changes refresh the
  snapshot.

Relevant code:

- `src/terminal/TerminalSession.h` — `TerminalLineRangeSnapshot`,
  `SnapshotLineRangeIfChanged`
- `src/terminal/TerminalSession.cpp` — `snapshot_generation_`, writer invalidation
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — cached visible terminal lines

### New finding 1: `WorkspaceReviewComments` O(visible_lines × comments) per-frame scan (HIGH)

Status: fixed.

`GetThreads(uri)` in `WorkspaceReviewComments.cpp` does a linear scan through all stored threads
and returns a new allocated vector on every call. `GetComments(uri, line_index)` does a linear
scan through all stored comments on every call. The render path in `WorkspaceShellRenderFrame.cpp`
calls `GetThreads` once per editor pane per frame to find marked lines, then — if no thread
markers are present for a line — calls `GetComments` for every visible line. When review comments
are active this is O(visible_lines × total_comments) per frame.

Fix: build per-URI line→thread and line→comment index maps inside `WorkspaceReviewComments` and
invalidate them only on `AddThread`, `AddComment`, `UpdateThread`, `DeleteThread`, and
`ClearForUri`. The render path then does O(1) lookups per line instead of O(total_comments) scans.

Implemented:

- `ReviewCommentsRegistry` now builds per-URI indexes for threads and comments grouped by line.
- Render-time review markers use `HasThreads(uri, line)` and `HasComments(uri, line)` instead of
  allocating vectors or scanning all comments per visible line.
- Regression coverage verifies the URI/line index updates after state changes and removals.

Relevant code:

- `src/workspace/WorkspaceReviewComments.cpp` — `GetThreads`, `GetComments`
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda
- `guidelines/tech-debt/archive/2026-05-01-render-and-layout-perf-batch.md` — §8

### New finding 2: `ComputeEditorPaneLayouts` called twice per render frame (MEDIUM)

Status: fixed.

`WorkspaceShellRenderFrame.cpp` calls `ComputeEditorPaneLayouts(layout.editor_surface)` twice per
frame — once during the main editor render pass and again during the scrollbar render pass. The
function recomputes pane geometry from scratch each time.

Fix implemented: the editor pane layout is computed once near the top of
`RenderActiveWorkspaceSurface` and reused by both the main editor render pass and scrollbar pass.
No caching infrastructure was needed.

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp` — two separate `ComputeEditorPaneLayouts` calls
- `guidelines/tech-debt/archive/2026-05-01-render-and-layout-perf-batch.md` — §9

### New finding 3: Terminal cursor state acquired under three separate mutex locks per frame (MEDIUM)

Status: fixed.

The terminal render path calls `cursor_row()`, `cursor_column()`, and `cursor_visible()` as
separate methods, each of which acquires and releases the `TerminalSession` mutex independently.
This is three mutex lock/unlock cycles on the render thread per frame when the terminal is visible,
where one combined snapshot call would suffice.

Fix implemented: `CursorSnapshot()` captures `{cursor_row, cursor_column, cursor_visible}` under a
single lock and returns a plain struct. Terminal render, caret invalidation, and pending-input
submission now use that snapshot instead of separate cursor accessors.

Relevant code:

- `src/terminal/TerminalSession.h` — `cursor_row()`, `cursor_column()`, `cursor_visible()`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — terminal cursor render path
- `guidelines/tech-debt/archive/2026-05-01-render-and-layout-perf-batch.md` — §10

### New finding 4: `std::find` on `marked_lines` vector in `draw_review_comment_markers` (MEDIUM)

Status: fixed.

The `draw_review_comment_markers` lambda in `WorkspaceShellRenderFrame.cpp` calls
`std::find(marked_lines.begin(), marked_lines.end(), one_based_line)` for each visible line.
If M lines have markers and N lines are visible, this is O(N × M) per frame. With typical screen
heights and a populated code review this is hundreds of comparisons per frame for a simple
membership test.

Fix: replace `marked_lines` with an `std::unordered_set<std::size_t>` or sort-plus-binary-search
so the per-line lookup is O(1) or O(log M) instead of O(M). The set can be built once per frame
from `GetThreads()` output (after finding 1 is addressed, this becomes trivially cheap).

Implemented:

- The marker renderer no longer builds `marked_lines`; it performs one indexed thread lookup and
  one indexed comment lookup per visible line.
- This removes both the per-frame marked-line vector allocation and the per-visible-line
  `std::find` membership scan.

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda
- `guidelines/tech-debt/archive/2026-05-01-render-and-layout-perf-batch.md` — §11

## Deep-Dive Findings (2026-04-23)

This section records bottlenecks found by static code review across all hot paths. None of these
have been measured in a live trace yet — treat them as a prioritized investigation queue rather
than confirmed numbers.

### 1. `CreateMatchData` malloc on every PCRE2 match (CRITICAL — render hot path)

Every call to `FindFirstRegex` and `FindAllRegex` in `RuntimeSyntaxRegistry.cpp` calls
`pattern.CreateMatchData()` which maps directly to `pcre2_match_data_create_from_pattern` — a
heap allocation. These functions are called for every rule on every visible line during syntax
highlighting. With ~50 visible rows and dozens of pattern rules per definition, this is hundreds
of malloc/free cycles per frame just for match data.

Fix: Use a thread-local `RegexMatchData` per compiled pattern. The match data is only used on the
calling thread and can be re-used across calls without locking. This eliminates the allocation
entirely for the fast (cache-hit) path.

Relevant code:

- `src/editor/RuntimeSyntaxRegistry.cpp` — `FindFirstRegex`, `FindAllRegex`
- `src/util/RegexUtil.h` — `CompiledRegex::CreateMatchData`

### 2. `FindAllRegex` heap-allocates a vector per rule per segment (CRITICAL — render hot path)

`ApplyPatternRules` calls `FindAllRegex` which returns a `std::vector<MatchRange>` by value for
every pattern rule on every text segment. This triggers a heap allocation for every rule-segment
combination on every visible line per frame.

Fix: Pass an output `std::vector<MatchRange>&` parameter (cleared before use) so callers can
reuse a single pre-allocated buffer across all calls on one line, or use a thread-local match
buffer.

Relevant code:

- `src/editor/RuntimeSyntaxRegistry.cpp` — `FindAllRegex`, `ApplyPatternRules`

### 3. `EnsureHighlightCheckpoints` blocks the render thread on first access (CRITICAL — typing / file-open)

`InvalidateDerivedCaches` (called on every edit) clears all syntax checkpoints. The next render
call triggers `EnsureHighlightCheckpoints`, which synchronously advances through every line in
the document to rebuild checkpoints. For a 10,000-line file at the current `kHighlightCheckpointInterval`
of 128, this is ~78 full `AdvanceState` passes, each running PCRE2 matches against a full line.
This blocks the first frame after every edit.

Fix: Build checkpoints lazily — only advance to the checkpoint that covers the first visible line,
not the entire document. Checkpoints further down can be built incrementally as the user scrolls.
A simpler short-term fix is to only invalidate checkpoints from the edited line forward rather
than clearing the entire array on every mutation.

Relevant code:

- `src/editor/TextViewport.cpp` — `EnsureHighlightCheckpoints`, `InvalidateDerivedCaches`
- `src/editor/TextViewport.h` — `kHighlightCheckpointInterval = 128`

### 4. `InvalidateDerivedCaches` does a full clear on every keystroke (HIGH — typing latency)

Any edit calls `InvalidateDerivedCaches()`, which clears all 256 highlight cache entries, all 256
visible-line cache entries, all per-line syntax states, and all checkpoints. For large files, the
next render has to rebuild caches from scratch for the full visible region.

Fix: On range edits, only invalidate caches at or after `range.start.line`. Lines before the edit
point are unaffected and their caches remain valid. This requires passing the edit start line into
`InvalidateDerivedCaches` and flushing only the relevant tail of each cache structure.

Relevant code:

- `src/editor/TextViewport.cpp` — `InvalidateDerivedCaches`, `ApplyHistoryEntry`

### 5. `VisibleLineCacheKey` includes `caret_text_column` causing excess cache misses (HIGH — cursor movement)

The cache key for `VisibleLineLayout` includes `caret_text_column`, which differs on every
horizontal cursor movement. The actual text layout (text, source_columns, text_offsets) does not
depend on the caret position. Only `caret_visible` and `caret_column` in the `LayoutLine` depend
on the caret. This means every left/right arrow key causes a cache miss for the current line, even
though the rendered text is identical.

Fix: Separate caret computation from text layout. `BuildVisibleLine` should return only the text
layout; caret visibility and column can be computed separately at render time from the same inputs
without a cache lookup. This lets the cache key drop `caret_text_column` entirely.

Relevant code:

- `src/editor/TextViewport.h` — `VisibleLineCacheKey`, line 158–180
- `src/editor/TextViewport.cpp` — `VisibleLineLayout`, line 461
- `src/editor/TextLayout.cpp` — `BuildVisibleLine`

### 6. Terminal `SnapshotLineRange` deep-copies lines every frame (HIGH — terminal render)

Status: fixed on 2026-04-23.

Every frame that renders the terminal panel calls `SnapshotLineRange`, which acquires the mutex
and deep-copies all visible `TerminalLine` objects. Each `TerminalLine` contains a
`std::vector<TerminalCell>`, so for 40 visible rows at 200 columns each, this is 8,000 cell
copies plus 40 vector copies per frame, even when the terminal has been idle.

Fix: added a generation counter incremented by terminal writer paths. The render thread checks
whether the generation and visible range changed since the last snapshot; if not, it reuses the
previous frame's terminal lines without copying. This makes the common idle-terminal case
allocation-free.

Relevant code:

- `src/terminal/TerminalSession.cpp` — `SnapshotLineRange`, line 705
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — line 269

### 7. Output panel calls `HighlightLine` on every visible line every frame (HIGH — output panel)

Status: fixed.

When the output panel shows code-context snippets, line 314 of `WorkspaceShellRenderBottomPanel.cpp`
calls `editor::runtime_syntax::HighlightLine` on every visible code snippet every frame. This
runs the full PCRE2 regex highlighter per line per frame even when the output hasn't changed.

Fix: `WorkspaceOutputChannels` parses context snippets on append and stores a per-entry
highlight cache. The render path asks `HighlightedContextSnippet` for cached tokens; highlighting
is only recomputed when the snippet has not been highlighted yet or the resolved path changes.
`Clear` drops the parsed metadata and cached tokens with the channel entries.

Relevant code:

- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — line 314
- `src/workspace/WorkspaceOutputChannels.*`

### 8. Terminal foreground rendering is per-cell rather than per-run (MEDIUM — terminal render)

The terminal cell renderer (line 183 of `WorkspaceShellRenderBottomPanel.cpp`) loops over every
cell and calls `DrawString` for each non-space character. Backgrounds are already coalesced into
runs, but foreground text is not. A line of 200 ASCII characters with the same foreground color
produces 200 `DrawString` calls instead of one.

Fix: Apply the same run-coalescing logic used for backgrounds to foreground rendering. Build a
contiguous text string for each run of cells sharing the same foreground color and draw the whole
run in a single `DrawString` call. This is especially impactful for terminal output that is
predominantly one color.

Relevant code:

- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — `draw_terminal_line` lambda, line 156–194

### 9. Buffer search lowercases every visible line every frame (MEDIUM — editor render)

When buffer search is active, `EditorViewRenderer::Render` lowercases every visible source line
and searches it on every frame, even when neither the query nor the document has changed. For 50
visible rows of 200 characters each, this is 10,000 characters lowercased and scanned per frame.

Fix: Cache search match ranges per line, keyed by (line_index, document_revision, query). Only
recompute when the query or document revision changes. The hit-testing already uses a sorted match
list; the rendering can use the same list.

Relevant code:

- `src/editor/EditorViewRenderer.cpp` — `Render`, line 312–347

### 10. `SdlTtfTextBackend::BuildCacheKey` allocates a `std::string` per draw call (MEDIUM — render)

Every call to `DrawString` or `DrawStringOn` allocates a `std::string` via `BuildCacheKey` before
doing the cache lookup. For a frame with 3,000 text draw calls, this is 3,000 temporary string
allocations even when every call is a cache hit.

Fix: Use a heterogeneous hash lookup with a compound key struct (pointer+length, color bytes) so
the cache lookup can proceed from a stack-allocated key without ever allocating a `std::string`.
This requires replacing `std::unordered_map<std::string, ...>` with a custom-hashed map that
accepts a string-view-like key for lookups.

Relevant code:

- `src/render/SdlTtfTextBackend.cpp` — `BuildCacheKey`, `ResolveEntry`

### 11. `ApplyPatternRules` iterates all rules to find pattern rules for a region (MEDIUM — syntax)

For every text segment in every line, `ApplyPatternRules` loops over all rules in the definition
(`definition.rule_count` can be dozens) and skips any that don't match `parent_region_id` or
aren't pattern rules. This is O(all_rules) filtering per segment.

Fix: Pre-partition the rule list by parent_region_id at registry-build time. Store per-region
rule index ranges so `ApplyPatternRules` and `FindEarliestRegionStart` can iterate only the
relevant subset.

Relevant code:

- `src/editor/RuntimeSyntaxRegistry.cpp` — `ApplyPatternRules`, `FindEarliestRegionStart`

### 12. `line_highlight_states_` uses `optional<SyntaxState>` (LOW — memory)

Status: fixed on 2026-04-23.

The per-line state cache uses `std::vector<std::optional<SyntaxState>>`. Each `optional` adds
a bool + padding, making each element ~24 bytes on 64-bit. For a 10,000-line file this is ~240KB
just for this vector. The "uncached" sentinel can instead be `SyntaxState{definition_id=0}`,
collapsing to a plain `std::vector<SyntaxState>` at ~16 bytes per entry (~160KB).

Fix: `line_highlight_states_` and `highlight_checkpoints_` now store plain `SyntaxState` values.
`SyntaxState{}` is the uncached sentinel, and cached syntax definitions use non-zero
`definition_id` values.

Relevant code:

- `src/editor/TextViewport.h` — `line_highlight_states_`, line 257

### 13. `ParseUnsignedStrict` allocates a string for `std::stoull` (LOW — output panel)

Line 41 of `WorkspaceShellRenderBottomPanel.cpp`:

```cpp
const unsigned long long parsed = std::stoull(std::string(text), &parsed_length);
```

This allocates a temporary string every time an output line is checked for a numeric field.
`std::from_chars` does the same work without allocation and is available in C++17.

Relevant code:

- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — `ParseUnsignedStrict`, line 35

## Still Worth Doing

### Syntax definition reloading optimization

With git status deferral complete, syntax definition reloading (44.27 ms) is now the primary
startup bottleneck. Recommended optimizations:

- Cache compiled/indexed syntax definitions to disk to avoid re-parsing on every startup
- Compare plugin definition checksums to skip reload when definitions haven't changed
- Defer full syntax reload to after first render if file syntax highlighting isn't immediately needed
- Parallelize syntax definition indexing across multiple worker threads

Relevant code:

- `src/editor/RuntimeSyntaxRegistry.cpp` - BuildRegistry, ReloadDefinitions
- `src/workspace/WorkspacePluginRuntime.cpp` - Reload

### Finer-grained surface invalidation

Dirty-rect ownership is now explicit and the retained-scene path supports multiple disjoint dirty
rects, but there is still room to make individual surfaces cheaper. The highest-value remaining UI
work is now:

- bottom-panel updates that repaint less than the full content area when only one row or caret
  changed
- finer-grained row or token invalidation inside merge and compare editing paths that still redraw
  from the changed row to the bottom when downstream rows may shift
- broader measurement of whether the remaining repaint cost is now dominated by text rendering
  rather than scene invalidation

Relevant code:

- `src/workspace/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellInput.cpp`
- `src/workspace/WorkspaceTabCoordinator.cpp`
- `src/workspace/WorkspaceSidebarCoordinator.cpp`
- `src/workspace/WorkspaceCompareInteractionCoordinator.cpp`

### Lower-cost text rendering backend

The new ASCII glyph cache is a good middle step, but it is still not a full atlas-backed text
renderer. A more complete glyph-atlas or batched text path is still a good next step if text
rendering remains measurable after this pass.

Relevant code:

- `src/render/TextRenderer.cpp`
- `src/render/SdlTtfTextBackend.cpp`

### Profiling discipline

The startup tracer exists, and redraw tracing can now be enabled with `MICROIDE_TRACE_REDRAW=1`, but
broader redraw and idle profiling still needs to be done regularly before and after rendering work.

Relevant docs:

- `dev-docs/performance/startup-tracing.md`

## Recent LSP Optimization Pass

Problem:

- Opening a TypeScript project caused noticeable delay at startup
- UI would freeze momentarily when using LSP features (e.g., find references) for the first time
- LSP server initialization was synchronous and blocked the main thread waiting for the
  initialize/initialized handshake

Implemented:

- `LspClient::Start()` now launches server initialization asynchronously on a background thread
- Process starts immediately, but capability negotiation happens in the background
- Reader thread starts after initialization completes to avoid race conditions with the
  initialization thread
- Query methods (hover, completion, find references, etc.) check `IsInitialized()` and only send
  requests after the LSP spec's required initialization handshake completes
- Added comprehensive startup/performance traces for `LspClient::Start`, initialization phases,
  and callback processing

Impact:

- UI is no longer blocked during LSP server startup (e.g., TypeScript Language Server takes 1-3s
  to start)
- Startup to first render remains unblocked at ~432 ms (plugin loading dominates at ~230 ms)
- LSP queries fail gracefully if the server hasn't initialized yet, rather than crashing
- Trace spans: `LspClient::Start`, `LspClient::Start::StartProcess`, `LspClient::DoInitializeBlocking::WaitInitializeResponse`,
  `LspManager::GetServer::InitializeServer`, `LspManager::DrainCallbacks`, `LspClient::DispatchResponse`

Relevant code:

- `src/workspace/WorkspaceLspClient.cpp` - async initialization and query synchronization
- `src/workspace/WorkspaceLspManager.cpp` - server lifecycle management
- `src/workspace/WorkspaceShellTooling.cpp` - LSP query methods

## Throughput Bottleneck Performance Pass (2026-05-13)

Captured under the `throughput-bottleneck-performance-pass` OpenSpec change after harness isolation made local advisory numbers comparable. All numbers below are p50 wall time from 10-iteration isolated dummy-driver runs; baseline updates wait for `perf-runner-v1`. Full ledger: `openspec/changes/throughput-bottleneck-performance-pass/perf-ledger.md`.

### Harness isolation (`§1`)

Problem:

- `PerfHarness::InitializeDriver` set `XDG_CONFIG_HOME` but left `XDG_STATE_HOME`, `XDG_CACHE_HOME`, and `XDG_DATA_HOME` pointing at the developer's home directory. Scenarios like `cold_startup_no_project` restored a real 50k-line editor session and `terminal_scroll_long_output` paid 1.4 s for unrelated `WorkspaceRootView::Render` work on that restored editor.

Implemented:

- Per-process sandbox under `temp_directory_path()/microide-perf-<pid>[-<rand>]` with `config/state/cache/data` subdirectories, all four `XDG_*_HOME` env vars exported, sandbox cleanup at shutdown unless `--keep-artifacts` is passed.
- `--keep-artifacts` CLI flag for triage; harness reports include runner-class, provenance, video/renderer driver, scenario list, iteration count, layout mode, seed, and sandbox status metadata.
- Regression unit test `PerfHarnessIsolation/ColdStartupIgnoresRealUserSession` plants a real `~/.local/state/microide/workspace-session` and asserts the harness's `AppDirectories::ResolveAppDirectory(State, "microide")` resolves inside the sandbox.

Impact:

- `cold_startup_no_project` max: 359 → 25 ms.
- `terminal_scroll_long_output` p50: 100 → 121 ms (now stable; previously bursty p95 553 ms / max 604 ms from restored editor contamination).

### Indexed FoldingModel lookups (`§2.1–§2.2`)

Problem:

- `FoldingModel::{IsLineHidden,FoldStartingAt,IsCollapsedAtOpener,InnermostFoldContaining}` did linear scans over `ranges_` for every viewport row, so big files with many folds paid O(n_folds × visible_lines) per visible-frame rebuild.

Implemented:

- Revision-keyed lookup cache: sorted collapsed-interval list with prefix `hi`-max for O(log n) `IsLineHidden`, binary search on the already-sorted unique-opener `ranges_` for `FoldStartingAt`/`IsCollapsedAtOpener`, and a prefix `closer_line` running-max for `InnermostFoldContaining`. Cache invalidates implicitly via the existing `revision_` counter.

### Non-soft-wrap viewport row mapping fast path (`§2.3`)

Problem:

- `TextViewport::EnsureWrappedRowLayouts()` called `VisualColumnForTextColumn` per line and queried `IsLineHidden` per line even when no folds were collapsed.

Implemented:

- Probe `folding_model_->collapsed_flags()` once per rebuild; skip per-line `IsLineHidden` calls when no fold is collapsed.
- Non-soft-wrap branch reuses a constant `WrappedRowLayout` template and skips the `VisualColumnForTextColumn` walk entirely.

Impact:

- `editor_fold_recompute` p50: 1165 → 716 ms (−39%).

### Same-line-count `ApplyHistoryEntry` fast path (`§3.1–§3.3`)

Problem:

- `TextViewport::ApplyHistoryEntry()` erased and re-inserted lines through the storage vector even when a history entry replaced N lines with N lines (the common case for ordinary typing and undo). For edits near the top of a 50k-line file this shifted the entire tail per keystroke.

Implemented:

- Same-count fast path: in-place assignment when `removed_count == inserted_lines.size()`. Encoding refresh, derived-cache invalidation, visual-column updates, cursor restoration, and applied-edit metadata are preserved.
- New regression tests `TextViewport/SameLineCountUndoOnLargeFilePreservesContent` and `TextViewport/SameLineCountEditInvalidatesSyntaxCache`.

Impact:

- `editor_auto_close_typing` p50: 3175 → 728 ms (**−77%**, 4.4× speedup).
- `editor_smart_indent_typing` p50: 3246 → 829 ms (**−74%**).
- `editor_shaping_multi_caret` p50: 146 → 36 ms (**−76%**).
- `editor_add_cursor_next_match` p50: 43 → 26 ms (−40%).
- `editor_bracket_match_caret_motion` p50: 108 → 69 ms (−35%).

### Slice-based multi-caret aggregate (`§3.4`)

Problem:

- `TryMultiCaretPairInsert()` copied the full `document_->lines` and diffed it against the post-edit document to build the aggregate undo entry, even when every caret's edit was confined to a small range of lines.

Implemented:

- When `ch != '\n'` and no selection spans multiple lines (the common surround / pair-insert path), snapshot only the line range bounded by the touched carets. Build the aggregate `HistoryEntry` from that slice and offset its `start_line` back into document coordinates. Conservative fallback preserves the full snapshot for newline inserts and multi-line selections.

Impact:

- `editor_surround_multi_caret` p50: 563 → 486 ms (−14%) — full-buffer copy reduced from 50k to ~8k lines for the 8-caret scenario.

### Undo-group child-delta aggregation (`§3.5` / `§3.6`)

Problem:

- `BeginUndoGroup()` still degraded to whole-buffer snapshotting for grouped edits that changed line count, which left snippets and grouped completions paying an O(document size) history cost even after the same-count and multi-caret work landed.

Implemented:

- Undo groups now accumulate known-range child `HistoryEntry` deltas and merge them into one aggregate undo entry when the child edits stay within the evolving changed slice.
- A documented conservative fallback reconstructs the pre-group buffer only when grouped child edits are disjoint or otherwise cannot be normalized into one contiguous aggregate entry.
- New regression tests `TextViewport/UndoGroupMergesKnownRangeChildEdits`, `TextViewport/UndoGroupFallsBackForDisjointChildEdits`, plus snippet undo-group coverage (`EditorSnippet/ExpansionSingleUndoStep`, `EditorSnippet/MultiOccurrenceLinkedTab`).

### Manual real-window trace (`§4.8`)

User verification on 2026-05-13:

- `env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=5 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide`

Observed:

- First real-window `WorkspaceRootView::Render` still took **2514.65 ms** while restoring `synthetic_kernel.cpp` from the `switch_project_b` fixture.
- The supporting startup path remained modest by comparison: `RuntimeSyntaxRegistry::EnsureInitialized` 44.07 ms, `TextViewport::OpenFile` 24.26 ms, `WorkspaceShell::InitializeCurrentProject` 42.10 ms.
- Once the first heavy frame completed, repeated `Application::WorkspaceRender(fallback-full)` samples mostly landed around **14-18 ms**, with intermittent spikes into **27-35 ms**.

### Still pending in this pass

- No remaining local implementation tasks. The authoritative `perf-runner-v1` gate was intentionally skipped for this archive because runner usage was unavailable, and no perf baselines were updated.

## Plugin-Rendering Zero-Cost Verification (2026-06-26)

Goal: confirm `feat/plugin-rendering` carries **no** measurable cost over `main` when no plugin
is loaded (or no plugin uses a given presentation capability), matching the debugger's
gated-behind-`DebugEnabled()` model. This was the merge blocker for the branch.

### Changes made before measuring

- One master gate in `WorkspaceShellRenderFrame.cpp`: the three `plugins.*` inset feature-flag
  setting reads now run only when `plugin_presentation_if_present() != nullptr`, so a no-plugin
  frame reads zero `plugins.*` settings (was three per frame). Mirrors the `DebugEnabled()` idiom.
- Dedup: `util::SamePathNormalized` (`src/util/PathMatch.h`) replaces three copy-pasted
  raw-then-`lexically_normal()` path matches; `editor::VisualRowInWindow` replaces two open-coded
  window-membership tests and is now shared with the render builder's ghost-tail check; removed the
  production-dead `EditorRowYLayout::WindowHeight`. The debug execution-line match now reuses
  `SamePathNormalized`, dropping two per-frame `generic_string()` allocations on stopped-debug frames.

### Evidence (branch incl. cleanups vs `main`, zero plugins loaded)

- **Gated zero-allocation tests** (`microide-perf` build, `MICROIDE_PERF_HARNESS_BUILD=1`):
  `microide_tests PluginPresentation` → 14/14 pass with heap-delta assertions active, confirming the
  presentation bundle is lazily allocated and released and the empty-store/empty-host resolve paths
  allocate nothing. Added `RenderViewModelBuilder/InsetGapsEmptyWithoutPluginPresentation` locking in
  the consumer-side early-out the master gate relies on (null bundle ⇒ empty `row_gaps`/no ghost tail
  regardless of feature flags).
- **`tools/perf-compare.py`** over `typing_small_file`, `typing_large_file`, `scroll_large_file`,
  `large_file_open_first_paint`, `cold_startup_no_project`, `cold_startup_small_project`,
  `repo_open_rss_idle`, `idle_soak_30s`: **"No metric regressed beyond the 2σ noise band."** Per-frame
  wall and allocation counts on the editing/scroll scenarios, cold-startup wall, idle wake behavior,
  and steady-state RSS all sit within run-to-run noise of `main` (scattered p50-allocation deltas pair
  with flat/lower p95/max — the signature of variance, not a systematic regression).
- **Startup trace** (`MICROIDE_STARTUP_TRACE=1 SDL_VIDEODRIVER=dummy`): the only plugin scopes are the
  pre-existing runtime reload (`PluginHost::Reload`/`PluginRuntime::Reload`/
  `ReloadPluginsForCurrentProject`, also present on `main`). **None** of the new rendering features
  (insets, ghost text, decorations, surfaces) introduce a startup scope.
- Full `ctest` suite, ASAN, and UBSAN all clean (`/tmp/microide-{tests,asan,ubsan}.log`).

### Method note / pending

- The interactive runtime profile (`MICROIDE_PERF_TRACE` during a live typing session) was covered
  instead by the reproducible `typing_*`/`scroll_large_file` perf-compare scenarios, which exercise the
  same per-frame editor render path headlessly. The authoritative `perf-runner-v1` gate was not run
  (runner unavailable); no perf baselines were moved — consistent with "no regression on the no-plugin
  path." Local dummy/Xvfb numbers above are advisory per `perf-harness.md`.

## Deep First-Paint Freeze Fix (2026-06-27)

Closes the **2514 ms** first real-window `WorkspaceRootView::Render` recorded in the
Throughput pass (§4.8 above) when restoring `synthetic_kernel.cpp` deep-scrolled.

Root cause: `TextViewport::HighlightStateBeforeLine` replayed the syntax-state
checkpoint chain from line 0 to the visible line **synchronously on the main
thread**. A top-of-file open replays ~0 lines (the existing
`large_file_open_first_paint` scenario stays ~25 ms p50), but a session restore
scrolled deep into a large file replayed tens of thousands of lines before the
first frame. The harness had no deep-restore scenario, so the freeze was invisible
to the gate.

Fix:

- New harness scenario `large_file_restore_deep_scroll_first_paint` reproduces it
  (jump to line 45 000 in the 50k C++ fixture with a cold highlight cache, inside
  the measured region). Sibling `mid_file_edit_latency_large_file` is the oracle
  for the later piece-tree migration.
- `HighlightStateBeforeLine` now caps synchronous replay: when the exact resume
  state is more than `kMaxSyncHighlightReplayLines` (512) past the nearest valid
  checkpoint, it returns the nearest checkpoint's state as an approximation
  (marking the result inexact so `HighlightedLineTokens` does not promote it to the
  authoritative per-line cache) and arms an off-thread checkpoint backfill.
- `HighlightPrefetchService::RequestCheckpoints` + `ComputeHighlightCheckpoints`
  replay the chain segment on the existing background worker (sparse output: one
  `SyntaxState` per 128-line checkpoint, not per-line tokens). The main thread folds
  results in via `InstallHighlightCheckpoints`, which clears the small token cache so
  approximate deep-jump tokens recompute exactly on the next repaint. Convergence is
  chunked across repaints (16 384 lines/chunk).

Evidence (advisory, `SDL_VIDEODRIVER=dummy`):

- `large_file_restore_deep_scroll_first_paint` measured phase
  `deep_restore.jump_and_first_paint`: **~360 ms → ~3.4 ms** (~106×); p50 allocations
  257 629 → 122 418.
- Correctness: new tests `TextViewport/CheckpointBackfillConvergesToExactMultilineState`
  and `.../HighlightCheckpointBackfillServiceRunsOnWorkerThread` assert the backfill
  converges to exact multiline (block-comment) state at depth; existing
  `HighlightCheckpointsBoundFarReplay` / `EditingNearTailDoesNotRebuildFarCheckpoints`
  updated for the bounded-sync + async-converge model.
- Full `ctest` green; ASAN clean; TSAN clean on the threaded backfill path
  (`setarch -R`, no sudo sysctl available locally).

Pending: authoritative `perf-runner-v1` baselines (local numbers are advisory per
`perf-harness.md`); the per-line `AdvanceState` cost on heavy-syntax files remains
the residual first-paint cost and is orthogonal (syntax-engine / data-structure work).

## Large-file overhaul — Phase 4: direct-load fast path

The piece tree (Phase 3) already backs the document, but the load path still
round-tripped the file through `DecodeLines` into a `vector<std::string>` and then
rejoined it into the tree's original buffer — two extra full copies of the file
plus one heap allocation per line, all one-time-per-open work the Phase 3 commit
explicitly left for Phase 4 (it showed up as a small open-path allocation
regression).

Fix: when a file's bytes contain no `'\r'` they are already the document's
canonical representation (lines joined by `'\n'`), so `OpenFile`/`LoadContent`
hand the bytes straight to `PieceTree::ResetFromText`, which moves them into the
original buffer and scans newlines once. CRLF/CR/mixed files still take the
normalizing `DecodeLines` path. Equivalence (LF vs CRLF content, no-trailing-
newline, empty file, UTF-8/NUL encoding classification) is pinned by
`TextViewport/FastLoad*` unit tests.

Measured (local advisory A/B, Phase 3 `HEAD` vs Phase 4 working tree, both built
`microide-perf` RelWithDebInfo, 15 iterations):

- `large_file_restore_deep_scroll_first_paint` — clean load-path isolation: the
  measured region (`deep_restore.jump_and_first_paint`) is **identical** at 10,802
  allocations on both, while whole-iteration allocations drop **172,875 → 72,848
  (−57.9%)** and bytes **−53.3%**. The entire delta is the one-time load, the only
  thing Phase 4 changed.
- `large_file_open_lf_first_paint` — new baseline-gated scenario opening the LF-only
  50k-line `synthetic_kernel.cpp` (the existing `large_file_open_first_paint`
  fixture has mixed line endings and so never exercised the fast path). p50 ≈ 66k
  allocations / ~46 ms to open + first paint.
- `mid_file_edit_latency_large_file` — unchanged within noise (Phase 4 does not
  touch the edit path).

Not done: memory-mapping the original buffer. It would cut steady-state RSS for
files that are never fully read, but a SIGBUS on external truncation while mapped
is a hard crash; the project's correctness-over-memory priority does not accept it.
The original buffer stays a heap `std::string`.

Pending: authoritative `perf-runner-v1` baseline for `large_file_open_lf_first_paint`
(the committed baseline is locally seeded / advisory per `perf-harness.md`).

## Large-file overhaul — Phase 5: incremental buffer-local find

Find-as-you-type (`WorkspaceShell::RefreshBufferSearch`) rebuilt the entire match
set on every keystroke: it materialized the whole document into a
`vector<std::string>` via `TextBuffer::Snapshot()` and rescanned all lines —
O(document) per keystroke regardless of how much had already been typed.

Two changes:

1. **No snapshot.** `FindLiteralSearchMatches` gained a `TextBuffer` overload that
   scans line-by-line through the piece tree's zero-copy `LineView`, so the cold
   path no longer materializes a whole-document `vector<std::string>` (the old
   `Snapshot()` allocated one `std::string` per line on the first keystroke of a
   session).
2. **Incremental refine.** When the new query merely extends the previously searched
   one over an unchanged buffer (`QueryExtendsCaseInsensitive` + viewport identity +
   `content_revision` guard), every match of the longer query is also a match of the
   shorter prefix, so the new set is a subset. `RefineLiteralSearchMatches` filters
   the cached `matches` in O(prior matches · needle) instead of rescanning the
   document. Each kept match is re-validated against the current buffer, so a stale
   cache can only drop matches, never invent them. A non-extending edit (backspace, a
   new word) or a buffer mutation falls back to the full `LineView` scan.

Measured (local advisory, `editor_buffer_find_incremental`, new baseline-gated
scenario typing `perfocc` one char at a time over the 50k-line `synthetic_kernel.cpp`
where the token sits on nearly every line): the whole 7-keystroke find-as-you-type
phase settles at **~1,735 allocations / ~10 ms** total — the per-keystroke cost is
flat in query length rather than O(document) each stroke. Equivalence (refine of a
prefix's match set == a fresh full scan for the longer query, plus the
`QueryExtendsCaseInsensitive` gate) is pinned by
`WorkspaceTextSearch/IncrementalLiteralSearch`.

The same whole-document `Snapshot()` sat in the Ctrl-D multi-caret path
(`AddCursorAtNextMatch` / `AddCursorAtAllMatches`), which materialized a
`vector<std::string>` of the entire file on every press just to scan for the next
match. Both now scan through `LineView` (the next-match scan got a `TextBuffer`
overload of `FindNextLiteralMatchAfterSeedWrapOnce`; "all matches" iterates
`LineView`). Measured on `editor_add_cursor_next_match` (96 Ctrl-D presses on the
50k-line fixture): p50 **121,639 → 74,378 allocations (−38.9%)**, wall ~41.5 → ~32 ms.
The eliminated work is the one-time 50k-string snapshot (it was amortized across the
96 presses in this loop, but a real edit between presses rebuilds it each time).
Baseline refreshed (`perf-baseline:` — snapshot removal, allocations-only mover).

Pending: authoritative `perf-runner-v1` baseline for `editor_buffer_find_incremental`
(the committed baseline is locally seeded / advisory per `perf-harness.md`).

## Large-file overhaul — Phase 6: multi-caret edit capture regression + Snapshot audit

Migration-completeness pass over the piece-tree work. Two findings:

### Multi-caret edit undo capture was snapshotting the whole document (regression)

The Phase 3 piece-tree refactor routed all three multi-caret edit paths
(`ApplyMultiCaretInsert` / `Backspace` / `Delete` in
`src/editor/TextViewportMultiCaret.cpp`) through two file-local helpers,
`CaptureLineSlice` and `BuildAggregateFromLineSlice`, both of which took a
`std::vector<std::string>` and were fed `document_->lines.Snapshot()`. That
materializes the entire document via `ToVector()` **twice per op** (before + after
state), silently reverting the earlier slice-based multi-caret aggregate
optimization (see *Slice-based multi-caret aggregate `§3.4`* above) — the §3.4 win
was written for the vector model and the piece-tree seam re-introduced the
whole-document copy.

Fix: both helpers now take `const TextBuffer&` and copy only the affected line
range via `TextBuffer::SliceLines(start, end)` — the same range-scoped capture the
single-caret / range paths already use (`TextViewport.cpp:548-583`,
`TextViewportLanguageBehavior.cpp:642-656`). This restores slice-only capture and
extends it to *all* multi-caret edit kinds (the §3.4 version only covered
non-newline pair-insert).

Measured `editor_surround_multi_caret` (8 carets, `InsertCharacter` on the 50k-line
fixture; local advisory A/B, 10 iterations): whole-iteration allocations p50
**297,660 → 203,232 (−32%)**; the measured `insert` phase drops to ~76k
allocations / ~7 ms p50. Both removed `ToVector()` copies were ~47k allocations
each. Committed baseline (`editor_surround_multi_caret.json`) is unchanged — it now
has ~32% headroom and the gate passes comfortably; re-seed it on `perf-runner-v1`
rather than from a local-advisory run.

Correctness guard: the apply→undo→redo round-trip in `editor_surround_multi_caret`
plus the `PieceTreeEquivalenceFuzz` oracle and the multi-caret/undo unit fixtures.

### Snapshot() audit — remaining call sites are legitimate cold paths

Swept every remaining `TextBuffer::Snapshot()` / `ToVector()` caller. All are
inherently O(document) cold paths and are correct as-is: file save/normalize
(`TextViewportFileIO`), LSP `DidChange` (`LspService`), session persistence
(`WorkspacePersistenceCoordinatorSession`), filetype/indent detection, initial
syntax state (`TextViewportHighlightCache`), compare/merge serialize + validate,
and the undo-history **fallback** path (`TextViewportUndoHistory.cpp:50,109`, which
genuinely reconstructs the whole document only when grouped child edits cannot be
merged into one contiguous aggregate). One redundant cold-path round-trip was
removed: `RefreshCompareTabDerivedState` (`WorkspaceShellCompare.cpp`) no longer
re-splits (`SplitSyntaxLines`) the string it just serialized from the right buffer
for `SyntaxHighlighter::InitialState` — it reuses the memoized `Snapshot()` vector
directly. The `line_cache_` design (`TextBuffer.h`) is left as-is: no hot path
indexes the whole document through `operator[]`, and its node-stability backs the
`&buffer[i]`-stays-valid contract.

## Compare/Merge Allocation Pass (2026-06-29)

Multi-agent review pass over the diff/compare/merge build path. The review's
"per-frame render hot path" claims were all demoted on verification; the genuine
wins were constant-factor allocation removals on the compare/merge *build* path
(diff open, merge load, per-keystroke merge edits), plus two pure-dedup cleanups.

### `std::span` for the line-diff builders (eliminates slice copies)

`AppendAnchoredFallbackOps` / `BuildLineDiffOps` / `BuildCompareModelProfiled` each
materialized `const std::vector<std::string_view>` *copies* of sub-ranges
(`{begin+off, begin+off2}`) just to recurse into the exact/anchored aligners —
one heap allocation + O(n) `string_view` copy per large hunk and once per build.
`BuildExactLineOps`, `AppendAnchoredFallbackOps`, `BuildAnchoredFallbackOps`,
`BuildUniqueLineAnchors`, `AppendEqualPairs/Insert/Delete`, and both public
`BuildLineDiffOps` overloads now take `std::span<const std::string_view>`; the
three slice sites became non-owning `subspan(...)`. `span<const T>` is implicitly
constructible from `vector<T>`, so the one external caller (`MergeModel.cpp`) and
all tests compile unchanged. The backing `SplitLineViews` vectors outlive every
call, so the spans are always valid.

### `MergeChoiceLineCount` for size-only merge callsites

`compare::MergeChoiceLines` returns a full `vector<std::string>` (and the `Both*`
choices concatenate a fresh one). `BuildMergeTrackedConflicts(ForResult)` in
`WorkspaceShellMergeState.cpp` called it twice purely for `.size()`. A new
allocation-free `compare::MergeChoiceLineCount(hunk, choice)` mirrors the branch
logic and returns just the count; the two size-only sites use it. A span-returning
variant was rejected — the `Both*` cases have no contiguous backing storage to
point at. The render-path preview site (`WorkspaceShellMergeRender.cpp`) was left
on `MergeChoiceLines`: it genuinely draws the line *contents*, so it is not
size-only (the review mislabeled it).

### Merge viewport edit: span over the cached snapshot

`UpdateMergeTrackingAfterViewportEdit` copied the changed line slice into a
temporary `vector<std::string>` to feed `UpdateMergeMaxVisualColumns`, which
already takes `std::span<const std::string>`. It now binds the (cached, lazily
materialized) `TextBuffer::Snapshot()` once and passes a `span` over the changed
sub-range — zero copies per keystroke. Safe only because `Snapshot()` returns
contiguous storage with no intervening mutation before the synchronous consume.

### Dedup cleanups (maintainability, no measurable perf delta)

- The `ensure_redraw` guard lambda (`if (!HasAnyRedraw()) req();`) was copy-pasted
  into four mouse/wheel/motion handlers. Replaced with a single inlined private
  template `WorkspaceShell::EnsureRedraw` (not `std::function` — keeps the input
  path allocation-free). The structurally-different copy in
  `WorkspaceKeyInputCoordinator` (a different class guarding on `operations_`) was
  deliberately left alone to respect the coordinator service-ref boundary.
- The plugin sidebar query trio (`SnapshotSidebar` / `ConfirmSidebarItem` /
  `ToggleSidebarItem` in `PluginHostPublicApi.inc`) repeated an identical
  published-view check + worker-thread live-map lookup + error literal. Folded into
  a private `PluginHost::DispatchSidebarQuery` template that takes the worker op as
  a callback (the live provider lookup stays *inside* the `RunOnWorkerBlocking`
  lambda, so no worker-owned pointer escapes). Not a hot path (each blocks on a
  cross-thread round-trip); pure dedup.

Validation: full `ctest` suite (incl. the architecture-invariants lint) green.

## Notes

- The blame overlay remains performance-sensitive, but the width-cache work should reduce its layout
  cost without changing behavior.
- The terminal still needs broader real-world validation; these fixes reduce load but do not expand
  emulator coverage by themselves.
- LSP server startup happens asynchronously; users will see gradual feature availability as the
  server initializes rather than upfront startup delay.
