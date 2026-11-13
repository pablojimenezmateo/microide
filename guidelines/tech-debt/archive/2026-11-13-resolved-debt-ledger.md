# Resolved debt ledger — moved 2026-11-13

Moved from `dev-docs/project/known-tech-debt.md`'s open queue as each entry was
resolved or closed, with its resolution appended. Newest first.

### TD-2026-11-11-443 — the structural refinement is per hunk; moved code is not matched across hunks. [FIXED 2026-11-13]

Each hunk is matched alone, so code moved between two hunks shows as novel in
both. That is also difftastic's behaviour (it has no move detection), and it keeps
the refinement proportional to the change, which the editable compare pane needs
-- it rebuilds on every keystroke. A move pass would match novel subtrees of one
hunk against novel subtrees of others by content id (cheap: the ids are already
interned per hunk; the interner would need to span the build) and mark them as
moved rather than novel, with a presentation of its own.

**Fixed 2026-11-13**, at the line level rather than inside the search: the
refinement stays per hunk (the per-keystroke rebuild needs it), and moves are read
off the finished rows as git's `--color-moved` reads them. `MarkMovedLines`
(`src/compare/CompareMoves.cpp`): a run of whole deleted rows in one hunk whose
text, indentation trimmed, reappears as a run of whole added rows in ANOTHER hunk
is moved (`CompareRow::left_moved` / `right_moved`, `CompareSideChange::Moved`),
given git's minimum of 20 alphanumerics per block; a hash per added line and at
most 16 candidates per deleted line keep it linear. The view tints a moved side
with the new theme colour `diff_moved` (colorscheme key `diff-moved`, falling back
to the scheme's constant colour). Tests: moved block marked on both sides,
re-indented move, lone brace / edited block not moves, the between-hunks rule on
hand-built rows; the fuzz target links it.

Left: a move whose lines were ALSO edited is two novel blocks (the token-level
cross-hunk matcher this entry first sketched would mark the unedited tokens).
The overview ruler marks moved runs in `diff_moved` too
(`CompareReview/OverviewRunsSeparateMovedLines`), and the control channel's
`editor` query reports `movedLeftLines`/`movedRightLines` (QA flow
`diff-moved-code`).


### TD-2026-11-13-445 — residuals of the per-pane diff (TD-439). [FIXED 2026-11-13]

Three paths still route by the FOCUSED group rather than the pane under the
pointer, and are harmless but wrong in a split with a diff or merge open:

- The wheel over an UNFOCUSED diff/merge pane goes to the editor wheel handler,
  which scrolls that group's (hidden) welcome viewport: the diff does not move.
  The compare/merge wheel handlers act on the focused tab; they want a
  "tab under the pointer" parameter, as the editor wheel's pane lookup has.
- Diagnostic and plugin hover cards over an EDITOR pane are not resolved while
  the focused group shows a diff (`DiagnosticHoverTargetAtPosition` returns at
  the compare branch).
- Merge divider drag highlights read the shared `drag_target`, so two merges in
  a split light both dividers while one is dragged (scrollbars take `focused`).

Each wants the hit-test to resolve the pane first and then the tab in front of
it -- the shape the editor pane loop already has.

**Fixed 2026-11-13**, the same day: the wheel resolves the pane under the pointer
and hands a diff or merge there its own tab and surface (the coordinators'
`HandleWheel` take both and scroll through their layout ops, so the three
focused-tab scroll operations they used are gone); `ActiveTabSurface(layout,
shown)` sizes a tab's viewport against the pane that shows it; the hover
resolvers answer for the diff only over its pane and let editor panes answer
beside it; the merge render takes `focused` for its caret, divider highlight and
inline blame (an unfocused merge no longer replaces the visible blame overlay the
hover card reads). Gate: the wheel step in
`WorkspaceShell/CompareInASplitStaysInItsPane` (scrolls the unfocused diff, keeps
focus, partial == full repaint).


### TD-2026-11-11-441 — hunk-local lexing starts in code state; raw strings are not lexed. [FIXED 2026-11-13]

Each hunk side is lexed on its own, starting as code. A hunk that begins inside
a multi-line block comment or string (its opener is on an unchanged line above
the hunk) lexes that text as code tokens. Raw/heredoc strings (Rust `r#"…"#`,
C++ `R"(…)"`, shell heredocs) are not recognised either. Both only change the
granularity of the highlighting -- the tokens are still matched or marked novel,
and the line diff, hunks and staging are unaffected -- and the replaced-literal
word diff absorbs most of it. The proper fix is to carry the lexer state at each
hunk's first line, which needs a state scan of the unchanged text above it (or
the grammar of TD-440); measure the cost against the per-keystroke rebuild of an
editable compare pane before doing it.

