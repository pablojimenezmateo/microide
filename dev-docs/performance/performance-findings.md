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

Updated on 2026-07-04 with a full measurement pass on perf-runner-v1: a plugin buffer-lifecycle
subscriber gate shipped, two tempting micro-optimizations were measured and rejected, and the
top interactive scenarios were confirmed render-bound (see "2026-07-04 measurement pass" below).

## 2026-08-13 (seventh pass) the two per-line caches free what the next frame rebuilds

Found by running the allocation tracer against the real binary rather than
reading the committed sweep — which is also how TD-2026-08-13-197 (the sweep's
top-site NAMES are `-fipa-icf` folds) was found. In
`editor_typing_minified_line.type_burst`, four of the five biggest sites were the
same two shapes, resolved through `addr2line -i`:

| allocations / iteration | site | what it is |
| ---: | --- | --- |
| 32 (40 B each) | `highlight_cache_.emplace` ← `HighlightedLineTokens` ← `EditorViewRenderer::Render` | a hash node per cached line |
| 32 (192 B each) | `visible_line_cache_.emplace` ← `VisibleLineLayoutRefCached` ← `Render` | a hash node per rendered row |
| 16 (1,536 B each) | `vector<size_t>::reserve` under the same | a `LayoutLine` buffer |

Both caches were **erasing** on invalidation and re-emplacing on the repaint that
immediately followed, so a keystroke freed a screenful of buffers and allocated
the same screenful back on the same frame. The two fixes are the same idea:

- **the per-line token cache stamps a generation instead of erasing.** An edit
  bumps a counter (whole-document invalidation) or stales the suffix at/after the
  edited line by zeroing those entries' stamps; the entry, its hash node and its
  token vector all stay, and the retokenize writes straight into the vector via
  the new `runtime_syntax::HighlightLineInto`. Lines *below* the edit keep their
  stamp and are not recomputed, which is what the old suffix erase already did.
  Retained bytes are bounded by the entry cap the live cache already had.
- **the visible-line layout cache retires a `LayoutLine` into a pool.** The
  eviction path already recycled a node (scrolling misses on every row); the edit
  path did not. `InvalidateVisibleLineCacheFrom` and the width-table resync in
  `ClearVisibleLineAndMaxColumns` now both move the layout into
  `visible_line_layout_pool_`, and the next miss builds into it —
  `BuildVisibleLineWindowInto` already `clear()`s rather than reassigns, so the
  capacity survives. `InvalidateAll` stays the one call that releases the pool.

The pool is a `vector<LayoutLine>` rather than extracted map nodes because a
`node_type` is move-only and `TextLayoutCache` must stay copyable — a split pane
copies its sibling's caches. That leaves the map node to allocate and reuses the
three per-row buffers, which are the ones sized to the row.

The trap worth remembering: the first version of the layout pool measured as
doing nothing, because `UpdateVisualColumnCacheAfterEdit` calls
`ClearVisibleLineAndMaxColumns` on any edit the per-line width table cannot
splice — one call *after* the invalidation had filled the pool, and it cleared
it. A regression test asserting the `editor.visible_line_layout_recycled` counter
moves is what caught it; the code review had not.

### Measured (perf lane, `p50_allocations` vs the committed baselines)

Allocation counts are deterministic within one build configuration, so these
hold despite the run being taken at load ~10; every wall/cpu/rss number from the
same run is lane noise and is excluded. All eight scenarios PASS.

| scenario | baseline | after | |
| --- | ---: | ---: | ---: |
| `lsp_document_symbols_parse` | 324,474 | 120,473 | −62.9 % |
| `editor_soft_wrap_long_line_typing` | 7,959.5 | 3,713.5 | −53.3 % |
| `editor_surround_multi_caret` | 808.5 | 645 | −20.2 % |
| `editor_typing_minified_line` | 1,843.5 | 1,583.5 | −14.1 % |
| `editor_smart_indent_typing` | 6,515 | 6,035 | −7.4 % |
| `scroll_large_file` | 259 | 258 | −0.4 % |
| `dap_protocol_encode_decode` | 330,309 | 329,508 | −0.2 % |
| `merge_scroll_interleaved_hunks` | 14,296 | 14,295 | −0.0 % |

Two of those numbers say something the headline does not:

- **`dap_protocol_encode_decode` barely moves, and that is expected.** The −801
  is the two envelope reserves; the deep-copy fix cannot show here because the
  scenario passes `vars_body` as a const lvalue that it reuses across 200
  iterations, so `MakeRequest` still has to copy it. The product path hands it a
  temporary and now moves. A scenario measuring the *shape* of a call is not
  automatically a scenario that can see the shape change.
- **`merge_scroll_interleaved_hunks` does not move at all.** Its scroll burst is
  compare-surface highlighting through `PopulateCompareSyntaxTokensForWindow`,
  which does not go through the viewport token cache these two fixes touch.

Nothing here can fail an allocation gate by getting cheaper — the gates are
one-sided — so the evidence that the work still happens is the correctness
tests, not the PASS: the two staleness tests, the recycle counter test, and the
full suite at 29/29.

## 2026-08-06 (sixth pass) opening a file you already have open (perf-runner-v1)

TD-2026-08-06-138 filed a counter reading: `editor.line_width_full_measures` was
**exactly 2x the document's line count** on ten scenarios that open a large file.
Its headline said "twice per file open" and it named a hypothesis — indent
detection landing after the first table is built, so `tab_size` changes and the
first build is discarded. Both were wrong.

### What the instrumentation said, and why the hypothesis could not have been tested without it

`full_measures` counts lines walked. It cannot distinguish one build from two, and
it says nothing about what invalidated the previous one. Splitting it fixed that in
one run: `editor.line_width_table_builds` plus three reason counters that sum to it
(`_rebuild_cold`, `_rebuild_tab_size`, `_rebuild_line_count`).

`tab_size` never fired. Both builds came out **cold** — the table was *empty* at
each one, so something wiped it whole in between. And iteration 0 of every scenario
built it **once**: a genuinely fresh open was never the 2x case.

The perf driver persists across iterations, so iteration 1 onwards *re*-opens a file
that is already open. That is where the real defect lived.

### Three findings, in order of what they cost

**1. Re-opening an already-open file re-read it from disk.**
`TabCoordinator::OpenFileInNewTab`'s existing-tab branch called
`ReloadCleanEditorTabsForPath` unconditionally: read the whole file, build a fresh
viewport, swap it in, and drop every derived cache the tab held — widths,
highlights, folds, undo history — to arrive at byte-identical content. That is what
Ctrl+P to a file you are already looking at did; VSCode focuses the tab. The
focus-regain sweep `ReloadCleanOpenBuffersFromDisk` paid the same cost once per open
buffer, on every window focus.

`ReloadEditorTabsForPath` now stats the file first and returns when every open view
of it already records that signature. mtime+size is the same equality the
self-write echo suppression in `WorkspaceShellProjectChanges` already trusts. Clean-only
form only — the from-disk form is the banner's explicit "discard my edits and
reload", where a dirty buffer's recorded signature can legitimately still match.

**2. A viewport copy deep-copied the width table and then threw it away.**
`TextViewport`'s copy constructor copied `layout_cache_` and then called
`InvalidateVisualColumnCache()`. Move construction and move assignment did the same
to a table they had just stolen for free. The reload copies a viewport twice, which
is precisely where the second cold build came from.

Widths are a function of the document's bytes and the tab size, and a copy shares
both — `document_` is a `shared_ptr`. The only thing a copy resets is
`folding_model_`, and the one product that depends on it is the wrapped-row table
(also the only place a stale `const FoldingModel*` could alias a later model at the
same address). So the copy now drops exactly that half, via
`TextLayoutCache::DropWrappedRowLayouts()`. `SetFoldingModel` had already made this
distinction, for the same measured reason, one commit family earlier.

**3. `ClampScrollState()` read `MaxVisualColumns()` to clamp an offset already at 0.**
The clamp can only lower `horizontal_scroll_`, so at 0 the answer is 0 either way —
but reaching it builds the whole-document table. Every file opens at column 0 and
every restored background tab stays there without any pane drawing a horizontal
scrollbar. An early return removed the whole-document walk from six further gated
scenarios outright.

### Result

p50 wall / p50 allocations for the whole scenario, against the baselines committed
the same morning:

| scenario | wall | allocations |
| --- | --- | --- |
| `editor_add_cursor_next_match` | 5.51ms → 0.26ms | 3,660 → 1,005 |
| `editor_indent_detect_open` | 3.28ms → 0.81ms | 1,961 → 708 |
| `large_file_restore_deep_scroll_first_paint` | 9.38ms → 3.24ms | 3,880 → 791 |
| `editor_column_selection_burst` | 11.14ms → 6.90ms | 2,949 → 278 |
| `editor_mouse_selection_drag` | 12.19ms → 4.47ms | 5,482 → 2,101 |
| `editor_occurrences_scan` | 17.51ms → 10.02ms | 4,676 → 1,641 |

`editor.line_width_table_builds` is 0 on every scenario that reopens a file and 1
where the content genuinely changed. The three diff/staging scenarios and
`external_change_refresh_open_diff` went from 14,465–14,475 full measures to 0.

### A fourth finding the new counters produced on their own

Counting the widest-line scan separately (`editor.line_width_max_scans` /
`_max_scan_lines`) showed `editor_toggle_comment_large_selection` doing **16 scans of
a 50,001-entry table — 800,016 entries walked** per iteration. `max_visual_columns()`
is memoized as (widest width, which line is widest), and the memo was dropped
whenever any line an edit wrote came out at least as wide as the current maximum.
Widening lines is an ordinary edit.

Exactly one case needs the rescan: the widest line *itself* was rewritten or removed,
because its old width is gone and the runner-up is recorded nowhere. Every other edit
leaves the old maximum standing, and a line the edit did not touch cannot have
shrunk — so the maximum can only grow, and it grows to the widest inserted line,
which is already in hand. 16 scans → 8; the 8 that remain are the un-toggles, which
shrink the lines they rewrite.

### Three things to carry forward

- **A scenario's counters cover the whole scenario function, not the `Measure(...)`
  region.** TD-138's table read "per file open" because the open is in the setup and
  the setup is inside the counter window. Check what the scenario body does before
  attributing a counter to the measured work.
- **A counter that aggregates two call sites cannot answer "how many times".**
  `full_measures` was bumped by both the full rebuild and the incremental edit path,
  which is why two scenarios were wrongly on TD-138's list and why the doubling had
  no attributable cause for a day. Count the *event*, then the *volume*.
- **Removing periodic work removes whatever it was incidentally bounding.** The
  reload was the only thing resetting a tab's derived caches. Resident-growth
  baselines moved accordingly, and the ceiling nobody had chosen is now
  TD-2026-08-06-142.

## 2026-08-05 (fifth pass) no caller wants the whole line (perf-runner-v1)

TD-2026-08-05-133 was the one line-sized copy left on the keystroke path: an
in-line edit splits its line into three pieces, so `PieceTree::LineView` can only
serve it by copying it into a per-revision cache, and on a minified bundle that is
~2 MiB per keystroke. The copy is **shared** — the first caller in a frame pays and
the rest hit the cache — so it could only be removed by making sure *no* caller
wants the whole line. There were eight.

**Result on `editor_typing_minified_line`: 256 line materializations per 8
iterations became 8** — one per file open, none per keystroke. Traced application
self time per iteration **5.16 ms → 1.30 ms (4.0x)**, allocations **4,918.5 →
4,559.5**.

