# MicroIDE Performance Findings

Last reviewed on 2026-04-22 after startup profiling focused on project-open overhead.
Updated on 2026-04-22 with Git status and syntax definition deferral optimizations.
Updated on 2026-04-22 with asynchronous LSP server initialization to prevent UI blocking.
Updated on 2026-04-23 with deep-dive static analysis of render-path and edit-path bottlenecks.
Updated on 2026-04-23 with syntax, viewport, terminal, and output-panel cache fixes from that review.
Updated on 2026-04-23 with terminal foreground run coalescing, buffer-search caching, SDL text-cache lookup cleanup, and syntax-rule partitioning.

This note captures concrete bottlenecks that were found in the current codebase, what was already
fixed, and what still remains worth doing.

## Fixed In This Pass

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

- `WorkspaceShell::SetProjectRoot` and plugin-reload startup paths now refresh Git sidebar data
  only when the active sidebar mode is Git
- `FileFinder` now defers index-cache materialization until a real file-finder query refresh is
  requested
- project-shell coverage now includes deferred Git sidebar refresh at project open
- project-shell coverage now includes file-finder open-and-select behavior with deferred index
  cache build

Impact:

- startup traces now avoid the eager file-index scan on project open
- on the same local startup-trace command, `WorkspaceShell::SetProjectRoot` dropped from the
  previous `~370 ms` hotspot range to `~17-20 ms`
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

Every frame that renders the terminal panel calls `SnapshotLineRange`, which acquires the mutex
and deep-copies all visible `TerminalLine` objects. Each `TerminalLine` contains a
`std::vector<TerminalCell>`, so for 40 visible rows at 200 columns each, this is 8,000 cell
copies plus 40 vector copies per frame, even when the terminal has been idle.

Fix: Add a generation counter incremented by the writer thread on every write. The render thread
checks whether the generation has changed since the last snapshot; if not, it reuses the previous
frame's terminal lines without copying. This makes the common idle-terminal case allocation-free.

Relevant code:

- `src/terminal/TerminalSession.cpp` — `SnapshotLineRange`, line 705
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — line 269

### 7. Output panel calls `HighlightLine` on every visible line every frame (HIGH — output panel)

When the output panel shows code-context snippets, line 314 of `WorkspaceShellRenderBottomPanel.cpp`
calls `editor::runtime_syntax::HighlightLine` on every visible code snippet every frame. This
runs the full PCRE2 regex highlighter per line per frame even when the output hasn't changed.

Fix: Cache the highlighted tokens for each output channel entry; invalidate the cache only when
the channel appends new entries. A simple `std::vector<HighlightedLine>` parallel to the channel's
entry list is enough.

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

The per-line state cache uses `std::vector<std::optional<SyntaxState>>`. Each `optional` adds
a bool + padding, making each element ~24 bytes on 64-bit. For a 10,000-line file this is ~240KB
just for this vector. The "uncached" sentinel can instead be `SyntaxState{definition_id=0}`,
collapsing to a plain `std::vector<SyntaxState>` at ~16 bytes per entry (~160KB).

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

- `docs/startup-tracing.md`

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

## Notes

- The blame overlay remains performance-sensitive, but the width-cache work should reduce its layout
  cost without changing behavior.
- The terminal still needs broader real-world validation; these fixes reduce load but do not expand
  emulator coverage by themselves.
- LSP server startup happens asynchronously; users will see gradual feature availability as the
  server initializes rather than upfront startup delay.