**Fixed 2026-11-13.** Both halves.

- Raw strings: `DiffSyntax::raw_strings` (`kDiffRawCpp` `R"d(...)d"` with the
  `u8`/`u`/`U`/`L` prefixes, `kDiffRawRust` `r#"..."#` / `br` / `cr`, raw
  identifiers excluded, `kDiffHeredoc` `<<WORD` / `<<-` / `<<~` / quoted words, and
  the shell-only spaced `<< WORD`). The host names them per language in
  `ResolveDiffSyntax` (`c++`, `rust`, `shell`, `ruby`, `perl`): the language
  contract carries no string syntax.
- Entry state: `DiffLexState` (code, or inside a block comment / triple string /
  backtick string / raw string / here-document, with its delimiter) seeds
  `BuildDiffSyntaxTree`; `ScanDiffLexState` computes it with the same recognisers
  as the lexer in a mode that emits nothing and steps over every byte that cannot
  start a multi-line token (a 256-entry table). The model build carries one scan
  per side down the file, hunk to hunk, so a build reads each line above the last
  hunk once. The scan looks AHEAD past its stop line to find where a token that
  opened above ends: whether a backtick opens a string depends on a closing one
  further down, which the first draft got wrong and the property test caught.
  The state is part of the `HunkRefinementCache` key (delimiter text copied into
  the arena), so a hunk whose text is unchanged but whose context moved into a
  comment is refined again.
- Gates: `StructuralDiff/RegionLexedFromScannedStateAgreesWithAWholeLex` (for
  random text and every split line, a region lexed from the scanned state yields
  exactly the whole lex's tokens from that line on; also stress-run at 20,000
  rounds on two seeds), `RawStringsAreOneToken`, `ScanStateCarriesMultiLineTokens`,
  `EntryStateIsPartOfTheRefinementKey`, and the new fuzz target
  `StructuralDiffFuzz` (model rows cover both sides; the same scan/region
  property; 329k runs clean). Two definitions the property forced: an empty
  here-document body is no token, and a here-document marker on a line that a
  multi-line token carries lexing off is dropped (a region starting inside that
  token cannot know it was pending; no real code does this).


### TD-2026-11-11-442 — no slider correction in the structural diff. [FIXED 2026-11-13]