### The single most useful finding: a bound that reads its input before rejecting it

Four of the eight callers already had a cap, and the tech-debt entry called them
"the easy half" for that reason. Every one of them applied the cap *after* reading
the line:

| caller | its bound | what it read first |
| --- | --- | --- |
| `SignatureDetectHead` | first 4 KiB of a head line | `lines[i].substr(0, 4096)` |
| `FoldingModel::ScanLine` | `kMaxBracketScanLineBytes` | `lines[i]` |
| `HighlightedLineTokens` | `kMaxHighlightLineBytes` | `lines.LineView(i)` |
| `MeasureIndent` | the leading whitespace | `lines[i]` |

So the very lines each cap exists to skip were the expensive ones, once per
keystroke. **A cap on a line's LENGTH must be asked of `LineLength`, which reads no
text — never of the bytes.** This is the same shape as "a bound in lines is not a
bound on work" from the pass below, one level down: a bound in bytes is not a bound
on work either, if you have to fetch the bytes to apply it.

### The primitive

`PieceTree::LineWindow(line, byte_start, byte_len, scratch)` — zero-copy when the
window lies inside one piece (still the common case on an edited line, which splits
into exactly three), otherwise copying the WINDOW, never the line. Surfaced through
`TextBuffer` and `LineSpan`. Two consumers grew out of it:

- `editor/CaretNeighborhood.h` — the line's length plus at most nine bytes around
  the caret, which is everything auto-close, skip-over-close, brace-split, the
  multi-caret pair fan-out, `Backspace` and the edit-range clamp actually consult.
- `editor/LineIndentScan.h` — the leading whitespace, read 256 bytes at a time.
  The fold indent source, the indent guides and the caret's active-guide column had
  each grown their own byte-identical copy of this scan; there is one now.

### The render path, which genuinely reads the line

`TextLayoutCache` builds a row from `{bytes, start_byte, line_length}`. The window
is **four bytes per visual column the walk can reach** — the longest UTF-8 sequence
— which is exactly tight: shortening it by one byte fails the differential, and so
does dropping the factor. `RowDecorationInput::text` is now empty on the editor's
row path unless the row carries a diagnostic (its only consumer there), with
whole-line plugin decorations reading a new `line_length`.

`FindBracketMatch` refuses a caret line past `runtime_syntax::kMaxHighlightLineBytes`
— the same threshold and the same argument the fold cap already makes: past it the
line has no tokens, so a brace inside a string cannot be filtered and the matched
pair would be arbitrary rather than approximate.

### Traps this pass hit

**A "fragmented" fixture that was not.** Three tests here spliced a *zero-length*
range to make a line span pieces. That is a no-op — the line stayed contiguous and
the copying path each test named was never taken, while every test passed. They now
insert a byte and delete it again (the tree never re-merges pieces), and one
asserts the fixture materializes before relying on it. Same failure mode as an
architecture lint that cannot fire.

**A differential whose oracle is insensitive to the bug.** The chunked indent scan
was first tested on a document whose lines all shared one indent string. Fold
output only compares indents *against each other*, so a consistently wrong
measurement produced identical folds and the test passed against a deliberately
broken chunk carry. The sharp case puts two lines one column apart across the
chunk boundary, with a tab size that does not divide the chunk — so the boundary is
not a tab stop and a wrong carry changes the fold tree.

**Removing a whole-line read can COST allocations.** With the render window in and
`CopyRange`'s walk stack still a local `std::vector`, allocations went *up* — 4,822
→ 4,969 — because a per-revision copy became a per-row-per-frame bounded read, and
each one paid for that stack. Moving it to a member took it to 4,559. A cost that
was amortized over a revision is not amortized over a frame.

### Instrumentation added

`PerfHarness::PumpFrames::Present` and `::ShellRender`. The scenario traced 41 ms
of self time against 160 ms of wall and the missing 119 ms read as "unexplained" —
it is `SDL_RenderPresent` on a 1920x1080 software surface, ~85% of the measured
wall, and entirely the harness's. **Read a frame-pumping scenario's wall number
knowing that.** The editor's own per-frame cost on this scenario is 2.2 ms against
16.5 ms traced, and a change to the edit path has to be judged against the 2.2.

`editor.bracket_match_line_too_long` joins the existing
`editor.line_materializations` / `editor.line_materialized_bytes`. The latter pair
is what makes any of this testable: every caller fixed here returned the *right
answer* before and after, so only a counter can tell a bounded read from an
unbounded one. `TextViewport/TypingInALongLineCopiesNothing` is the always-on gate,
confirmed to fail with any single caller reverted.

## 2026-08-05 (fourth pass) the rest of the long-line interactions (perf-runner-v1)

The three passes below fixed *typing* on a file with no line breaks in it. Nothing
measured any other interaction with that shape, which is the same coverage hole
that made TD-2026-08-05-130/131/132 possible in the first place. Three scenarios
were added first -- horizontal scroll, buffer search, and the whole-document edit
verbs -- and then read.

| scenario | at first measurement | now |
| --- | --- | --- |
| `editor_long_line_horizontal_scroll` | 96.2 ms | 22.8 ms (4.2x) |
| `editor_long_line_buffer_search` | 33.0 ms | 17.8 ms (1.9x) |
| `editor_long_line_select_all_edit` | 22.8 ms | 17.6 ms |

Every fix below has **byte-identical allocation counts** before and after, which is
what says the change is algorithmic rather than machine noise.

### A line-count bound is not a bound on work

The first scenario reported **12,584,364 calls to `HighlightedLineTokens`** across
40 frames. Whether a bracket is inside a string or comment is a per-LINE question,
but the bracket matchers walk a line byte by byte and asked it per COLUMN — so
every byte re-resolved the line's token vector through the highlight LRU. On
ordinary source that is a few dozen probes. On this shape it is one probe per byte
of the document, per caret move.

Behind it was the same mistake one level up: `FindBracketMatch` bounds its scan by
`max_lines_each_side`, which bounds work only if lines are bounded. Press End next
to a minified bundle's closing bracket and one arrow key read the whole file —
1.7 ms, per keystroke. `kMaxBracketMatchScanBytes` is that bound in the unit that
costs, chosen against the widest realistic window: the 50k-line C++ fixture
averages 163 bytes a line, so its full 2000-line window is ~326 KB, and the cap
sits at 512 KiB.

**Generalizable:** a bound expressed in lines, rows, or entries is a bound on work
only if those units have bounded size. Both bugs here were "we already bounded
this" — and both bounds were in the wrong unit.

### Narrowing by line is not narrowing when there is one line

Three separate loops build a row's match fills (explicit regex fragments, the
literal Ctrl+F scan cache, and the occurrence highlight). All three binary-searched
their span list down to the current LINE and then resolved every span on it against
the row's cell grid before clipping it away. On a one-line document that is every
match in the file, per visible row, per frame — the whole of `BuildDecoratedRow`'s
cost at 45 us a row.

All three now bound the slice by the row's visible source-byte window, which
`row_layout.source_columns` already carries.

The occurrence *scan* had the same shape a level earlier: it read every visible
line end to end, and its only consumer is the fill loop that clips to the window.
Since the needle is the word under the caret, a word-motion keystroke re-lowercased
and re-searched a megabyte. It is now bounded to the same window (soft wrap keeps
the full scan, because there the whole line really is visible).

### What it took to test the narrowing honestly

The risk of any narrowing is dropping something that should have been drawn, and
the first two test designs could not see that:

- **A pixel differential against the same text rendered unscrolled passes when the
  bound is wrong the same way on both sides.** The control goes through the same
  production code and loses the same fills. Three of four injected bound bugs
  survived it.
- **An exact-color pixel count finds nothing**: highlight fills are alpha-blended
  and then overpainted by glyphs, so the color never appears literally.

What works is computing each in-window match's cell position *independently* — from
`ComputeMetrics` and the char width, not from the code under test — and asserting
each one changed pixels against a no-matches render of the same frame. All six
injected bound bugs fail it.

**Generalizable:** a differential test whose reference shares the code path under
test is only a consistency check. It catches asymmetric bugs and is blind to
symmetric ones, which is the class a bounds change most often produces.

### A coupling this created, on purpose

The occurrence scan window and the row paint window are both
`viewport.visible_columns()`. They agree only because
`RenderActiveWorkspaceSurface` applies the render metrics to the viewport before
building the view model. The regression mirrors that ordering explicitly and says
why — it was previously sizing the viewport by hand and passing for the wrong
reason, which is how the change caught it.

### Instrumentation added

`EditorViewRenderer::Render::BracketMatch` and
`WorkspaceShell::Render::BuildEditorViewModel`. Both ran on every frame, both were
the largest cost in their parent, and both sat inside a parent's self time with no
scope of their own — which is why a scan reading megabytes per keystroke had never
appeared in a ranking.

## 2026-08-05 (third pass) closing the render and folding paths on a long line (perf-runner-v1)

The two passes below took the *edit* path on a file with no line breaks in it from
13x the affected line's bytes to 1x. This pass takes the rest: everything that
re-derived a whole-line fact per keystroke. **`editor_typing_minified_line`
148.7 ms -> 18.9 ms (7.9x)**, allocations unchanged at 4,918 (TD-2026-08-05-132).

### One fact replaced four O(line) passes