difftastic post-processes its match (`fix_all_sliders`) so that, when a novel run
could equally sit at two positions (inserting an item into a list whose
neighbours are equal, or a block whose closing brace equals the next block's),
it lands where a reader expects -- the inner position for nested novelty, the
later delimiter for a list item. Here the Dijkstra tie-break decides, which is
deterministic but occasionally highlights `}` of the existing block instead of
the new one. Add the pass over `StructuralMatch` (it only moves novelty between
equal tokens, so the line diff and the row alignment's inputs are unchanged).

**Fixed 2026-11-13**: `FixStructuralSliders` (`src/compare/StructuralSliders.cpp`),
run by `MatchSyntaxTrees` after the search. Two passes per side. Nested: a
novel list wrapping exactly one matched list with the same delimiters gives its
novelty to the inner pair. Sibling: every maximal novel stretch of a sibling
run slides across equal, wholly-matched neighbours (one node at a time, so each
intermediate match stays symmetric and ordered) to the placement ranked best --
whole lines; then fewer lines touched; then ending its line; then the latest.
Only which of two equal nodes is unchanged moves, so the line diff, hunks and
row alignment are untouched; `StructuralDiff/MatchInvariantsOnRandomInput` holds
with it. Found the wrong placements by probing random insertions with the pass on
and off: every difference left was novelty straddling a line break it did not
need (`)b();` split as `b();` + a `)` on the next line). Gates:
`StructuralDiff/NovelRunDoesNotStraddleALineBreakItNeedNot` (fails without the
pass), `NovelCallSlidesOntoItsOwnLine`, and three guards the tie-break already
got right that the pass must keep (appended list item, inner block, whole-line
statement). Cost: linear in tokens x depth per hunk; `microide_diff_bench`
A/B on the four bench files was within the noise of the loaded machine (same
vertex counts).


### TD-2026-11-11-439 — a compare or merge tab in a split draws over the whole editor column. [FIXED 2026-11-13]

Soak `ae1203cffa96`, `ea6f9461e041`, `d211729515a9` (oracle `overlap`, flaky
because a monkey run rarely builds the state): with the editor split into groups,
a compare or merge tab active in one group renders across the whole editor
column -- `RenderCompareSurface` / `RenderMergeSurface` take
`layout.editor_surface`, which is the column, not that group's pane -- while every
group's tab strip and breadcrumb are still laid out and drawn. The other groups'
tab strips then sit on top of the diff, and its buttons (`merge/<n>/accept-incoming`,
`compare/context-<n>/all`) share pixels with their close and scroll buttons; a
click there reaches whichever the hit test meets first. Seen at 1024x700 with a
merge in the bottom of three groups: its toolbar and panes covered the two top
groups.

Not a local fix: about 120 sites in the compare/merge render, mouse, hover,
cursor, scroll and control-ui paths read `layout.editor_surface` as "the view's
surface" (the editor proper already lays out per pane). Two coherent shapes:
render compare/merge inside the owning group's pane surface (VS Code's diff and
merge editors live in their group), routed through one "active tab surface"
accessor so the 120 sites stop naming the column; or have an active compare/merge
tab take the column and leave the other groups' strips out of the layout while it
is shown. The first is the product's direction (splits are first-class); it wants
its own change with the merge/compare QA suites as the gate.

**Fixed 2026-11-13** (branch `td/structural-and-split`), in the shape the entry
named first: a compare or merge tab renders, lays out and hit-tests inside the
owning group's pane.

- `WorkspaceShell::ActiveTabSurface(layout)` (memoized through
  `ComputeEditorGroupRectsForState`) and the shell-free
  `FocusedGroupSurface(layout, split, focused)` are the one "where the focused tab
  lives" accessor; with one group both are `layout.editor_surface`, so the
  unsplit product is unchanged. The compare/merge mouse coordinators take that
  surface instead of the whole `WorkspaceLayout` (they read nothing else of it).
- `RenderActiveWorkspaceSurface` draws every pane's own front tab -- editors,
  and a diff or merge in each pane that shows one (the focused one with its
  caret); frame prep populates syntax tokens per diff pane. A diff in an
  unfocused pane used to show the welcome page. `RenderMergeSurface` /
  `RenderMergeScrollbars` take the merge tab rather than reading the focused one;
  inline blame is built only for the focused diff (two diffs would trade the one
  blame request per frame).
- A click into an unfocused diff/merge pane focuses it first, as an editor pane
  does; the wheel, cursor shape, hover and caret paths read the pane.
- The find widget and the debug toolbar float over the focused pane, not the
  column -- the same bug in a different widget (soak 3cea0cf87f72,
  83dfd7f84e2f: the toolbar on another pane's merge buttons / tab strip). The
  find widget's wheel hit test also read `editor_area` while it paints in
  `editor_surface`; fixed with it.
- The welcome page's click probe laid out against the column while paint uses
  the focused pane; it reads the pane now.
- Gates: `WorkspaceShell/CompareInASplitStaysInItsPane` (pixels of the other
  pane, the unfocused diff still painted, focus by click both ways, partial ==
  full repaint), the four-pane grid added to
  `QaFlowRegression/TheDebugToolbarCoversNoOtherControl` (fails on the old
  anchor), the QA flow `diff-in-split` (4 findings on main's binary, clean
  here), and the hard lint `CheckTabSurfacesLayOutInTheFocusedPane` (no compare /
  merge / find-widget / toolbar layout entry point fed `*.editor_surface`;
  fixtures both ways). Residuals: TD-2026-11-13-445.

### TD-2026-11-10-438 — one ASan shard aborted inside glibc at thread exit. [FIXED]

CI's `asan` job lost `microide_tests_shard_20` on `7d983b374` and again on
`b2c6c6694`, 2.7 s in, to `Fatal glibc error: arena.c:907
(__malloc_arena_thread_freeres): assertion failed: a->attached_threads > 0`,
with no ASan report before it. It never reproduced on the dev PC (glibc 2.43);
an Ubuntu 24.04 container (glibc 2.39, the runner's) reproduced it once in 40
runs of the shard.

Cause, from a gdb trace in that container: `util::ReleaseFreedHeap` --
`malloc_trim(0)`, called by `MirrorSyncEngine::SyncNow` on its `mirror-sync`
workers -- was the process's first call into glibc's own allocator, because
under ASan every `malloc` goes to the sanitizer's allocator instead. glibc 2.39
initializes its allocator lazily and without a lock (`ptmalloc_init`), binding
the calling thread to the main arena; two workers trimming at once were both
bound while the arena counted one, and the second to exit aborted. In a plain
build the main thread's first `malloc`, before any thread starts, has already
initialized it, so this needs a sanitizer's allocator to happen.

Fixed: `util/HeapUsage.h` decides `MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR` (off
under ASan/TSan/MSan), and `ReleaseFreedHeap` / `CurrentHeapUsage` do nothing
there -- glibc's heap holds none of ours in such a build, so trimming or
measuring it answered nothing anyway. The control channel's and the server's
`status` leave out `heap` in a sanitizer build. Validated in the same container:
the gdb trace shows no call into glibc's allocator, and the shard loop no abort.
No unit test fails first: the race is the first call into glibc's allocator,
once per process, on CI's glibc only.

### TD-2026-11-10-437 — CI's qa monkey at scale 1.75: one-step anti-aliasing diffs under the software renderer. [FIXED]

From `c20e96b4e` on, CI's `qa` and `qa-asan` monkey (seed 1; Xvfb, SDL 3.4.2
software renderer, the monkey zoomed to scale 1.75) failed on QA-STALE findings
whose every differing pixel is an anti-aliased glyph pixel off by exactly one
step in one channel: editor text against the find widget's left edge, a tab
label, a tab tooltip (`build/ci-c20`, `build/ci-21f`). `acec16bdd` was green;
the monkey's action list diverges from that run at action 235 (a random
right-click target), so seed 1 is not a fixed path and the runs did not exercise
the same states. Replaying the failing runs locally under `SDL_RENDER_DRIVER=
software` fails rarely (2 of 12 on `c20e96b4e`, 0 of 11 on `acec16bdd` and
`21fbe6554`), so it is timing-dependent, and it does not look caused by
`c20e96b4e` (Code Actions title truncation).

Ruled out, with standalone SDL 3.4.2 programs (scratch, not in the tree): a
clip edge through a blended texture changes no pixel it leaves visible, at scale
1 or 1.75; a texture's first draw and its later ones are identical (no lazy RLE
rounding). Found but NOT the cause: SDL's software blend of white coverage with
a colour mod differs by one step from the same coverage pre-coloured, on almost
every AA pixel, so `DrawEphemeralRuns` (the gutter's line numbers, atlas + colour
mod) and the cached composite disagree -- which `TextRenderer ephemeral runs match
the cached composite path` tolerates as "within SDL's blend rounding". Harmless
today (line numbers always take the atlas path), but a trap: tinting composites
the same way costs ~4.5x per blit on the software renderer (50 vs 11 us per
600x20 run). A per-colour pre-tinted atlas would make the two paths identical at
no blit cost.

Lead for next time: in the local failure the stale frame (993) follows a burst of
`fallback-full` frames and a `present-without-scene` around a window resize --
full frames the verifier does not check. Something painted under a transient
presentation that the later partial frames only partly cover would look exactly
like this.

**Root cause (fixed the same day).** Not the resize: the diffs straddle the
partial frame's clip edges, which at 1.75 sit on fractional device columns
(292.25, 330.75, 822.5). SDL's software renderer turns a copy's destination into
device pixels as `(int)(x * scale)` and `(int)(w * scale)` in float, and the text
backend drew a `w`-pixel glyph run at logical width `w / scale` -- which for many
`w` multiplies back to `w - 0.00001` and truncates to `w - 1`. That made the copy
a NEAREST-*scaled* blit (one glyph column dropped, at every scale that is not a
power of two), and SDL's scaled blit samples and blends differently when a clip
cuts it (`SDL_BlitSurfaceClippedScaled`). So the same line of text painted one
shade off in a partial frame whose damage clip crossed it, and the full reference
render disagreed. The first standalone experiment missed it because it skipped
the two device columns next to the clip edge -- exactly where the diffs are.
`render::DeviceExactLogical` now steps the quotient by ulps until the product
truncates back to the intended pixel, for the origin and the extent of every
glyph blit (`DrawString`, `DrawAsciiRunFromAtlas`); `TextRenderer text blits are
clip-invariant at a fractional scale` failed on the first run it tried ("W" at
x=6, clipped at 7) and an exhaustive standalone sweep (79,032 clip cases) went
from 1,246 differing to 0. The one-step colour-mod remark above still stands as a
separate, harmless difference.