Is this line plain ASCII — no tab, no byte >= 0x80? If so it spends exactly one
visual cell per byte, so visual column IS byte column at every offset and every
conversion between the two is O(1). The per-line width table `TextLayoutCache`
already maintains now carries that bit, packed into the word it already stored (a
`bool` member would have doubled a document-sized table to carry one bit; widths
are bounded by the line's byte length, so the top bit is free).

| what it did per keystroke | now |
| --- | --- |
| `BuildVisibleLineInto` stepped code points from column 0 to the horizontal scroll offset, per rendered row | starts at the line's first tab-or-multibyte byte, or skips the scan entirely with the cached facts |
| ...and re-measured the full line for the scrollbar and end-of-line decorations | takes the cached width |
| `UpdateVisualColumnCacheAfterEdit` re-measured the edited line | derives the width from the splice: a plain line stays plain exactly when the inserted text is, and its width is its byte count |
| `CaretForLine` / `PreferredColumnForCaret` / the soft-wrap row lookup walked to the caret | one shared `VisualColumnAt` / `TextColumnAtVisualColumn` pair reading the same facts |

The prefix-skip is worth stating precisely because it reads like a special case and
is not: the walk starts at the first byte where visual column can stop tracking
byte offset, which is exact for every line. On an ordinary line both forms do the
same negligible work.

### Folding got the threshold, not an algorithm

The per-line bracket cache resyncs a whole line at a time — an edit anywhere in a
line rescans all of it, and there is no summary that can be had for less. On the
2 MiB fixture that meant re-matching 209,570 bracket events every keystroke.

A line past `FoldingModel::kMaxBracketScanLineBytes` now contributes no brackets,
and that cap is set equal to `runtime_syntax::kMaxHighlightLineBytes` with a
`static_assert` holding the ordering. The equality is the argument: past the
tokenization cap a line has no syntax tokens, so nothing can tell a brace inside a
string literal from a real one, and the folds derived from such a line were
arbitrary rather than approximate. Contributing none is both cheaper and more
honest than contributing wrong ones. This is the same shape mature editors use to
stop giving very long lines whole-line treatment.

### The thing that hid in plain sight

After the first three fixes the ranking still showed ~0.055 ms per frame of
unexplained self time in the render row loop, and an equal amount inside *filetype
detection*. Both were the same cost wearing different names: `PieceTree` copies a
line that spans pieces into a per-revision cache, and an in-line edit splits its
line into three pieces — so from the first keystroke the edited line is a copy,
charged to whichever consumer asks for it first in that frame. As the code around
it got faster, it kept moving.

**Generalizable:** a shared, cached, first-caller-pays cost is invisible to a
self-time ranking by construction — it is never where it appears to be. It needs
its own scope. It has one now (`PieceTree::MaterializeSpanningLine`), plus
`editor.line_materializations` / `editor.line_materialized_bytes`, and it is the
whole of TD-2026-08-05-133: 7.5 of the remaining 19 ms.

The caret walk it hid needed its own counter for a different reason — a walk and a
lookup return the same answer, so no correctness test can distinguish them.
`editor.visual_column_walk_bytes` counts what the fallback actually re-read, which
is what makes "typing in a long line does not walk it" a regression test rather
than a comment.

### Gates

`editor_typing_minified_line` is rebaselined and keeps its 1% allocation tolerance.
New coverage: the visible-line build against a walk from column zero (special byte
at every offset across several eight-byte scan chunks, every scroll offset, hinted
and unhinted); the width derivation against a from-scratch measurement across each
transition that breaks its precondition; the caret conversions against the direct
walk on plain / tabbed / multibyte / empty rows with the table cold and warm; the
fold cap at the boundary in both directions; and a zero-bytes-walked assertion with
its own reachability check. Each was confirmed to fail with its fix reverted. A
debug-only cross-check in `VisibleLineLayoutRefCached` re-measures short lines
against the width table so the "every content edit either splices this table or
drops it" invariant the pass now rests on cannot rot quietly.

## 2026-08-05 (second pass) closing the edit path on a long line (perf-runner-v1)

The first pass below took a one-character insert from 13x the affected line's
bytes to 5x, and filed the remaining 5x as the data model. This pass closes it:
**5.00x in 12 allocations -> 1.00x in 9**, and
`editor_typing_minified_line` **528 ms -> 156 ms (3.4x)**.

### The undo entry now has two shapes

An edit that stays inside one line records
`{start_line, start_column, removed_text, inserted_text}`. Everything structural
— line splits and joins, multi-line replaces, grouped/multi-caret aggregates,
whole-document changes — keeps `{start_line, before_lines, after_lines}`.

Three of the five remaining copies were the entry itself (the pre-edit slice, the
composed post-edit line, the coalesce's copy of the aggregate). The other two fell
out with them, because the whole-line shape was what forced them:

- `PieceTree::ReplaceTextRange(start_line, start_column, end_line, end_column, text)`
  exposes the splice the tree already performed underneath. The line-shaped
  `ReplaceLineRange` had to join the rebuilt line into a replacement buffer and
  append that to `add_`; the column-scoped form copies only `text`.
- The encoding upgrade scans the spliced-in bytes instead of the rebuilt line.
  Exact, not approximate: every other byte on that line was classified when it
  entered the document.

The one line-sized allocation left is the piece tree materializing the edited line
into its per-revision cache. The renderer and the visual-width scan both want the
line as one contiguous view, and they share that one copy.

### The largest single win was not in the entry model

With the copies gone, `MICROIDE_PERF_SUMMARY=1` named
`TextViewport::BuildRangeHistoryEntry`'s own **self** time — 164 ms over 64 calls —
rather than anything it called. `TextLayout::ClampTextColumn` rounded a byte offset
down to a code-point boundary by **re-tiling the line from byte 0**, one UTF-8
sequence at a time: an O(1) question at O(column) cost, run four times per
keystroke (both range endpoints, the preferred column, and `PreviousTextColumn` for
a backspace). UTF-8 is self-synchronizing, so the answer is at most three bytes
back. **461 ms -> 160 ms from that one change.**

Worth generalizing: after removing allocations, re-read the profile before
assuming the next cost is also an allocation. The remaining hot spot had none.

### What is left, and why it is a design decision

Per-keystroke self time on the ~2 MiB line, after all of the above:

| scope | calls | per call |
| --- | --- | --- |
| `EditorViewRenderer::Render::RowLayout` | 136 | 0.91 ms |
| `FoldingModel::SyncLineBracketCache` | 64 | 1.25 ms |
| `FoldingModel::WalkLines` | 64 | 0.57 ms |
| `TextViewport::ApplyHistoryEntry` | 64 | 0.24 ms |

The edit path is now ~0.3 ms of a ~2.4 ms keystroke. The rest is layout and folding
re-deriving whole-line facts, and none of it is a stray copy — each is a real
O(line) pass. VS Code's answer to this class is a length threshold past which a
line stops getting whole-line treatment; that is a product decision, filed as
**TD-2026-08-05-132** rather than done here.

### Gates

`editor_typing_minified_line` keeps its 1% allocation tolerance and is rebaselined.
`TextViewport/SingleLineEditAllocatesABoundedMultipleOfTheLine` tightened 6x -> 2x.
Both, plus a new test that pins which entry shape each edit records, were confirmed
to fail with the routing reverted. `ClampTextColumn` is pinned against the old
forward tiling over every offset of every arrangement of 1–4-byte sequences, with
separate bounded-resync properties for malformed bytes.
`PieceTreeEquivalenceFuzz` now drives both mutation primitives against one model.

## 2026-08-05 the edit path on a line with no newlines in it (perf-runner-v1)

A one-character insert allocated **13x the affected line's bytes** in 24
allocations. The whole 95-scenario suite was green throughout, because no fixture
in the tree has a long line — the widest in `large_project` is 20 bytes — so a
cost proportional to line length had nowhere to show up. On a minified bundle,
which the editor opens with no size guard, that was ~22 ms per keystroke.

Five of the copies were bookkeeping, not the data model, and are gone:

| site | what it did | why it mattered |
| --- | --- | --- |
| `BuildAppliedEdit` | deep-copied after-lines, then erased the common prefix/suffix | copied exactly what the trim discarded, to produce a 1-byte replacement |
| `TryMergeGroupEntry` | `Entry merged = aggregate;` before knowing if the merge was possible | two whole-line copies per keystroke; paid again on the reject paths |
| `PieceTree::ReplaceLineRange` | joined with no reserve, then `std::move(joined) + "\n"` | the concatenation reallocates — 4 MB join, then 8 MB |
| `PieceTree::SetLine` | wrapped its argument in a temporary vector | one full line copy per in-line keystroke |
| `BuildRangeHistoryEntry` | `prefix + replacement + suffix` from owned substrings; `operator[]` in Backspace/DeleteForward | ~3 walks of the line, plus `operator[]`'s materialized copy |

Result: **13x -> 5x in allocated bytes, 24 -> 12 allocations, ~22 ms -> ~13 ms
per keystroke on an 8 MB line.** Ordinary-file scenarios are unmoved — all 14
editor/typing baselines pass unchanged, which is the expected shape: these are
proportional-to-line-length costs, and ordinary lines are short.

**Two things worth carrying forward.**

`TextBuffer::operator[]` is not an accessor, it is a materializer. It copies the
line into a per-revision cache that the next mutation clears, and the header says
so and even exposes `materialized_line_count()` so a test can pin a path as
zero-copy. Hot paths want `LineView`/`LineLength`. The single-caret edit path was
converted; `TextViewportMultiCaret.cpp` and `TextViewportLanguageBehavior.cpp`
still hold ~20 reads that only want a length or a clamp.

The remaining 5x is the whole-line undo model and was filed as **TD-2026-08-05-131**
with the per-copy breakdown. It needed a column-scoped `HistoryEntry`, which is the
second pass above.

Gated by `editor_typing_minified_line` (new fixture: one ~2 MiB line) at a **1%**
allocation tolerance — the default 10% would have passed the +7.7% regression the
scenario exists to catch — plus a byte-budget unit test. Both were confirmed to
fail with a fix reverted.

## 2026-08-04 `FoldingModel` rewrite, ninth round (perf-runner-v1)

The item that had stood open since the fifth round -- "the fold model rescans the
whole document per keystroke" -- is closed. It was correctly diagnosed as a
rewrite rather than a constant-factor change, and that is what it took.

### What the shape had to become

Folds are a pure function of two per-line facts (the bracket bytes on a line, its
leading indent width). Both were already cached and spliced across an edit; the
cost was everything downstream, which re-derived the whole document's fold set
from them on every keystroke.

Two changes removed the asymptote, and both are worth understanding before
touching `FoldingModel` again:

1. **Block words.** The document is partitioned into ~1024-line blocks, each
   holding its *reduced word*: the brackets it leaves unmatched and the indent
   levels it leaves open. That reduction is a monoid -- the same stack machine
   the line scan runs -- so combining adjacent words reaches exactly the state a
   full scan would. Words store block-RELATIVE line numbers, so lines shifting
   above a block never touch it; an edit invalidates only the block it lands in.
   The walk state is memoised at each block boundary, which is what makes a
   keystroke's prefix free: the edit is inside the viewport's own block, so the
   boundary before it is still valid.
2. **Windowed resolution.** The model no longer materialises every fold in the
   document -- it resolves the folds a line window needs (opening inside it, plus
   the ancestors of its first line for sticky scroll). Per-frame cost became
   O(window). Reaching a far-below closer consumes whole block words rather than
   lines.

### Three things measurement caught that reading the code did not

- **The forward walk never terminated early.** It stopped when both stacks had
  been empty, but *every non-blank line pushes an indent entry* — the indent
  stack is never empty after one, so the test could not fire and every refresh
  walked to end-of-document. A size-based low-water mark cannot express "the
  entries I care about are gone" when pops and pushes interleave; the fix counts
  the entries opened at or before the window instead.
- **The 50k-line fixture is an 8300-level brace onion**, not ordinary source.
  Tracking every enclosing construct cost a copy per refresh, a memoised state
  per block, and a walk that could not stop until the outermost brace closed.
  Everything a fold consumer asks for is innermost-first (the gutter, sticky
  scroll's eight, `InnermostFoldContaining`'s one), so `kMaxOpenDepth` = 256 is a
  cap on nothing anyone reads.
- **Chasing a closer means reading every byte in between.** There is no summary
  that can be had for less, so `kMaxForwardResolveLines` = 8192 bounds it. Above
  that bound the fold resolves when the viewport moves nearer — which is what the
  old model did for *everything* past its budget, so this is still strictly more
  fold markers on the first frame, not fewer.

### Numbers

Interleaved A/B against a worktree build of the pre-rewrite commit (five rounds
of 15 iterations, alternating binaries; single-shot runs of the *same* binary
drifted 45→55 ms on one scenario, which is why interleaving was necessary):

| scenario | delta |
| --- | --- |
| `mid_file_edit_latency_large_file` | **−20%** |
| `editor_fold_viewport_refresh` | +1.0% |
| `editor_sticky_scroll_scroll` | −0.1% |
| `editor_indent_guides_paint` | +1.2% |
| `editor_fold_recompute` | +4…8% |

`editor_fold_recompute` is the one that got slower, and it is the cold-start
cost: its viewport sits at line 25000, so the first resolve must know the bracket
depth there, which means reading everything above it — and unlike the old model
it then also builds the block words for what it read. That investment is what
makes the *edits* in the same scenario, and all of
`mid_file_edit_latency_large_file`, cheaper.

Both fold gates also caught an allocation regression on their own, and the
allocation count is what set the block size. Building every block of a 50k-line
file grew four small vectors per block by `push_back` -- ~2900 extra allocations.
Block words are now built into reused scratch and assigned in, and the block size
went from 256 to 1024 lines: the obvious trade (rebuild cost O(block) against
walk cost O(document / block)) would put the optimum near sqrt(document), but a
rebuild reads cached per-line arrays at about 4 ns a line and is nearly free at
any size in this range, while each block costs allocations for its word lists the
first time it is built.

`editor_mouse_selection_drag` sits closest to its gate afterwards (5482
allocations against a 5511 limit, and deterministic across runs). The remaining
delta is NOT the newly-resolving fold markers -- capping forward resolution to
nothing moved it only to 5450 -- it is the model's own cold-start structures on a
scenario whose drag scrolls ~18k lines into the fixture.

### Instrumentation added

`editor.fold_{refresh_calls,window_lines_walked,block_words_applied,`
`block_summaries_built,block_summary_lines,bracket_lines_scanned,`
`indent_lines_measured,partial_resolves}`. The first three were what showed the
non-terminating forward walk: 97 block words applied per refresh on a file whose
viewport needed about three. `editor.fold_event_index_lines_walked` is gone with
the O(line) prefix sum it measured — blocks carry their own event totals.

## 2026-08-04 measurement pass, eighth round (perf-runner-v1)

Six fixes, two vacuous gates repaired, one wrong debt hypothesis corrected, and a
quantified answer to "how much of the gated lane is the software text backend".
Full-suite result versus the same binary before the pass, at 20 iterations:
**95 PASS / 0 FAIL**, mean p50 wall −1.3% to −2.4% over 96 scenarios across two
sweeps. Read the per-scenario allocation deltas below rather than that mean — the
wall spread between two sweeps of the same binary is wider than the mean itself,
which is the reason this suite gates on allocation counts.

### `reserve` is not `push_back`: the reused-scratch-buffer trap

`std::vector::reserve(n)` allocates **exactly** n when n exceeds capacity — it
deliberately does not apply the growth factor `push_back` does. That is wrong for
a REUSED scratch buffer refilled with a size that grows a little each call:
`clear(); reserve(n);` with n rising 1..N reallocates and copies on every single
call, which is exactly the O(N²) the scratch buffer was introduced to avoid, while
the code reads as the allocation-free version.

A held Ctrl+Shift+Alt+Down gesture is that shape. Four exact reserves sat on it
(the box range list, the candidate list, `secondary_carets_`, and the
`secondary_caret_positions()` cache), so the previous commit's scratch members
bought nothing: the gate measured 1,200 allocations across a 400-step gesture,
i.e. exactly 3 per keystroke. `util::ReserveGrowing` (`util/ScratchVector.h`)
reserves `max(n, 2*capacity)`. **1,200 → 30 allocations; scenario p50 4,119 →
2,949.**

The same shape, one layer up: `TextLayoutCache::VisibleLineLayoutRefCached`
destroyed the LRU's front entry to make room and built a fresh `LayoutLine`
(a string plus two vectors, ~3.4 KB for a 200-column window) for the new one.
`extract` hands the node back with its buffers intact, so rewriting `node.key()`
and reinserting reuses the map node **and** all three buffers.
`editor_sticky_scroll_scroll` −5.7% allocations / −8.6% wall,
`editor_fold_viewport_refresh` −3.3% / −8.2%.

And once more in the terminal: every range-snapshot entry point on
`TerminalSession` used `clear()`+`assign()`, which destroys each `TerminalLine`
(freeing its `cells` vector) and copy-constructs a new one. That is the render
path's per-frame copy and the per-wheel-tick hover copy, so it was one allocation
**and** one free per visible row per event.

### Two gates were measuring an empty terminal

`terminal_scroll_long_output` and `terminal_alt_screen_toggle` launched a command
and slept. The harness runs with `SetUsePlaceholderTerminalsForTesting(true)`, so
no shell is ever spawned: both terminals held exactly one blank line for the whole
measured window. Neither had ever measured the thing it is named for, and the
committed baselines recorded that.

They also flapped, for a second reason: the driver is shared across a scenario's
iterations and neither closed its terminal, so iteration N ran with N terminals
attached and `terminal_alt_screen_toggle`'s allocations climbed monotonically
(751, 1030, 1231, 1400, 1965 …) purely from accumulation. That is why its
committed baseline read p50 995 against max 2805.

Both now feed the emulator through `ScenarioContext::FeedTerminalOutput` (the same
path the pty reader thread uses) and close their terminals. Byte-identical every
iteration afterwards.

### Balanced allocation counts with growing RSS means a growing container

TD-2026-08-04-130 was filed as suspected heap fragmentation. It was
`PieceTree`'s append-only `add_` buffer, which nothing ever reclaimed below the
4 GiB overflow backstop — so sustained large multi-line editing grew resident
memory without bound.

Three counters said "no leak" and all three were true about the wrong thing:
allocation counts stayed balanced (a container doubling is one alloc and one
free); `mallinfo2` showed a flat arena and flat `uordblks` (a 35 MB block comes
from mmap, not sbrk); `malloc_trim(0)` returned nothing (the memory was live).
What found it: `/proc/self/smaps` diffed per iteration named one growing anonymous
mapping, and `MICROIDE_PERF_BIG_ALLOC_BYTES=16000000` — a backtrace on any
allocation at or above that size, now kept in `tests/perf/AllocationCounter.cpp` —
named the line. **Check the mapping and the large-allocation backtrace before
theorising about the allocator.**

### The merge model copied the changed lines four times

`compare::BuildMergeModel` is one synchronous call on the shell path that opens a
merge tab, and its own self time — the hunk grouping, not the two diffs — was the
largest scope in any merge scenario. All of it was copies of `SideChange`, each of
which owns a `vector<string>`: the side tag, the concatenation, the per-hunk
partition, and a by-value parameter fed an lvalue. All four are now moves.
**Total 40.7 → 22.5 ms; grouping self time 21.2 → 3.9 ms.**

Two harness problems fell out of it. The win was **ungated** — every merge
scenario reuses its already-open tab, so the build lands on iteration 0 and is
absorbed by the warmup; `merge_model_build_interleaved` now calls it directly.
And `merge_edit_result_then_scroll`'s fixture ran `current += base` inside its
build loop, so `current` came out as the quadratic sum of every prefix: 3.3 MB of
repeated text against a 24 KB base. The scenario measured a 140x-lopsided diff
rather than the three-way merge it is named for.

### How much of the gated lane is the software text backend

Worth knowing before optimising anything in a render scope. The gated lane pins
`--renderer=software`, where `SdlTtfTextBackend` composites and uploads one
texture per unique string; the GPU-gated glyph atlas (shipped 2026-06-28) is off
there. Measured on the advisory lane, same binary, same scenario:

| lane | `editor_sticky_scroll_scroll` p50 |
| --- | ---: |
| `--renderer=software --video=dummy` (gated) | 101.6 ms |
| `--renderer=auto --video=x11` (opengles2) | **62.8 ms** |

with 98,062 glyphs going out in 23,841 batched runs and only 3,079 string-texture
hits. So ~40% of these scenarios is text drawing that real users on a GPU do not
pay, and `render::TextBackend::UploadStringTexture` topping a ranked summary is
not by itself a reason to act. `editor_scroll_fresh_content_large` is the extreme
case: 567 ms of upload and 242 ms of rasterize, all of it lane.

### Instrumentation added, and what it immediately said

- `editor.visible_line_layout_{queries,hits,evictions,recycled}` — reported a
  **99.85% hit rate with zero evictions** in `editor_scroll_fresh_content_large`,
  the scenario that looked like the obvious candidate for the layout-cache work
  above. Guessing would have optimised the wrong one.
- `FoldingModel::EventIndexForLine` + `editor.fold_event_index_lines_walked` — an
  O(line) prefix sum the fold recompute wanted three times per keystroke,
  previously hidden inside `SyncLineBracketCache`'s self time. Deriving the
  splice's end index from its start took it to two (1.8M → 1.2M line counts walked
  per `mid_file_edit_latency_large_file` iteration).
- `WorkspaceShell::Render{Merge,Compare}Surface::{Chrome,SideRows}` — both
  surfaces carried 63–74 µs per frame of unattributed self time, the largest such
  blocks in the suite. It is essentially all the side panes' row loop (the result
  pane's `EditorViewRenderer` scopes had been hiding it), and that loop is text
  draws, i.e. lane again.
- `document.add_buffer_compactions`, and the big-allocation backtrace above.

### Still open

- ~~**The fold model still rescans the whole document per keystroke.**~~
  **RESOLVED 2026-08-04 by the ninth round below.**
- **`persistence::WriteFile::DurableWrite`** is still the largest scope in
  `linter_on_save` (30 ms) and it is the fsync. Unchanged; needs the
  `SaveX() == "bytes are on disk"` contract changed deliberately.
- **`TextLayoutCache::BuildVisualLineColumns`** is ~1 ms per open of the 50k
  fixture. It is already an ASCII-fast-scan over every line's bytes at roughly
  memory bandwidth, so the only lever left is not doing it eagerly on open.
- **`multi_project_switch`'s committed baseline (17.53 ms) no longer holds on this
  machine, and it is not this pass.** It read 26.0 ms in the final sweep, so it was
  A/B'd against a worktree build of the pre-pass commit: **25.6 ms there, 26.0 ms
  here.** The scenario has simply drifted ~45% above a baseline recorded earlier;
  it still passes the 100% wall gate, and it is the one scenario whose CPU/wall
  ratio was already 1.54 (concurrent file-index, git and language-server work for
  the new root). Left unbaselined rather than papered over — rebaselining it would
  bake in whatever this machine is doing today.

  Two traps worth recording from that A/B. **Perf fixtures are gitignored**, so a
  fresh worktree fails with a fixture hash mismatch until `tests/perf/fixtures` is
  symlinked from the main tree. And once symlinked, **allocation counts are not
  comparable across worktrees**: this scenario walks project trees, the worktree's
  path prefix is 25 characters shorter, and that alone moves the count by ~600
  through std::string's small-string threshold. Compare wall across worktrees;
  compare allocations only within one.

## 2026-08-03 measurement pass, seventh round (perf-runner-v1)

Seven fixes across the highlighter, the fold recompute, the render row loop
and the file-open path, plus three flaky allocation gates repaired. Every
number below is perf-runner-v1.

### The highlighter: 24.5 -> 12.9 ms per 4000 C++ lines

`RuntimeSyntaxRegistry::HighlightLine` is the top main-thread scope of any
scroll through fresh content -- each newly exposed line is a token-cache
miss the render path resolves synchronously, because the off-thread
prefetch cannot land inside the frame that scrolled. It had **no scenario
of its own**; the interactive scenarios that reach it also pay open,
layout, render and present, so a 2x change in it moved them a few percent.
`syntax_highlight_cpp_lines`, `syntax_highlight_python_lines` and
`syntax_advance_state_cpp_lines` now gate it directly. Add the gate before
optimizing something -- two of the three fixes below would have been
unmeasurable otherwise.

- **PCRE2_UCP costs ~50% of the match time, PCRE2_UTF another ~17%.** The
  syntax rules need those flags for `\b`/`\w` around accented identifiers,
  but both flags only change behaviour at code points >= 0x80 and real
  source is overwhelmingly ASCII. Each rule regex is now compiled twice and
  the line picks the byte-mode one when it holds no byte >= 0x80. Verified
  before relying on it: all 1906 distinct built-in rule literals, both
  compilations, 20k ASCII fixture lines, zero divergences. **21.7 -> 12.9 ms.**
- **The match-data cache was a hash probe per rule per line.** `ReusableMatchData`
  keyed a `thread_local unordered_map` on the CompiledRegex address; every
  regex the hot path can reach already sits at a known index in
  `registry.rules`, so the slot is arithmetic and the lookup an array read.
  **22.6 -> 21.7 ms** on C++ (48 rules), but **11.5 -> 5.8 ms** on Python and
  **2.7 -> 1.1 ms** on the state-only replay, whose per-line work the probe
  dominated.
- **`pcre2_jit_match` skips `pcre2_match`'s per-call validation.** Gated on
  the JIT having compiled and on the pattern being byte-oriented or
  PCRE2_MATCH_INVALID_UTF, since skipping the UTF check is only safe when
  there is nothing to check. **24.5 -> 22.6 ms**, and search/replace share
  the dispatch.

### The fold recompute: 4.8 -> 3.1 ms per keystroke on 50k lines

Still the dominant cost of typing in a large file (a mid-file edit resumes
at the anchor and rescans everything after it), but three constant factors
came out of it. None changes the ranges produced.

- **The bracket scan skips eight bytes at a time** (`NextBracketCandidate`,
  has-zero-byte filter per distinct bracket byte, exact table only inside a
  flagged word). **ScanBracketRangesTail 2.84 -> 1.96 ms.**
- **The per-line syntax-token probe is deferred** until the line is known to
  hold a bracket; it is a hash lookup and was paid for every visited line.
- **The merge buckets instead of sorting.** Ordering every range in the
  document by opener line and dropping duplicate openers was a comparison
  sort over tens of thousands of ranges per keystroke; the key is a line
  index, so slotting by opener and compacting is O(n + line_count).
  **MergeRanges 0.75 -> 0.06 ms.**

`mid_file_edit_latency_large_file` 143 -> 102 ms.

### The render path

- **The editor view model was built twice per frame** whenever sticky scroll
  was on (the default): the band's height decides `visible_rows`, and the
  band's line list came out of the view-model build, so the pane built the
  whole model against a zero-height band just to read `sticky_lines.size()`.
  `StickyScrollLines` exposes the already-memoized band on its own.
  `render.build_editor_view_model_calls` 2 -> 1 per frame;
  `editor_sticky_scroll_scroll` 114.6 -> 107.4 ms.
- **The row loop read line text through the copying accessor.**
  `TextBuffer::operator[]` materializes a heap copy into an unbounded cache
  cleared only on mutation, and its header says so and exposes
  `materialized_line_count()` for a test to pin the rule. Nothing called
  that accessor and the render path was the violator (six reads per row),
  so scrolling a large file end to end left a second copy of the document
  resident. `RowDecorationInput::text` is a `std::string_view` now and the
  whole path uses `LineView`. `editor_moby_dick_workout` allocations
  **138602 -> 97976 (-29%)**.

### The open path

- **`PieceTree::RebuildFromOriginal` indexed newlines byte at a time** --
  8.2M iterations for the 50k-line fixture, growing the index from empty.
  memchr plus a reserve: `OpenFile::ResetStateFromText` **2.64 -> 0.34 ms**.

### Three allocation gates were measuring the wrong thing

`typing_large_file`, `scroll_large_file`, `multi_tab_cycle`,
`large_file_open_first_paint`, `large_file_open_lf_first_paint` and
`large_file_restore_deep_scroll_first_paint` all open a project and measure
a small amount of work on top of it. Only the first iteration on a fresh
driver pays the cold open (file-index build, initial watch batch, session
write) -- `typing_large_file` ran 5691, 392, 391, 390, ... -- and that one
sample governed p95/max. The committed baselines recorded it: p50 390
against p95 3305. A percentile that far from the steady state tracks which
iteration index the cold pass landed on, not the tail of the measured work,
and the gates flapped between runs of an unchanged binary. All six now set
`warmup_iterations = 1`; the rebaselined p95s sit within 1% of their p50s.
`cold_startup_large_project` is the scenario that measures the open on
purpose.

### Still open

- **The fold model still rescans the whole document per keystroke.**
  `ScanIndentRanges::Measure` is 0.74 ms of it and is now the largest single
  piece; it needs the scan to stop being whole-document, not a faster inner
  loop. The bracket tail is the same shape. The real fix is VSCode's
  incremental bracket-pair tree (reuse unchanged subtrees across an edit),
  which is a rewrite of `FoldingModel`, not a constant-factor change.
- **`persistence::WriteFile::DurableWrite` is still the largest main-thread
  scope in the smoke suite** (108 ms). Unchanged from the sixth round: the
  cost is the fsync, and deferring it needs the `SaveX() == "bytes are on
  disk"` contract changed deliberately. See that round's entry.
- **`TextLayoutCache::BuildVisualLineColumns`** is 1.4 ms per open of the
  50k fixture (it measures every line's visual width to find the longest
  one, for the horizontal scrollbar extent). Incremental after an edit,
  whole-document on open.
- **Compiling a definition's rules now costs ~1.3 ms** instead of ~0.7 ms,
  because of the ASCII twins. It is once per filetype per session, inside
  the first frame that highlights such a file. Making the *Unicode* copy the
  lazy one would recover it but needs a second per-definition `once_flag`.

## 2026-08-01 measurement pass, sixth round (perf-runner-v1)

Two findings and one rejected change. The rejected one is the more useful entry.

### Fixed

- **Indent guides were sorted every frame.** `ComputeIndentGuides` emitted one
  entry per (visible row, guide column) and `std::sort`ed the lot so a coalescing
  pass could merge neighbours into vertical runs. The entry count is rows x indent
  levels -- thousands per frame on deeply indented content -- and the guides are
  recomputed on every scrolled frame by design, since the cache key includes the
  scroll line. The step had no scope, so it sat inside the renderer's self time;
  scoped, it was the largest single thing in the editor's render path, and the
  sort was **15.6 ms of its 17.0 ms** across a 372-frame scroll. Sweeping one
  guide column at a time produces the runs already grouped and already in row
  order, so there is nothing to sort: **46 -> 4.2 us per frame**.
- `LeadingVisualIndent` (called per visible row by the above) now shares
  `util::LeadingByteRun` with the fold scan's `MeasureIndent` -- the same
  leading-whitespace scan had been written twice.

### Rejected: deferring persisted-record writes to a background thread

`persistence::WriteFile` was the largest main-thread scope in the smoke suite
(123 ms across the run). Splitting it settled what it is: the durable write is
**1.58 ms of a 1.60 ms write (97.6%)**, up to 6.7 ms; the backup rotation and
rename together are 0.038 ms and the path/directory work 0.0095 ms. So the cost
is the fsync, and the only levers are off-thread, less often, or not at all.

A serial background writer was implemented -- queue, worker, flush before every
read so a load could never see a stale file, flush at the shutdown seam that
already flushes recents. It was **backed out**, because the test suite said
something the design had assumed away: `SaveX()` returning true currently *means*
the bytes are on disk. `PersistenceServiceSkipsRewritingIdenticalState` checks
`std::filesystem::exists` immediately after a save, and three
`WorkspaceShell/...AfterRestart` tests model a real user flow -- change a
setting, restart -- by reading with a fresh shell while the old one is still
alive. Flushing before reads *through the same service* does not cover either.

The observers of that contract are not enumerable from inside the service (other
instances, other processes, the control channel, a user looking at the file), and
the failure mode for missing one is silent state loss. Deferring is the right
idea and VSCode does it; doing it safely needs the contract changed deliberately
-- every caller audited, `SaveX` documented as "queued", and the flush points
chosen from that audit rather than from where tests happened to fail.

The stage-split instrumentation is kept, so the next attempt starts from the
number rather than from a guess.

### Still open

Unchanged from the fifth round: the fold model still rescans the whole document
per keystroke (bracket tail scan, indent measure, range merge -- roughly 3.7 ms
on the 50k-line fixture), and an edit at line 0 cannot resume at all. Beyond
that, `RuntimeSyntaxRegistry::HighlightLine` is ~5.6 us per line and runs one
PCRE2 execution per pattern rule; `ReusableMatchData` does a thread_local
`unordered_map` lookup keyed on the compiled-pattern address for every one of
those executions, which a slot index derived from the rule index would make an
array read (~9% of the highlight path, not attempted).

## 2026-08-01 measurement pass, fifth round (perf-runner-v1)

Continuation of the fourth round's open item: after the line-0 fixes, the fold
model was essentially the entire cost of editing a large file. It rescans the
whole document on every keystroke, and this round is about how much of that
scan was avoidable work rather than about stopping the rescan (still open).

### Fixed

- **The indent measure counted leading whitespace one byte at a time.**
  `MeasureIndent` runs for every line of the document on every fold recompute.
  On shallow code that is nothing; on deeply nested code it is the whole scan --
  the 50k-line fixture averages 131 leading whitespace bytes per line, and the
  pass cost 2.30 ms per keystroke against 0.25 ms when a probe read only each
  line's first byte. Leading spaces are now counted eight bytes at a time
  (`util::LeadingByteRun`); a tab still falls into the exact per-character rule
  from where it appears. **2.30 -> 0.63 ms**, and
  `mid_file_edit_latency_large_file` 204 -> 136 ms.
- **The "incremental" bracket resume walked the whole document anyway.** It
  avoids re-emitting ranges for the lines before the edit, but still walked every
  byte of them to rebuild the bracket stack -- half a document per keystroke on a
  mid-file edit, and neither half of the resume had a scope, so it had never
  appeared in a ranking. Consecutive keystrokes in one place resume at the same
  line, and an edit at or after a line cannot change a byte before it, so the
  stack is reusable while the resume line is unchanged. That is sound precisely
  because the resume line is the *minimum* line touched since the last refresh:
  an earlier edit lowers it and the memo stops matching.
  **BuildBracketStackPrefix 120 calls / 176 ms -> 1 call / 1.4 ms**; scenario
  136 -> 126 ms.
- **A sequential line walk cost a tree descent per line.** `PieceTree::LineView`
  needs the start of line `i` and of `i+1`; the one-entry memo can only ever hold
  one of them, so an ascending walk paid a descent plus a binary search per line,
  and then a *second* descent inside `TryViewRange` to turn the byte range back
  into a view of the piece it had just located. Both are O(1) now, keyed off the
  node the last resolved newline sat in: measured 2,502,600 O(1) steps against
  2,544 descents, and 2,502,744 in-node views against 312. Kept despite a wall
  win inside the noise band, because it is strictly less work on the editor's
  hottest read primitive; the remaining per-line cost is call overhead, not the
  lookup.

### A measurement mistake worth repeating back

The first attempt at localising the indent scan replaced
`MeasureIndent(lines[i], ...)` with a **constant** and concluded that 99.6% of
the pass was the line lookup. The compiler hoisted the constant out of the loop,
so that experiment measured a memset. The correct probe keeps the call and
changes only what it does with the result (`lines[i].empty()`, then
`lines[i].substr(0, 1)`), which said the opposite: the lookup was ~10% and the
byte counting was ~90%. **An ablation that lets the compiler delete the
surrounding work measures nothing.**

The same lesson applied to the tests: the first version of both fold-memo
regression tests PASSED against deliberately broken code, because the two
bracket stacks being compared happened to coincide. They now alternate resume
lines between different bracket depths and give the two documents brackets
opened at different lines.

### Still open

- **The fold model still rescans the whole document on every keystroke.** What
  is left after this round is the bracket tail scan (~2.4 ms), the indent measure
  (~0.63 ms) and the range merge (~0.65 ms) -- roughly 3.7 ms per keystroke on
  the 50k-line fixture. The remaining structural fixes are the ones named last
  round: checkpointed bracket stacks (the syntax highlighter already does exactly
  this for its state chain, in the same file), and VSCode's approach of computing
  fold ranges behind a debounce while shifting the existing ranges by the edit
  delta.
- **An edit at line 0 cannot use the bracket resume at all** -- there is nothing
  before it to keep -- so it rescans fully every keystroke and
  `first_line_edit_latency_large_file` sits at 188 ms against the mid-file
  burst's 134. Confirmed rather than assumed: `BuildBracketStackPrefix` records
  zero calls in that scenario. Only checkpointing would help here.
- Unchanged from earlier rounds: the workspace-session durable write, the
  `ReusableMatchData` hash lookup per regex execution, `ResolveGitBaseReference`'s
  six spawns on the shell thread, and the synchronous blob loads in
  `compare_scroll_selection`.

## 2026-08-01 measurement pass, fourth round (perf-runner-v1)

Theme: **the same edit costs wildly different amounts depending on where in the
file you make it**, and nothing measured that. Every editing scenario in the suite
typed somewhere in the middle of the file, so a whole class of first-line
pathologies was invisible.

### Fixed

- **Editing line 0 rebuilt every line's visual width.** `InvalidateDerivedCaches`
  has a dedicated branch for a content edit anchored at line 0 — line 0 genuinely
  is special for the highlight state, because the syntax state chains forward from
  it — and that branch dropped the per-line visual-column table along with the
  visible-line layouts. The next `MaxVisualColumns()` then recomputed the width of
  every line in the buffer, per keystroke. A line's width depends on that line's
  bytes and nothing else, so line 0 is not special here, and the incremental
  update every edit path already runs handles it. On the 50k-line fixture an
  identical enter/backspace burst measured **121 full rebuilds (269.7 ms) at line 0
  against 1 (3.1 ms) at line 25000**.
- **The same branch cleared two 50k-entry vectors** that `EnsureHighlightCaches`
  resized straight back, value-initialising ~50k `SyntaxState`s (2 MB) per
  keystroke. The non-zero-start branch a few lines below already documents the
  right answer ("drop the validity cursor instead of looping SyntaxState{} into
  ~50 000 entries"); the two had drifted.
- **A heap allocation per keystroke in the width update.** `UpdateVisualColumnCacheAfterEdit`
  built a fresh vector for the inserted-line widths — usually to hold one value.
  Mostly hidden before, because an edit at line 0 bailed out of that function
  early; with line 0 on the incremental path it showed up immediately as +22,710
  allocations on the snippet mirror scenario.
- **The indent fold scan re-derived "is this line blank" for every line.**
  `ScanIndentRanges` measures every line's indent into an array, then walks that
  array; the walk asked `LineIsBlankOrIndentOnly(lines[i])` — a piece-tree line
  lookup plus a byte scan — for a question the measure pass had already answered
  (`MeasureIndent` returns its sentinel *exactly* when the line is all spaces and
  tabs). The check could never fire. Emission pass **227.0 -> 8.9 ms** across the
  mid-file burst; `mid_file_edit_latency_large_file` p50 wall 245 -> 205 ms.

`first_line_edit_latency_large_file` overall: p50 wall **277.9 -> 169.1 ms**,
shell-thread allocations **57,123 -> 19,147**. It is now faster than the mid-file
burst rather than dramatically slower.

### Instrumentation added

- `first_line_edit_latency_large_file`, the mirror of the existing mid-file
  scenario. The two differ only in the line they edit, which is the point: a
  position-dependent cost is invisible without a differential pair.
- `FoldingModel::ScanIndentRanges::Measure`, splitting the per-line measure from
  the range walk. That split is what showed the walk was 40% of the scan for no
  reason.

### Still open — and this is now the dominant editor cost

**The fold model rescans the whole document on every keystroke.** After the fixes
above it is essentially all that is left of a large-file edit: on the 50k-line
fixture the mid-file burst spends ~320 ms of its ~205 ms/iteration budget in
`ScanIndentRanges` alone (2.6 ms per keystroke), and the first-line burst adds a
full bracket rescan on top (the bracket scanner has an incremental resume path,
but it requires a resume line > 0).

Two measurements worth carrying into that work:

1. **99.6% of the indent measure pass is the line lookup, not the measuring.**
   Replacing `MeasureIndent(lines[i], ...)` with a constant took the pass from
   2.58 ms to 0.011 ms per call. `PieceTree::LineView` memoizes a single line
   index, and its own comment explains it leaves the memo holding `index + 1` for
   the next ascending call — but each call also asks for `index + 1`'s start,
   which misses, so a sequential walk pays one full tree descent per line. A
   cursor or a range-visiting API (`ExtractLineRange` already does one pruned
   walk for many lines) would make this O(bytes) instead of O(lines · log n), and
   it would pay off well beyond folding.
2. **The scan should not be running at all on most keystrokes.** VSCode computes
   fold ranges asynchronously behind a debounce and shifts the existing ranges by
   the edit delta meanwhile. The pieces are already here — `ConsumeFoldEditAnchorLine`,
   `computed_line_count_`, and `RemapCollapsedFlags` — so the missing part is
   deferring the recompute rather than a new mechanism.

Also still open from earlier rounds: the workspace-session durable write, the
`ReusableMatchData` hash lookup per regex execution, `ResolveGitBaseReference`'s
six spawns on the shell thread, and the synchronous blob loads in
`compare_scroll_selection`.

## 2026-08-01 measurement pass, third round (perf-runner-v1)

This round started from a gate failure rather than a ranking, and the failure turned
out to be about the instrument, not the code. The theme: **a regression suite that
cannot fail is worth less than one that fails honestly**, and the metric worth gating
is the one that is reproducible.

### The suite was not detecting anything

- **Most committed baselines were 3–8x stale, in the vacuous direction.** Gates only
  trip on *increases*, so a scenario measuring 79 ms against a 185 ms baseline passes —
  silently, forever. A full gated run compared against the committed set showed the
  majority of scenarios at 10–30 % of their baseline wall and a small fraction of their
  baseline allocations.
- **A bare `--update-baseline` could not finish.** It swept in every registered
  scenario and returned 1 on the first advisory-only one it reached, leaving every
  baseline after it untouched. That is a good part of how the drift accumulated. It
  skips advisory scenarios now and fails only when one is named explicitly.

### The allocation counter was measuring the wrong thing

The counting `operator new`/`delete` used process-global atomics. Every consumer
snapshots and deltas on one thread and asks a question about *that thread's* work, so
the editor's background file-index builds, tree walks and git were being charged to
whichever measured iteration the scheduler ran them in. `cold_startup_large_project`
reported a p50 of 399 allocations on one run and 1749 on the next from identical
binaries; `scroll_large_file` 4410 and 10580. No percentage tolerance covers a 4x swing
without being meaningless.

Counting **per thread** makes the number deterministic and makes it the number that
matters — allocation on the shell thread is what costs a frame. Across three full gated
runs of one unchanged binary the allocation gate never fired once. It also replaces an
atomic read-modify-write with a plain thread-local increment on the hottest path in the
process, and it makes the "this frame must not allocate" tests mean the frame rather
than the whole process.

That, in turn, allowed the gating policy to become honest: **wall loose (100/150/200 %),
allocations tight (10/20/50 %)**. The same three runs produced 0, 3 and 0 wall failures
against baselines captured from that binary, overshooting +150 % on the loaded run —
with allocations byte-identical in every one of those failures. A 10 % wall gate on this
runner is a coin flip. What the policy gives up (a constant-factor wall regression under
~2x) is already `tools/perf-compare.py`'s job, where shared load cancels.

A scenario whose *allocation* count is not reproducible is now treated as a scenario
bug, not a tolerance problem: `file_finder_cold` waits for the file index instead of
letting the finder-cache rebuild land in a random subset of its iterations.

### Fixed (product code)

- **The file finder's cache rebuild was half redundant.** It rebuilds an entry per
  indexed file whenever the index version moves — so any file change makes the next
  finder open pay it — and each entry stored the same path four times: a
  `std::filesystem::path`, its string, the folded string, and a separately folded
  filename. The path is only read when a (capped) result row is materialized, and the
  folded filename is a *suffix* of the folded path, so it is an offset. A `path -> index`
  map over every indexed file, existing only so the empty-query recents lookup was O(1),
  was inverted to a map over the recents (tens of entries) scanned against the index
  once. 81,914 → 41,913 allocations on the 10k-file fixture, plus a
  `std::filesystem::path` and a `std::string` per file off the resident cache.

### Instrumentation added

- **Every subprocess spawn now names its subcommand.** `RunSubprocess(program=git)` was
  one row for every git invocation; it reported 37 ms of shell-thread time in the git
  sidebar refresh and could not say which call that was. It resolves into status 22.4,
  diff 6.5, symbolic-ref 3.8, show-ref 2.9, config 1.5. Bounded to short lowercase
  `[a-z0-9-]` arguments so paths, revisions and object ids cannot blow the label cap.
- `git::ResolveBaseReference`, which chains up to six spawns to answer one question.

### Still open

- **`ResolveGitBaseReference` runs on the shell thread**: one call, 7.98 ms, entirely
  main-thread, in `git_sidebar_refresh_large_repo` — almost all of it process-spawn
  cost. `GitRepositoryService::ResolveOutgoingBaseCached` memoizes it against
  `(root, head_oid, branch, upstream)`, so the cost is per cold open / per HEAD move,
  but it is paid where the user waits.
- `git status` shows 16–22 ms of *main-thread* time in the git sidebar scenarios even
  though the refresh is supposed to be asynchronous. Worth attributing before assuming
  the async path covers every caller.
- Unchanged from the previous round: the workspace-session durable write, the
  `ReusableMatchData` hash lookup per regex execution, and the synchronous
  `git show`/`cat-file` blob loads in `compare_scroll_selection`.

## 2026-08-01 measurement pass, second round (perf-runner-v1)

Driven by the ranked trace summary again. The theme this time is that **a wall-time
number in a scope is not evidence that the code in that scope is slow** — three of the
findings below are cases where the traced call was innocent and the surrounding
measurement was lying, and one of them turned out to be a blocking syscall nobody would
guess is blocking.

### Fixed

- **A fold model attaching wiped every layout cache, then immediately rebuilt one.**
  `TextViewport::SetFoldingModel` called `InvalidateVisualColumnCache()`, which drops the
  per-line visual-column table, the visible-line layout LRU, and the wrapped-row layouts.
  Its very next statement, `ClampScrollState()`, reads `MaxVisualColumns()` — so
  attaching a model forced a full O(lines) rebuild of the width table, ~10 ms of
  shell-thread time inside a prepared frame on the 50k-line fixture, paid the first time
  a fold scan resolved any range. Folds change which lines are *visible*, never how wide
  any line is; the one fold-dependent product (the wrapped-row layout) is already keyed
  on `(folding_model, fold_revision)`.

- **Every line's width was computed one UTF-8 code point at a time.**
  `VisualColumnForTextColumn` walked each line with a `Utf8SequenceLength` call and a
  tab-stop branch per character, even though a byte below 0x80 that is not a tab always
  contributes exactly one column. `TextLayoutCache::MaxVisualColumns` runs it over every
  line of a buffer on open. Skipping to the first byte that can break that assumption,
  eight bytes at a time, took `BuildVisualLineColumns` from 11.4 to 1.7 ms per call on
  the 50k-line fixture.

- **Opening a file walked its contents four times.** A byte-at-a-time line-ending
  classification, a `memchr` for NUL, an all-ASCII scan, and a byte-at-a-time LF
  normalization. An LF-only file is now decided by one `memchr` for CR (no CR means no
  CRLF and no CR ending, so nothing else can change the verdict); the NUL and non-ASCII
  scans are one pass; and normalization copies the runs between carriage returns
  (0.69 → 0.17 ms on the 1 MB mixed-endings fixture). The eight-bytes-at-a-time scan is
  shared with the line-width fast path above as `util::FirstNonAsciiOrByte`.

- **`close()` on an inotify descriptor blocks for milliseconds, at random.**
  This is the one worth remembering. Releasing an fsnotify group makes the kernel wait
  for an SRCU grace period before its marks can be freed, so closing an inotify fd
  sporadically costs multiple milliseconds *regardless of watch count* — a standalone
  20-line C program that creates a descriptor with a **single** watch and closes it
  measures 0.02 ms most iterations and 3–6 ms on the rest. A project switch retires two
  such descriptors (the file-index watcher and the project tree watcher's native
  backend) and closed both inline on the shell thread. They now go through
  `platform::RetireDescriptorAsync`. `multi_project_switch` p50 wall 79 → 23 ms;
  `SetProjectFileMonitorRoot` 98.8 → 0.3 ms and `StopFileIndexWatcher` 56 → 1.3 ms across
  the measured iterations.

  Two wrong turns on the way there, both worth not repeating: the per-watch
  `inotify_rm_watch` loop that ran before each close was removed (redundant — closing the
  descriptor releases every watch on it) and did **not** explain the cost; and a
  size-thresholded offload was written and thrown away after the watch count went into
  the trace label and showed the fixtures hold about twenty watches while a descriptor
  with *one* was just as slow. Put the count in the label first.

### Instrumentation added

Every one of the fixes above came out of a scope that did not exist before this pass, so
this is not incidental:

- The three steps of the per-frame fold refresh (`RefreshEditorFoldingModels` was the
  largest main-thread scope in the suite and held all of it in *self* time), the range
  merge inside `ComputeWithBudget`, and the two halves of `MaxVisualColumns`.
- The twelve subsystem paints `WorkspaceRootView::Render` dispatches to. It was the
  second-largest main-thread scope, also entirely self time, and could say only
  "painting is slow".
- The four stages of `TextViewport::OpenFile` and the five steps of
  `StoreCurrentProjectState`.
- Watcher teardown: `Unwatch` split into native/poll, each thread join named, and the
  inotify close carrying its **watch count** in the label — that count is what falsified
  the "kernel is freeing a big watch table" theory.

### Gate maintenance

- **Three permanently-red gates now mean something.** `debug_value_tree_paging`,
  `debug_breakpoints_model_rebuild` and `debug_pane_hittest_geometry` had failed their
  wall baselines across two measurement passes on unmodified code. They now use the
  decoupled tolerances the tech-debt coverage scenarios already use (allocations tight at
  10/20/50 %, wall wide at 75/250/400 %), and `debug_pane_hittest_geometry` repeats its
  sweep 100× because one sweep resolved in ~0.1 ms — below anything this runner can time.
- **Most committed baselines were stale in the vacuous direction.** Comparing a full
  gated run against `tests/perf/baselines/`, the majority of scenarios measured at
  10–30 % of their committed wall p50 and a small fraction of their allocation counts —
  `multi_project_switch` at 79 ms against a 185 ms baseline, `switch_and_idle` at ~90 ms
  against 265 ms. Regression gates only trip on increases, so those scenarios had been
  passing against numbers no longer connected to the code, and would not have caught a
  3× regression. Rebaselined.

### Still open

- `persistence::WriteFile(file=workspace-session)` is now the dominant main-thread cost
  of a project switch: 1.9 ms average and up to 26.8 ms for one durable write (temp +
  fsync + backup rotation + rename), once per switch. `RecentsService` already
  establishes the coalescing pattern (750 ms window + shutdown flush) and it would fold a
  burst of switches into one write — but a single switch would still pay one write, so
  the win is mostly on the benchmark rather than for a user. The fsync itself should stay:
  the write is what makes the atomic rename meaningful.
- `RuntimeSyntaxRegistry::HighlightLine` is ~5 µs per line and runs one PCRE2 execution
  per pattern rule per line. `ReusableMatchData` does a `thread_local unordered_map`
  lookup keyed by the compiled-pattern address on every one of those executions; a stable
  slot index into a per-thread vector would make it an array index. Not attempted —
  measured at a few percent of the highlight path.
- Unchanged from the previous round: `compare_scroll_selection` still runs `git show` /
  `cat-file` synchronously on the shell thread, and
  `ProjectCatalogService::LoadProjectState::SetProjectFileMonitorRoot`'s re-arm is
  deferred while its teardown is not (though the teardown is no longer where the time
  went).

## 2026-08-01 measurement pass (perf-runner-v1)

Driven by the ranked trace summary again, starting from a full-suite wall-time ranking. Two of
the five findings are cases where the thing being measured was not the thing that was slow, which
is the recurring theme: the ranking is only as good as the scopes under it.

### Fixed

- **Every subprocess spawn slept up to 5 ms after its output was already read.**
  Once `PumpChildIo` drained the pipes, `RunSubprocess`'s deadline loop probed the child with
  `waitpid(WNOHANG)` and, if it was not reapable yet, slept a flat `poll(nullptr, 0, 5)`. The
  comment justified that with "the child exits when its stdio closes, so the first WNOHANG
  catches it with no sleep" — a race the parent usually loses, because the kernel closes the
  child's descriptors partway through exit. It now waits on a **pidfd**: one poll that returns
  exactly when the child is reapable, no sleep, exact timeout. (No-pidfd kernels keep the probe
  loop at 1 ms.) This is on every git invocation the editor makes, several of which still run
  synchronously on the shell thread. `compare_scroll_selection`: git spawn 7.82 → 5.10 ms
  average, main-thread git time 145.7 → 106.5 ms, p50 wall 499 → 428 ms.

- **Glyph composites were built in a pixel format the renderer does not store.**
  Composites were `RGBA32` (ABGR8888 on little-endian); the software renderer's textures are
  ARGB8888. `SDL_CreateTextureFromSurface` therefore converted the whole bitmap channel by
  channel on **every** cache miss — and a scroll through unseen content misses on essentially
  every string it draws. That upload was 605 ms of the 2035 ms a 640-page-down sweep over a
  50k-line file spends, three times the cost of rasterizing the glyphs. Building the composite
  (and the coverage atlas it is assembled from) in the renderer's own format makes the upload a
  copy: 605 → 509 ms. Output is unchanged — the conversion was lossless, just work.

- **The compare tab reclassified and re-laid-out itself on events that changed nothing.**
  `RefreshCompareTabDerivedState` carries a fingerprint whose whole purpose is to skip whole-file
  work, then ran the two most expensive things it does unconditionally: the semantic
  classification (which serialized the entire right viewport, copied the entire left string, and
  scanned both for NUL bytes / a submodule pointer / a line-ending-only difference — that last
  one allocating two more whole-file strings) and the full presentation row rebuild. Both are now
  gated on their real inputs, the classifier takes views instead of owned strings, and the
  line-ending comparison walks both buffers normalized without materializing anything.

- **A state file was rewritten durably even when it already held those bytes.**
  Every persisted-state save is a temp write + fsync + backup rotation + rename on the shell
  thread, and a project switch does three of them at ~1.1 ms each — mostly re-encoding state that
  had not changed. `PersistenceService` now remembers the body each path was last observed to
  hold (set on load *and* on save) and skips a save whose body equals it, gated on the primary
  file still existing. `multi_project_switch`: project-config writes 29 → 5, project-session
  writes 29 → 5, main-thread persistence time 98 → 54 ms.

- **Per-frame answers in the status bar that move per project.** `root.lexically_normal()` and
  `viewport->path().lexically_normal()` allocated a path every frame; a scan for "does the
  worktree have changes" walked every git sidebar entry every frame (1000 iterations/frame on the
  1000-changed-file fixture). Both keyed on what actually changes them now.

### Harness fixes found on the way

- The four temp-tree compare/merge scenarios rebuilt their fixture **inside the measured
  iteration**: a 1 MiB seed file rewritten two or three times, plus `git init` + two `git config`
  + `git add` + `git commit` for the git-backed ones. On `compare_scroll_large_fixture` the
  ranking put 190 of the ~250 ms of main-thread time in `RunSubprocess(program=git)` — and only 4
  of those 24 spawns were the application's. The scroll burst the scenario exists to measure was
  under a quarter of its own number. Fixtures are now built once per process and shared.

### Instrumentation added

Each of these existed because the ranking bottomed out somewhere uninformative:

- `EditorViewRenderer::Render::Rows` held 46% of the page-down scenario in *self* time with
  nothing under it. Split into the per-row layout resolve, the decorated-row assembly, and the
  gutter line number — and, inside the text backend, `RasterizeString` vs `UploadStringTexture`.
  That split is what located the pixel-format conversion.
- `WorkspaceShell::RefreshCompareTabDerivedState` and its model rebuild had no scope at all, so
  the compare refresh cost above did not appear in any table.

### Still open

- The gutter line number costs **2.4x the entire decorated text row it sits beside** (0.0105 ms
  vs 0.0049 ms). It is a separate whole-string composite per row, and on a page-down sweep every
  number is a fresh cache miss. Drawing it per digit from cached single-digit entries would make
  it allocation- and upload-free, but digit origins would snap to the device grid independently
  rather than as one string, which risks 1 px spacing jitter. Not attempted.
- A texture-recycling pool over the string cache was implemented and **removed**: evicted
  textures matched an incoming composite's exact (width, height, format) only 4% of the time,
  because composite width tracks string length continuously. Bucketing widths would raise the
  hit rate at the cost of padding every composite; not attempted.
- `ProjectCatalogService::LoadProjectState::SetProjectFileMonitorRoot` is ~1.8 ms of synchronous
  shell-thread time per project switch. The re-arm is already deferred to a worker
  (`ArmPendingWatch`, 0 main ms); the teardown half is not.
- Unchanged from the previous pass: `compare_scroll_selection` still runs `git show` /
  `cat-file` synchronously on the shell thread to load blob content.
- **Four committed wall baselines no longer hold on this runner, and did not before this pass
  either.** `debug_pane_hittest_geometry`, `debug_value_tree_paging`,
  `debug_breakpoints_model_rebuild`, and `mid_file_edit_latency_large_file` measure 40–120% over
  their committed p50 — including on an unmodified checkout of the previous commit, built and run
  in a worktree for exactly this comparison. Allocation counts are byte-identical to baseline in
  every case, so nothing algorithmic moved. Repeat runs of one binary swung
  `debug_pane_hittest_geometry` across 0.148–0.225 ms and `debug_value_tree_paging` across
  3.6–6.8 ms, which is far outside their 10%/20% tolerances: these micro-benchmarks measure
  sub-15 ms of work and the runner cannot hold that envelope any more. Deliberately **not**
  rebaselined here — capturing under this jitter would bake it into the committed numbers. What
  they need is either a quieter measurement environment or wall tolerances decoupled from their
  (still exactly deterministic) allocation gates, the way the tech-debt coverage scenarios
  already are.

## 2026-07-31 measurement pass (perf-runner-v1)

Driven by the ranked trace summary, which the perf harness could not print until this
pass: `MICROIDE_PERF_SUMMARY=1` folds every scope into a self-time ranking, but nothing in
`microide_perf` called `DumpSummaryOnce`, so the env var silently did nothing there and the
ranking was reachable only by driving a live session by hand. `PerfHarness::RunScenario`
now resets the aggregate after warmup and writes the table per scenario. Every finding
below came off the first three tables it printed.

### Fixed

- **Inline blame re-spawned git at frame rate for files it cannot answer for.**
  `GitBlameService::Request` runs once per rendered frame (the overlay is built from the
  render path). Its re-validation throttle required an *eligible* cache, so untracked /
  ignored / unborn-HEAD files fell through it every time and re-ran `git rev-parse`, plus
  `ls-files` and `status` once HEAD resolved. Ineligibility does not depend on the window,
  so the throttle now covers it, keyed additionally on the (file, clear) generation so a
  real edit still re-probes at once. A second half: the pre-blame verdicts were being
  *discarded* when a scroll superseded the request after the probes had already run, so a
  continuous scroll never wrote the cache entry the throttle needs — those verdicts are
  about the file, not the window, and now record on generation currency alone.
  `compare_scroll_selection`: git spawns 98 -> 42, traced main-thread time 204 -> 116 ms,
  p50 wall 181 -> 123 ms.

- **The fold scan re-walked its whole prefix on every extension.**
  `ComputeWithBudget` always rescans `[0, scan_end)`; its incremental path serves a
  localized edit, not a forward extension. Extending by one look-ahead window at a time
  therefore made scrolling a large file quadratic — 108 extensions averaging 2.35 ms on the
  50k-line fixture. The resolved prefix now grows geometrically: O(log n) extensions,
  O(n) total work, and no change to the worst-case single hitch (the largest scan under
  either policy is the one reaching the end of the file). The win is in the tail, where a
  scroll hitch lives: `editor_sticky_scroll_scroll` p95 252 -> 157 ms, max 330 -> 159 ms;
  `editor_fold_recompute` p50 88 -> 78 ms.

- **A durable disk write per file open.** `RecentsService` saved the MRU inline after every
  record — a temp + fsync + backup + rename at ~1.35 ms on the shell thread. Justified in
  its header by "opens are user-paced", which is true of a click and false of session
  restore, multi-select open, file-finder browsing, or anything scripted. Writes now
  coalesce over a 750 ms window with a shutdown flush (the only reliable point — the
  process leaves via `quick_exit()`), and re-recording the already-newest entry returns
  before touching anything. `multi_tab_cycle` p50 114 -> 92 ms, traced main-thread time
  104 -> 31 ms.

- **`JsonObject` was a hash map.** One node allocation per member plus a bucket array per
  object, on paths that are nothing but small objects (a DAP `variables` response is
  hundreds of four-key objects). It is a flat vector sorted by key now: one allocation per
  object, binary search over contiguous memory for lookup, a straight vector compare for
  the key-order-independent equality the map gave for free, and deterministic
  serialization. `sizeof(JsonValue)` drops 64 -> 40. The key order is (length, bytes)
  rather than lexicographic — an internal detail that makes most lookup comparisons a
  single integer compare, and worth ~12% on its own.
  `dap_protocol_encode_decode` wall 72.2 -> 41.0 ms and allocations 1013110 -> 330308;
  `lsp_message_framing` 11.2 -> 9.5 ms and 186794 -> 85794;
  `lsp_document_symbols_parse` 95.3 -> 83.6 ms; `lsp_publish_diagnostics_parse` 28.5 -> 26.9 ms.

- **Per-frame filetype detection, twice.** `DetectFiletype` materializes the
  signature-scan head into a `vector<std::string>` and runs the detection regexes. The
  status-bar language segment had grown its own four-field cache inline; the fold refresh
  had none and paid ~0.85 ms per frame on a 50k-line buffer for an answer that had not
  changed. `runtime_syntax::FiletypeMemo` is that cache extracted, held per editor tab so a
  split view with two files does not thrash one entry.

### Harness fixes found on the way

- `project_search_literal` / `project_search_regex` ran `search` — Find in Buffer — not
  project search, against a query present in no file's *content*, then slept a flat 50 ms
  and sampled whatever had happened. Their entire ~52 ms wall was the sleep and their
  allocations were the background index build at a random point, which is why the committed
  baselines held p50=193 against max=166987. They now run `project-search` over a query
  with real coverage and drive both async stages to fixed states; allocation counts are
  exactly reproducible run to run.
- Trace coverage added where the ranking could not localize: the steps inside
  `PrepareFrameOnce`, the two fold scans, and the state file behind each
  `persistence::WriteFile`.

### Still open

- `compare_scroll_selection` still spawns `git show` / `cat-file` **synchronously on the
  shell thread** to load blob content for the diff (~17 ms per three iterations). Moving
  that behind `ProjectBackgroundExecutor` is the remaining main-thread git cost there.

## 2026-07-04 measurement pass (perf-runner-v1)

Full-suite ranking (10 iters, software renderer) plus targeted `perf-compare` runs. No hardware
CPU sampler was available in this environment (`perf_event_paranoid=4`, no passwordless sudo, no
valgrind), so localization used `perf_counters` + `perf-compare` wall/alloc deltas, not flamegraphs.

What the ranking showed:

- Rank scenarios by their **measured interactive phase** (not total `wall_ms`, which is dominated by
  50k-line file-open setup). The top interactive costs are the compare/merge scroll bursts
  (`compare_scroll_selection`, `merge_accept_hunk_interleaved`, `merge_scroll_interleaved_hunks`,
  `merge_scroll_large_fixture`, `compare_scroll_large_fixture`) at ~4.5 ms/frame, **low-allocation
  (6–8k)** → CPU/render-bound, not malloc-bound.
- These frames spend their time in cached-glyph **texture blitting** under the software renderer
  (~350–560 `text_texture_cache_hits`/frame at ~99% hit) plus ~400 `MeasureWidth`/frame (also ~99%
  cache hit). Both are already cached; there is **no cheap CPU win** on these paths. The GPU-gated
  batched glyph atlas (shipped 2026-06-28) is the real lever here but is disabled under the software
  renderer the gate pins.
- `editor_scroll_only_no_content_bump` is a guard test — it asserts scrolling bumps zero
  content/syntax/layout revisions and passes; its 50k `invalidate_derived_caches_lines` are all
  file-open setup outside the measured scroll window (not a scroll-path regression).
- `compare_scroll_selection` is bimodal across iterations (≈550 ms vs ≈1300 ms) — a stateful/noisy
  outlier, not a clean per-op signal.

Shipped:

- **Plugin buffer-lifecycle subscriber gate** (`PluginHost::OnBuffer{Open,Save,Close}`): added
  `buffer_open`/`buffer_save` interest flags (computed in `ComputeEditorEventInterests`, excluded
  from `any()` since they are consulted only at lifecycle-dispatch time) and short-circuited all
  three dispatch sites before `CaptureSnapshot()` + worker-task post when no loaded plugin
  subscribes. Avoids a snapshot + thread handoff per file open/save/close in the common
  plugins-loaded-but-not-subscribing configuration. Behavioral, not isolated by any harness
  scenario (the harness runs with an empty plugin config); guarded by
  `PluginHost/BufferLifecycleInterestGate`.

Measured and rejected (kept here so they are not re-attempted blind):

- **Single-line `BuildRangeHistoryEntry` fast path** (deep-review-plan "B-Hist"): correct and
  byte-identical, but `perf-compare` on typing/multi-caret/edit scenarios showed **zero allocation
  delta and wall within the 2σ noise band** — the per-edit allocations that dominate are
  highlight/view-model, not the history entry. Reverted; not worth the extra branch.
- **Width-cache ASCII fast path** (`TextRenderer::MeasureWidth` → a new backend
  `FastMonospaceWidth`, bypassing the `unordered_map<string,float>` for printable ASCII): the small
  hot map's hit cost ≈ the ASCII-scan cost, and inserting a virtual call before every lookup is a
  slight pessimization for non-ASCII. `perf-compare` across the compare/merge/editor scroll
  scenarios: all deltas within 2σ noise. Reverted.

Still open (documented, not attempted this pass):

- **Persistence record-body deep-copy** (deep-review-plan "B-Body"): `PersistedRecordReader`
  copies the whole record body that consumers only span-read, and `AppendRecord` allocates a
  throwaway per-field vector. Medium/structural, startup-path allocation only.

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

### Release binary switched to LTO (`tools/release.sh`)

Problem:

- The perf harness — and therefore every committed wall/allocation baseline — measures a
  RelWithDebInfo build with interprocedural optimization (LTO) enabled. The distributable, however,
  was configured by `tools/release.sh` as a plain `-DCMAKE_BUILD_TYPE=Release` with **no** IPO, so
  the shipped `.deb` was built with cross-TU inlining disabled. The binary users actually ran was
  slower than the profile the project optimizes and reports numbers against; the hot editor/render
  paths that depend on cross-TU inlining (e.g. `TextViewport` <-> `EditorViewRenderer`) never got
  it in the shipped artifact.

Implemented:

- `tools/release.sh`'s Release configure now passes `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`, so
  the shipped binary is built with the same LTO codegen the perf gate already validates. `CMakeLists`
  downgrades gracefully when the toolchain lacks IPO and auto-skips `ld.lld` under LTO, so the change
  is a no-op on toolchains that cannot honor it. Verified a Release+LTO configure/build links cleanly
  with the workstation GCC and the binary reports the expected version; the release script's existing
  ctest gate covers the resulting codegen.

Measured:

- No separate A/B was run for this change: it is a build-configuration alignment, not a source
  optimization. The win is qualitative — the released artifact now matches the LTO codegen that all
  committed perf baselines are measured under, closing the gap between "the profile we optimize" and
  "the binary we ship." The known trade-off is a slower final link during the release build.

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
- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — cached visible terminal lines

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
- `src/workspace/render/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda
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

- `src/workspace/render/WorkspaceShellRenderFrame.cpp` — two separate `ComputeEditorPaneLayouts` calls
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
- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — terminal cursor render path
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

- `src/workspace/render/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda
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
- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — line 269

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

- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — line 314
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

- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — `draw_terminal_line` lambda, line 156–194

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

- `src/workspace/render/WorkspaceShellRenderBottomPanel.cpp` — `ParseUnsignedStrict`, line 35

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

- `src/workspace/shell/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellInput.cpp`
- `src/workspace/coordinators/WorkspaceTabCoordinator.cpp`
- `src/workspace/coordinators/WorkspaceSidebarCoordinator.cpp`
- `src/workspace/coordinators/WorkspaceCompareInteractionCoordinator.cpp`

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

- `src/workspace/lsp/WorkspaceLspClient.cpp` - async initialization and query synchronization
- `src/workspace/lsp/WorkspaceLspManager.cpp` - server lifecycle management
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
