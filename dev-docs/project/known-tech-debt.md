# MicroIDE Known Tech Debt

Reviewed 2026-08-12. A deep dive on **soft wrap**, prompted by a report that
moving up out of a wrapped line did nothing. It did nothing: wrapped rows are
contiguous in visual columns, so the wrap point is one text position two rows
both answer for, and it always resolved to the later one — so Up onto a shorter
row landed on the row it had just left, at any repeat count, and Down skipped the
short row. Fixed with an explicit affinity bit (VS Code's `PositionAffinity`),
along with the sticky column ignoring the hanging indent, a wrap-width change
leaving the scroll pointing into the old row numbering (and skipping its clamp),
a right-click retargeting the caret by screen column on a continuation row, and
gutter markers repeating down every wrapped row. The perf half: wrapped rows were
rebuilt from scratch every frame and each build re-measured the whole logical
line, and the whitespace-run builder walked its line from byte 0 per row and
stepped one byte per cell. Five entries opened: [185](#td-2026-08-12-185)
(hidden-line row offsets), [186](#td-2026-08-12-186) (the new wrap scenarios need
a quiet-runner baseline), [187](#td-2026-08-12-187) (the fallback whitespace walk),
[188](#td-2026-08-12-188) (Home/End are logical-line verbs under wrap),
[189](#td-2026-08-12-189) (the visible-line cache evicts by insertion order, so a
cache HIT does not protect the entry a caller is reading). **All five closed the
same day**, along with [174](#td-2026-08-10-174), [181](#td-2026-08-10-181),
[182](#td-2026-08-11-182), [183](#td-2026-08-11-183), [184](#td-2026-08-11-184),
[142](#td-2026-08-06-142) and [144](#td-2026-08-06-144); two new ones were filed
from what the gate run turned up ([190](#td-2026-08-12-190),
[191](#td-2026-08-12-191)).

Two things from that pass are worth carrying forward more than the fixes are.
**A parity test between two implementations is blind to anything that does not
change the answer** — the whitespace one could not see either the presence or the
absence of the row-resume optimization, which is why 187 shipped with a counter
(`editor.whitespace_marker_walk_bytes`) rather than with another parity fixture.
And **a perf comparison is only a comparison if both sides are the same lane**:
four net-heap "regressions" that looked like the session's work turned out to be
present at its base commit under the `microide-perf` preset and ABSENT at both
commits under plain `Release` ([191](#td-2026-08-12-191)).

Reviewed 2026-08-11. The 2026-08-11 pass read the suite's two biggest phases
([159](#td-2026-08-06-159)): `multi_project.switch_cycles` (-42 %) and
`git.refresh_dispatch` (-14 % / -12 %). Five fixes, all of them the same
mistake in different clothes — **re-deriving, on an event, something that event
cannot change**: the language-contract default table and the colorscheme picker
on every project switch, a path key per tree entry, a normalization per git
entry. Three entries opened: [182](#td-2026-08-11-182) (an undo entry costs one
string per line it covers), [183](#td-2026-08-11-183) (a git refresh builds each
path four times, and libstdc++'s `path` is two allocations before it holds
anything), [184](#td-2026-08-11-184) (every gate this pass touched is now 12-40 %
loose and this runner may not re-record it).

Reviewed 2026-08-10. The 2026-08-10 second pass read the four phases
[159](#td-2026-08-06-159) named as next, which produced two fixes
(`PieceTree::ExtractLineRange`'s per-call walk stack, and the welcome surface
rebuilt per frame *and* per mouse event), two read/inherent results, and one new
entry: [181](#td-2026-08-10-181), twelve phase allocation gates that read
differently at `--iterations=2` than at 10 — three of them named for an operation
they never run. It also closed [174](#td-2026-08-10-174)'s scan half: every
`x.lexically_normal() == normalized` comparison in `src/` now goes through
`util::SameAsNormalizedPath`.

Reviewed 2026-08-07. **84 open items.**
([162](#td-2026-08-07-162), [163](#td-2026-08-07-163) and
[165](#td-2026-08-07-165) resolved; [164](#td-2026-08-07-164) and
[166](#td-2026-08-07-166) opened by that work; [159](#td-2026-08-06-159) took a
grep-first pass and stays open for its tail; [161](#td-2026-08-07-161) is half
done — its deterministic gates are rerecorded and its timing/resident gates still
need an idle runner.)

**The perf cluster's 2026-08-07 pass**, in the order it was taken:
[163](#td-2026-08-07-163) audited the instrument (answer: `plugin_status_item_update`
was unique — 1 of 115 phases above threshold, and that one legitimately),
[159](#td-2026-08-06-159) took the grep-first sweep plus the audit's own
product-site table as its worklist, and [162](#td-2026-08-07-162) landed the debug
value tree. On the way, reading allocation counts closely surfaced
[165](#td-2026-08-07-165): **every scenario's shell was reading the developer's
real `~/.local/state/microide`**, so any allocation gate on a shell scenario was
partly measuring the operator's machine. That is fixed, and the deterministic half
of the suite is re-recorded against it.

[161](#td-2026-08-07-161) remains blocked on a quiet machine, not on effort — take
it whenever one appears, and it also settles the menu anomaly recorded there. Its
one visible symptom today is `typing_large_file`'s `mean_rss_growth_bytes`, the
suite's only red gate.

The 2026-08-06 interactive sweep is [149](#td-2026-08-06-149)'s own instruction
carried out — "generalise the sweep: the instrument is cheap now, and no
interactive scenario other than the drag and this one has been read through it."
Seven phases read, six fixes, and the three entries the reading opened
(**157**, **158**, **159**).

It read 7 of 70. **159** is the remaining 63, with the worklist, the method, a
definition of done, and one lead already open (toggle-comment costs 89 allocations
per line and only ~8 of them are accounted for).

The largest was not an editor path at all. `ApplyBranchReviewPresentationMarkers`
runs unconditionally from the compare tab's derived-state refresh — the refresh
whose *other* half already carries a comment saying it fires from ~10 event sites
including every mouse move — and it cost **1,418,736 allocations / 28.6 ms** a
pass. It is **721 / 0.103 ms**, and **0 / 0.0002 ms** when nothing moved:

  - 79 % of it was `PathsEqual` calling `lexically_normal()` on **both** sides of
    every comparison, inside scans that run per reviewed-hunk entry per hunk per
    row. Every stored path was already normalised on ingress by the mutators; the
    persistence bridge is now the last ingress to say so, and the comparison is a
    string compare.
  - the rest was asking one hunk at a time: `HunkStatus` scans the entry list per
    hunk and, for a hunk with no entry of its own, falls back to a `FileStatus`
    that walks that list against every model hunk **recomputing each hunk's
    content hash**. `ResolveHunkMarkers` walks each list once. The content hash
    also stopped serialising the hunk into a `std::ostringstream`.
  - and then it stopped running at all on a refresh that moved none of its three
    inputs (row list, review revision, branch target).

The editor half was one shape in two places: **a span captured, then copied
again**. A multi-caret edit's undo entry spans the first to the last affected
caret line, so on `editor_surround_multi_caret` (8 carets over 8,400 lines) the
tracer showed that span materialised **eight times per keystroke**, six
avoidable — `BuildEntryForDocumentChange` copying both vectors instead of moving
the trimmed sub-range out, `PushHistoryEntry` copying the finished entry, and a
per-line `lines[i]` capture that goes through `TextBuffer::LineRef` and so also
inserted every line of the span into the buffer's line cache. The line ops
(`Move Line`, `Toggle Comment`, `Sort`, `Indent`) had the same two on their own
replacement vector.

    editor.surround_multi_caret.insert   67,366 -> 16,979  (-74.8%)
    move_line_down.multi_caret_burst     56,822 -> 28,924  (-49.1%)
    toggle_line_comment.1000_lines      105,060 -> 89,044  (-15.2%)

The two the model *requires* — a before and an after image of the whole span,
including every unchanged line between the carets — are [157](#td-2026-08-06-157).

Two per-frame shell probes fell out of the scroll scenarios, and they moved all
four at once: `HoveredTooltip` laid out both tab strips before the containment
test that discards them, and `IsGitRepoValid` built a whole `GitRepository` to
ask whether `.git` exists.

    editor_scroll_only_no_content_bump.scroll_frame   13,350 -> 7,150   (-46.4%)
    editor_sticky_scroll_scroll.fast_scroll_frame     22,506 -> 16,306  (-27.5%)
    editor_fold_viewport_refresh.scroll_frame         22,152 -> 16,200  (-26.9%)
    editor_render_whitespace_paint.scroll_overlay     19,230 -> 14,270  (-25.8%)

What is left of that probe — one `stat` per painted frame, uncached on purpose —
is [158](#td-2026-08-06-158).

**Not yet done: the rebaseline.** These moves left their allocation gates loose by
between 1.2x and 1,967x — `branch_review.presentation_markers` is gated at
1,418,736 and costs 721 — which is exactly the defect
[147](#td-2026-08-06-147) is about: a gate that loose passes a complete
regression. The rebaseline was deliberately NOT taken in this pass because the
runner was busy (load average 9-25), and a baseline recorded on a loaded box
records the load, not the code. It also has to arm the one new phase, which the
run reports as `NOT GATED` until then:

```
tools/run-checks.sh tests   # confirm green first
# on an IDLE perf-runner-v1, at the DEFAULT iteration count, bare (no xvfb):
./build/microide-perf-make/microide/microide_perf --update-baseline \
    --reference-runner=perf-runner-v1
# then re-gate against what it just wrote — a rebaseline is not evidence of itself:
./build/microide-perf-make/microide/microide_perf --reference-runner=perf-runner-v1
```

Certify with `harness.cpu_calibration_ns` (not load average) and check for
clock-drift warnings on the verdict lines before committing the result.

The 2026-08-06 finder pass closed **153** and **154**, and opened **155** by
tripping over it.

153 said a phase that is measured and compared to nothing is not a gate. Every
`Measure` phase now carries its own allocation gate, written into the baseline
and enforced: **114 phase gates across 82 baselines**, certified by a whole-suite
rebaseline re-gated against itself (**100 PASS, 0 FAIL, zero ungated phases**).
`search_first_result` gates its search at 145 allocations instead of gating
20,192 allocations of project-open and calling that "the authoritative signal". A
baseline phase the run stops measuring now FAILS — a deleted `Measure` call must
not remove a gate in silence.

154 then used it. The file finder's cache rebuild was **41,622 allocations** on a
10,000-file project; it is **1,621**. The index deep-copy is gone (and so is the
deep-copying accessor, which had exactly one caller), and `CachedFileEntry` owns
no strings at all — path bytes and their fold live in two blobs and the entry is
five 32-bit offsets. The scan the finder runs per keystroke now walks two
contiguous buffers instead of 20,000 heap nodes.

The rebaseline that armed the phase gates is what found **155**: four git
scenarios had drifted up 0.1 %, which turned out to be a scenario appending to a
fixture **in the repository** — 1,361 times, growing a 3-line worktree diff to
2,725 lines. Those scenarios had been measuring how often the suite had ever been
run on that checkout.

And the new scenario 155 brought with it — `file_finder_type_query`, the finder's
interactive path, which had no coverage at all — immediately found **156**: the
ranking is allocation-free, and then every keystroke deep-copies 512 result rows
to render twenty. Two thirds of that is gone (the per-row
`std::filesystem::path` served one function that runs once, on Enter); the
remaining third is a lifetime problem stated in the entry.

The earlier 2026-08-06 measurement-integrity pass closed three — **150** (the resident gate
is a coin flip), **151** (the biggest interactive scenarios have no measured
phase), and **146** (ccache + LTO ICEs, so a history walk skips commits) — and the
theme is that two of them were instruments, not code, and both paid off
immediately.

150 asked whether the resident swing was process state or a bimodal fixture. It is
process state, an 8x range driven by which scenarios fragmented the heap first —
while `bytes_allocated - bytes_freed` across the measured window reproduced **to
the byte for 52 of 52 scenarios across three completely different suite prefixes**.
That number was already being recorded per iteration and thrown away. It is now
`p50_net_heap_bytes`, the deterministic retention gate, and the resident mean was
demoted to the job it can still do (growth that never went through `operator new`).

151 wrapped eight scenarios' existing timed loops in `ScenarioContext::Measure` so
the phase-scoped allocation tracer could be aimed at them at all. The first trace
found that the inline-blame overlay re-normalises the same two paths three to four
times **per painted frame** — 11 of the top 12 sites in one phase. A memo on a
purely-lexical, therefore un-staleable, function took **~30 % of the allocations off
every editor scroll scenario**.

The memo is not confined to the scenario that found it: **41 of 99 gated scenarios
moved**, `linter_on_save` by 52 % and `editor_scroll_only_no_content_bump` by
41 %. The suite was rebaselined whole rather than patched — leaving 41 gates 30 %
loose is the defect [147](#td-2026-08-06-147) exists about — on a confirmed-idle
perf-runner-v1 with **zero clock-drift warnings across all 99 scenarios**, then
re-gated against what it wrote: **99 PASS, 0 FAIL, every gated metric below 75 %
of its envelope** (the previous set had one sitting at 94 %).

It also took **142** most of the way: the measurement that entry said had to come
before any cap now exists, and it says a warmed 50k-line tab holds **4.13 MiB**,
46 % of it in the per-line highlighter state — the component the entry did not
size, rather than the width table it expected to dominate.

It opened three. Two are about what the suite still cannot see: **152** (every
metric is a property of the whole run because all 93 scenarios share one process)
and **153** (`search_first_result`'s tight allocation gate is 99.3 % project-open,
and its baseline comment claims the opposite). The third, **154**, is the next
piece of speed work and came from the same instrument aimed at the next declared
phase: the file finder's cache rebuild costs **4 allocations per indexed file**,
and the largest of the four is a deep copy of the whole index — taken under a
lock, for the only caller in the tree, which does not need the two fields that
force it, while a zero-copy snapshot sits one method below it.

The 2026-08-06 input-path pass closed **145** (a mouse-drag rebuilding the pane
layout three times per motion event). The drag's measured phase now allocates
**nothing** — 960 to 0 — because option (a) removed four of the six per-move
allocations and the two underneath turned out to be a different bug entirely: a
copy where a move was meant in `ConsumePendingRenderInvalidation`, plus a
`std::vector` that could not keep its buffer across events. Attributing those two
needed the allocation tracer scoped to the measured phase, which it could not do;
`MICROIDE_PERF_ALLOC_TRACE_PHASE` now does, and named both in one run.

It opened three. Two are about the gate rather than the code: **147** (the fix left
four allocation baselines 4-5x loose, so they would pass a complete regression) and
**148** (`typing_large_file`'s RSS gate rides its envelope, and a non-default
`--iterations` silently flips the verdict).

The third, **149**, is the next pass's work and came from pointing 145's own
instrument at the next interactive scenario: a menu-bar hover costs **50
allocations per motion event** — 25x what the drag cost *before* 145 — and every
site in the printed table bottoms out in one function, `ComputeVisibleMenuBarItems`,
reached three independent ways per event. It is 145's shape one subsystem over,
down to a `std::function<std::vector<...>(const SDL_FRect&)>` coordinator callback.
**Generalise the sweep**: the instrument is cheap now, and no interactive scenario
other than the drag and this one has been read through it.

The 2026-08-06 perf-integrity pass closed three: **139** (five allocation
gates drifted up, unexplained), **141** (nothing reruns the gate), and **143**
(`MaxVisualColumns` stamping a revision onto a table it did not verify). It opened
three: **144** (`FoldingModel::Block`'s four per-block vectors, the residue of
139's real half), **145** (a mouse-drag rebuilding the pane layout three times per
motion event, found by the tracer 141 added), and **146** (ccache + LTO ICEs, so an
A/B over history skips commits — the toolchain problem that made 139 expensive to
attribute).

**139 is the second entry in three days whose headline was wrong**, after 138.
Both were wrong the same way: a number attributed to the work the scenario is named
after, when it came from the scenario's setup. `p50_allocations` covers the whole
iteration; `phase_metrics` in `--report-json` is what covers the measured phase,
and it read 960 allocations at every commit across 139's 90-commit window. Four of
139's five drifts turned out never to have happened at all — they do not reproduce
in a controlled A/B of the two endpoints, so they were a property of the full run,
not of the code.

The 2026-08-06 pass closed the three perf-gate-integrity entries filed on
2026-08-05: TD-2026-08-05-135 (four gated baselines 40-80% looser than the code
they gate), TD-2026-08-05-136 (a resident-growth gate decided by allocator arena
state), and TD-2026-08-05-137 (a CPU gate decided by the governor). All three were
the same failure in different metrics — a green run that was green for a reason
having nothing to do with the code — and all three are now measured rather than
worked around.

It opened four: **138** (the whole-document width table is built twice per file
open), **139** (five allocation gates drifted up, unexplained, and passed), **140**
(the wall gate cannot catch a regression under 2x, and now has the data to), and
**141** (nothing reruns the gate, so drift is only ever found by accident). The
first came out of reading counters during the sweep; the other three were live
findings sitting in the body of the entry that same sweep *closed*, which is this
file's own failure mode — work nobody scanning the open items would ever see. A
finding discovered by a fix does not belong inside the fix's entry.

**138 then closed the same day, and its headline was wrong.** The table is not built
twice per file *open* — a fresh open builds it once. It was built twice per
*re*-open, because re-opening a file that was already open re-read it from disk and
threw every derived cache away first. The entry's own instruction ("confirm the
trigger, do not go straight to a fix") is what caught it, and its named hypothesis
(indent detection changing `tab_size` after the first build) never fired once. The
fix opened two: **142** (a tab's derived caches have no measured ceiling — the
reload was the only thing bounding them) and **143** (`MaxVisualColumns` stamps a
fresh content revision onto a table it did not verify, an invariant nothing
enforces).

The 2026-08-05 burndown closed seven: TD-2026-07-30-001 (search-panel Alt chords),
TD-2026-07-26-005 (replace-all measured through counters only it writes),
TD-2026-07-26-003 (post-commit refresh failure reaches the user), and four from the
2026-07-10 deferred set (LSP diagnostics across a close→reopen, three terminal spec
deviations, the `LineEndingHeavy` mislabel, and the single-line view-metrics
coverage gap).

This file is the queue for tech debt that is **open, actionable, and still present
in the tree**. Closed debt does not live here.

It had stopped being that. 555 of its 637 entries were resolved or won't-do, in
sections whose titles said so ("Fixed in the 2026-07-13 actionable sweep") and one
whose title did not — "Still open (deferred, lower value / larger / latent)" held
215 entries, every one of them already RESOLVED or WON'T-DO. A queue that is 87%
closed is not a queue; it is a changelog that nobody can find the open work in.

The closed record moved to
`guidelines/tech-debt/archive/2026-08-04-resolved-debt-ledger.md`. Nothing was
deleted — the split was verified item-by-item, 82 + 555 = 637, none lost, none
duplicated.

Verified won't-do decisions stay here on purpose, so they are not re-filed.

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

### TD-2026-08-12-192 — a fuzz target had not compiled since 2026-08-11 and nothing noticed. [RESOLVED 2026-08-12 — and the reason it went unnoticed is the entry.]

`GitPorcelainV2ParserFuzz.cpp` called `.native()` on
`GitRepositoryEntry::path.relative_path`, which stopped being a
`std::filesystem::path` and became generic '/'-separated TEXT when the git
pipeline stopped materializing a path per entry
([183](#td-2026-08-11-183)). The target has not compiled since.

**Nothing caught it because no default flow builds the fuzz targets.** They are
behind `-DMICROIDE_FUZZ=ON` with a clang toolchain, so `tests`, all three
sanitizer lanes, `clang-build` and the perf lanes are all green with a fuzz
target that does not exist. That is the same shape as the bench binaries' curated
source lists, except the bench targets ARE built by the sanitizer lanes and the
fuzz ones are built by nothing.

Fixed, and all 11 targets build; `GitPorcelainV2ParserFuzz` and
`PieceTreeEquivalenceFuzz` were run against their committed corpora (2,205 and
21,761 runs, no findings). **The habit to keep: run `tools/run-checks.sh fuzz
--list` after any change to a type a fuzz target touches** — it configures the
clang+fuzz tree and builds every target without running them, which is the cheap
proof they still compile.

### TD-2026-08-12-191 — four `p50_net_heap_bytes` gates have been red since before 2026-08-12, and the metric is BUILD-CONFIGURATION dependent. OPEN (the configuration half shipped; the retention itself is unexplained).

Found by running the full gate (which is itself supposed to be routine —
[141](#td-2026-08-06-141)) and then A/B-ing the failures against the session's
base commit, because they looked like a regression from that session's work.
They are not. Two separate facts came out of it.

**1. Four retention gates were already red.** On the canonical `microide-perf`
lane (RelWithDebInfo + LTO), at commit `74471554` — before any of the 2026-08-12
work:

| scenario | baseline | at 74471554 | at HEAD |
| --- | ---: | ---: | ---: |
| `project_traversal_filter_scan` | 8,294 | 75,872 | 75,872 |
| `settings_change_many_tabs` | 11,782 | 97,864 | 107,526 |
| `multi_tab_cycle` | 60,961 | 80,342 | 92,044 |
| `switch_and_idle` | 30,010 | 94,638 | 101,750 |

`project_traversal_filter_scan` is byte-identical across the two commits, which
is what rules the session's changes out as the cause of the class. The ~10 KB
each of the other three moved is the visible-line LRU's per-entry growth
([189](#td-2026-08-12-189) added a 32-byte self key plus two links per cached
row, bounded by the 256-entry limit), which is a deliberate and bounded cost of
making a handed-out reference safe.

So a retention regression of 3-10x landed at some point and no gate run caught
it — the same "nothing reruns the gate" shape as [141](#td-2026-08-06-141),
except the gate DID exist and was simply never run.

**2. `p50_net_heap_bytes` is not portable across build configurations.** The
same four scenarios, same commits, built `Release` WITHOUT LTO instead of the
`microide-perf` preset: all four PASS. The metric reproduces to the byte within
one configuration (every run above repeated exactly) and moves by 60-90 KB
between two. [174](#td-2026-08-10-174)'s "deterministic retention gate" claim is
therefore true only with the build configuration held fixed, and nothing in a
baseline records which configuration produced it — the same class of gap as
[167](#td-2026-08-07-167) (the measurement REGIME is not recorded) and the
video/CPU lane findings.

Two pieces of work were named. **The second shipped 2026-08-12**: a baseline
records `build_config` (CMake bakes `MICROIDE_PERF_BUILD_CONFIG` — build type
plus the IPO flag — into the perf binary), and a mismatch unenforces exactly the
metrics that move with it (wall, cpu, `mean_rss_growth_bytes`,
`p50_net_heap_bytes`) with both configuration names on the verdict line.
Allocation counts stay enforced, because they came out byte-identical across the
two configurations measured. Verified live: the `Release+lto` binary run against
a `RelWithDebInfo+lto` baseline PASSes on allocations and reports the other eight
metrics as not comparable. Baselines written before the field compare exactly as
they did.

**One cause found and fixed 2026-08-12: the RSS probe allocated inside the
measured window.** `ProcessResidentBytes()` read `/proc/self/statm` with a
`std::ifstream`, and the `rss_before` reading is taken AFTER the allocation
snapshot opens — so a filebuf's 8 KB stream buffer was one allocation and 8,192
bytes charged to **every scenario's window, on every iteration**. It surfaced as
two 10-allocation trace sites in a 10-iteration run of a scenario whose own phase
allocates nothing: the instrument inside its own measurement, the shape
[163](#td-2026-08-07-163) audits for.

Now an `open`/`read` into a stack buffer, and the numbers moved by exactly the
predicted amount: `project_traversal_filter_scan` 14,078 → **14,077**
allocations and 75,872 → **67,680** net bytes. **Every committed
`p50_allocations` baseline is therefore one too high and every
`p50_net_heap_bytes` 8,192 too high**, which is a suite-wide deterministic
re-record.

**Second cause found and fixed 2026-08-12: `project_traversal_filter_scan` was
measuring its own fixture.** After the probe fix it still retained 67,680 bytes
per iteration. Proved what that was by doubling the fixture: allocations went
14,077 → 28,059 and net heap 67,680 → 135,056, i.e. **both scale 1:1 with the
scenario's own entry count** — in a scenario whose measured phase allocates
exactly zero. Building 2,048 `std::filesystem::path`s per iteration WAS the
measurement.

The entry vector is scenario INPUT, so it is now built once for the process
(function-local static, with `warmup_iterations = 1` declaring the iteration that
pays for it). Steady-state readings: **1,916 allocations (was 14,078, -86 %) and
6,240 net bytes (was 75,872)**. `measurement_revision` bumped, rebaselined,
re-gated green. Same rule as [163](#td-2026-08-07-163): build scenario inputs
outside the measured window — this is the second scenario to have broken it, and
the first where the giveaway was a retention gate rather than a duration.

**Still open: the other three** — `settings_change_many_tabs`, `multi_tab_cycle`
and `switch_and_idle`. They are shell scenarios rather than unit-shaped ones, so
the fixture-scaling test above does not transfer directly; the method does. Note
the canary scenario nets exactly **0** bytes over 2,000 balanced allocations, so
the accounting itself is sound and anything these three report is real retention
on the scenario thread.

A mechanism was proposed on 2026-08-12 and is WRONG; it is recorded here so it is
not proposed again. The theory was "`p50_net_heap_bytes` is process-global while
the phase allocation counters are per-thread, so a scenario with background work
measures whichever interleaving it got". The evidence for it was that tracing
`project.traversal_filter_scan` returns no allocations at all while the scenario
reports 14,078. Both halves fall apart on inspection:

- `Allocations::Snapshot()` reads `t_allocations` / `t_bytes_allocated` — all
  four counters are `thread_local`. Net heap is per-thread exactly like the
  allocation counts.
- That phase's own committed `p50_allocations` is **0**. The tracer found nothing
  because there is nothing; the 14,078 is the scenario TOTAL, which spans setup.

What remains is the actual open question, now sharper: **the same commit, same
box, same measured window, byte-identical allocation COUNTS, and 67 KB more
retained under `RelWithDebInfo+lto` than under `Release`.** Same count, different
net bytes means either the allocation SIZES differ or something is freed inside
the window in one build and outside it in the other. Inlining plausibly moves a
temporary's lifetime across the window boundary; that is a hypothesis and not yet
evidence.

Do NOT rebaseline these four while this is open — a rebaseline would enshrine one
build's answer. `switch_and_idle`'s own top allocation sites (the session-record
encoder running on the switch path) are a separate real finding, see
[159](#td-2026-08-06-159)'s 2026-08-12 entry.

### TD-2026-08-12-190 — three merge scenarios gate on a re-show they name an open, and the fix needs an idle runner. [RESOLVED 2026-08-12.]

**Fixed 2026-08-12.** `ScenarioContext::CloseActiveTab()` (over a new
`TestAccess::CloseTab`, the unconditional close that skips the dirty prompt)
runs BEFORE the measured window, so the `merge ...` command opens a tab on every
iteration instead of re-showing the one the previous iteration left. Each of the
three scenarios bumped `measurement_revision` to 2, which is what made the run
refuse to compare against the old numbers rather than silently reporting a
1000x "regression".

What the gates were actually worth, before and after:

| phase | was (a re-show) | now (an open) |
| --- | ---: | ---: |
| `merge_large.open_to_first_paint` | 185 | **53,906** |
| `merge_interleaved.open_to_first_paint` | 179 | **11,291** |
| `merge.open_many_conflicts` | 180 | **11,363** |

The rebaseline was the part this entry called blocked. It is not, with
[186](#td-2026-08-12-186)'s mechanism: recorded here with the timing half marked
advisory, so the allocation and retention gates are honest immediately and the
wall/cpu half says it is waiting for a reference run.

One trap worth recording: the scenario that owns
`merge_interleaved.open_to_first_paint` is `merge_scroll_interleaved_hunks`, not
`merge_model_build_interleaved`. The first attempt bumped the revision on — and
rebaselined — the wrong one, which would have downgraded a reference-recorded
timing half to advisory for a scenario that had not changed. The phase-name-to-
scenario mapping is not the obvious one; read it off the `Measure()` call, not
off the name.

Split out of [181](#td-2026-08-10-181), whose item 1 shipped and whose item 2 did
not. `merge_large.open_to_first_paint`, `merge_interleaved.open_to_first_paint`
and `merge.open_many_conflicts` each re-run their `merge ...` command per
iteration against a driver that reuses the already-open merge tab, so iteration 0
pays the real open (27,293 allocations) and iterations 1..9 pay a re-show; the
p50 lands on the re-show and the baseline records 185.

The code half is small: `ScenarioContext` needs a close-tab verb and
`WorkspaceShell::TestAccess` needs to expose one. The blocker is the other half —
closing the tab between iterations changes what all three scenarios measure, so
they need `measurement_revision` bumped and a full rerecord, and the wall/cpu half
of a rerecord taken on a loaded box would bake that box in. With
[186](#td-2026-08-12-186)'s advisory-timing mechanism the allocation half could be
rerecorded here and the timing half left explicitly unenforced, which is probably
the right move — it is listed separately because it is a scenario-semantics
change and not a mechanical one.

### TD-2026-08-12-189 — the visible-line cache is a FIFO, so a HIT does not protect the entry from the next miss's eviction. [RESOLVED 2026-08-12.]

**Fixed 2026-08-12.** The recency order is an intrusive doubly-linked list
threaded through the map's own nodes (`VisibleLineCacheEntry::lru_prev/lru_next`
plus a self key so eviction can `extract()` the head without a reverse lookup),
spliced to the tail on every hit. Intrusive rather than `std::list<Key>` because
a list would pay 256 node allocations on first fill of every tab's cache, and it
lets the recycle path splice head→tail without allocating at all; the side deque
of keys is gone. An `unordered_map` node's address survives both rehash and
`extract()`/`insert()` of the same node, which is what makes the pointers valid
across the recycle.

`TextLayout/HitProtectsEntryFromEviction` pins the property that separates an LRU
from a FIFO — an entry re-read every round survives 512 misses and its reference
keeps pointing at its own content — and was probed by disabling the touch, where
it fails on the first eviction.

#### Original entry


`TextLayoutCache::VisibleLineLayoutRefCached` hands out a reference into a
256-entry map and evicts by insertion order (`visible_line_cache_order_` is a
`deque` that a hit never touches). The header's safety argument — "a frame's
working set is far below the limit, so a frame never evicts what it is still
reading" — covers entries the frame BUILDS, not entries it merely HITS: an entry
inserted many frames ago sits at the front of the FIFO no matter how recently it
was read, so a miss taken while a caller holds a reference to it recycles that
node underneath the caller. The result is a row painted with another row's
glyphs, not a crash: `extract`/`insert` keeps the node alive and rewrites it in
place.

Reachability is narrow and got narrower on 2026-08-12: the render loop now takes
exactly ONE cache reference per row (the end-of-line decoration path used to take
a second one, for a line width it can read off the wrapped-row table instead).
What remains is any future caller that holds a row layout across another layout
query. `visible_line_evictions` already counts the evictions this argument turns
on, which is what makes the claim testable.

The fix is to make it a real LRU: an intrusive `std::list` of keys with the
iterator stored beside the layout, spliced to the back on a hit. Then the
eviction victim is always older than anything the current frame has touched, and
the invariant holds for hits as well as builds.

### TD-2026-08-12-188 — Home/End under soft wrap move by LOGICAL line, where VS Code moves by visual row. [RESOLVED 2026-08-12 — both halves.]

**Shipped 2026-08-12**, taking VS Code's semantics for both halves the entry
named:

- **View-line motion.** `ViewLineBoundsForCaret` resolves the caret's wrapped row
  and converts its `[visual_start, visual_end)` to text columns; with wrap off,
  or on a trivial layout, it hands back the whole logical line, so the non-wrap
  behaviour is unchanged by construction. An End that lands exactly on a wrap
  point takes `WrapRowAffinity::kPreviousRow`, so it renders at that row's
  trailing edge instead of the next row's start. A caret on a fold-hidden line
  resolves to the OPENER's row, which belongs to a different logical line — that
  case falls back to the whole line rather than moving the caret onto someone
  else's row.
- **The first-non-whitespace toggle.** Home goes to the first non-whitespace
  character of the view line, and to the row's true start when the caret is
  already there.

Both applied to secondary carets too. Worth recording: **the whole suite stayed
green after the semantics changed**, so nothing had pinned the old Home
behaviour — the two new tests are the first coverage this verb has had.

#### Original entry


With word wrap on, `MoveCursorLineStart`/`MoveCursorLineEnd` jump to column 0 /
the end of the whole logical line, so pressing Home on the fourth wrapped row of
a paragraph scrolls back up three rows. VS Code binds Home/End to `cursorHome`
/ `cursorEnd`, which move within the **view line** (the wrapped row), with
`cursorLineStart`/`cursorLineEnd` as the separate logical-line verbs.

Left as an entry rather than a commit because it is a keybinding-semantics
change, not a defect: this editor also diverges from VS Code on Home in a second
way (no first-non-whitespace toggle), so "match VS Code" here is a two-part
product decision. The pieces to build it on already exist:
`TextViewport::WrappedVisualRowLayout` gives the row's `[visual_start,
visual_end)`, and `WrapRowAffinity::kPreviousRow` is exactly what an End that
lands on a wrap point needs so the caret renders at the row's trailing edge
instead of the next row's start.

### TD-2026-08-12-187 — the renderer's fallback whitespace walk still restarts at byte 0 of the line for every visible row. [RESOLVED 2026-08-12.]

**Fixed 2026-08-12.** `EditorViewRenderer`'s fallback resumes at the row's own
start under the same plain-ASCII-prefix condition the view-model builder uses, so
the two paths now have the same asymptotic shape and not merely the same answer.

Two things the fix surfaced, both worth more than the fix itself:

- **The parity test could not have caught this, and could not catch a WRONG
  resume either.** Walking from byte 0 produces identical markers, just slower —
  so parity is blind to the whole optimization. It needed an instrument:
  `editor.whitespace_marker_walk_bytes` counts the bytes each row's walk visits,
  in both producers, once per row. `TextRenderer editor view whitespace walk
  resumes at the row` asserts the resuming fixture costs about ONE pass over the
  line while an un-resumable one (a tab in column 0) costs many, and runs against
  both producers. Probed by deleting each resume in turn.
- **The parity test's soft-wrap case exposed that the RENDERER owns the viewport
  geometry**: `Render` calls `SetViewportSize(metrics.visible_rows,
  metrics.visible_columns)` from the rect, so a size set by a test is overwritten
  by the first render, and a view model built before that render describes a
  different wrap width than the pixels it is compared against. The helper now
  renders once to settle geometry, then builds — which is the order the shell
  uses — and carries a control that the fixture really does wrap.

#### Original entry


`RenderViewModelBuilder`'s whitespace-run builder — the path production paints
from — now resumes at the row's own start when the bytes before it are plain
ASCII, so a soft-wrapped long line is no longer re-walked once per visible row.
`EditorViewRenderer`'s text-iteration fallback (`use_vm_whitespace == false`) was
left as it was: it still walks from byte 0 and stops at `row_end_visual`, which
is the same quadratic shape over the rows of one wrapped line.

It is reached only when no view model is supplied, which in the tree means the
renderer's own tests and the parity test that pins the two paths together — so
the cost is not paid in the app. Worth closing anyway, because the parity test is
what keeps the two implementations honest, and a fallback that is
asymptotically different from the real path is a fallback whose parity only holds
on small fixtures.

### TD-2026-08-12-186 — the two soft-wrap perf scenarios have no baseline, so only their hand-written invariants gate. [RESOLVED 2026-08-12 — the deterministic half is armed; the timing half is explicitly advisory.]

**Fixed 2026-08-12 by making the harness able to say which half it recorded.**
The entry's instruction was "record them on a quiet reference run", which meant
the scenarios gated on NOTHING until somebody found an idle machine — a state
that had already lasted the whole life of the pair.

A `BaselineRecord` now carries `timing_is_advisory`. When set, the deterministic
metrics (allocations, phase allocations, net-heap retention) gate normally and
the machine-sensitive ones (wall, cpu, `mean_rss_growth_bytes`) are measured,
printed, and explicitly NOT enforced, with the reason on the verdict line.
`--update-baseline=deterministic` mints such a record when no baseline exists
instead of skipping, and `MergeDeterministicMetrics` starts from the committed
record, so a reference-recorded timing half is never downgraded by a later local
allocation rebaseline.

Both scenarios are `baseline_gated = true` now, with baselines minted here. What
is still owed is one `--update-baseline --reference-runner=perf-runner-v1` on an
idle box to arm the timing half — but the failure mode changed from "no gate" to
"three quarters of a gate, and it says which quarter is missing".

Same mechanism closes the "documented way to say deterministic metrics only" that
[184](#td-2026-08-11-184) asks for.

#### Original entry


`editor_soft_wrap_long_line_scroll` and `editor_soft_wrap_long_line_typing`
(added 2026-08-12 with the wrap fixes — soft wrap had no perf coverage at all
before them) are registered `baseline_gated = false`. Their two hard invariants
do gate on any box (a pure scroll must not re-measure a row's logical line, and
must not rebuild the O(document) wrapped-row table), and both are protected from
passing vacuously. What is missing is the wall/allocation envelope: they were
written on a box under load average 14, where a recorded baseline would bake in
the noise.

Record them with `--update-baseline --scenarios=editor_soft_wrap_long_line_scroll,editor_soft_wrap_long_line_typing`
on a quiet reference run, then flip `baseline_gated` to true. Same runner
constraint as [184](#td-2026-08-11-184) and [161](#td-2026-08-07-161).

### TD-2026-08-12-185 — a caret on a fold-hidden line resolves to the fold opener's FIRST wrapped row. [RESOLVED 2026-08-12.]

**Fixed 2026-08-12.** `CursorVisualRowForCaret` answers the opener's LAST row for
a hidden line under soft wrap (`WrappedRowRangeForLine(opener_line).second`),
which is the fold's trailing edge — one Down leaves it, matching what the same
caret already does with wrap off. The fix is scoped to the caret query rather
than to the shared row-offset table: `VisualRowForLine` feeds scroll anchoring,
inset placement and the blame overlay, where "the row the enclosing visible line
starts at" is the right answer and changing it would move an unrelated set of
behaviours.

`TextViewport/SoftWrapCaretOnFoldHiddenLineEscapesInOneStep` pins it, with a
control that the opener really does span several rows (otherwise first and last
row are the same and the test proves nothing), and was probed by reverting the
fix.

#### Original entry


`TextLayoutCache`'s row-offset table stores, for every hidden line, the row index
where the last VISIBLE line started. With soft wrap on, a collapsed opener that
wraps into rows [R, R+3] therefore answers R for every line it hides, so a caret
parked on a hidden line (a search hit, a restored session, a jump-to-definition
into a folded body) resolves to the opener's first row and needs four Downs to
escape a fold it should leave in one.

The opener's own rows are handled — `WrappedRowRangeForLine` scans by
`line_index` precisely so a wrapped opener does not collapse to a single row
(that fix is covered by `SoftWrapCollapsedFoldOpenerVerticalMotionEscapes`) —
this is the hidden-line half of the same table. The cheap fix is to store the
opener's LAST row for hidden lines, which makes vertical motion off a hidden line
leave the fold immediately; the thorough one is to refuse to place a caret on a
hidden line at all (VS Code reveals the fold instead).

### TD-2026-08-11-184 — every allocation gate this pass touched now passes with 12-40 % of slack, and nothing can re-record them here. [RESOLVED 2026-08-12.]

**The entry's premise was wrong in one detail and right in the conclusion.**
`microide_perf` does NOT refuse to write baselines on this box — it prints
`advisory run (runner_class=local-advisory)` and writes them anyway. What was
missing was a way to write only the half a non-reference runner is entitled to
measure, which [186](#td-2026-08-12-186) built (`timing_is_advisory`, plus
`--update-baseline=deterministic` minting rather than skipping).

**Re-recorded 2026-08-12** on the canonical `microide-perf` lane, deterministic
half only, timing half carried forward from the committed record:

| gate | was | now |
| --- | ---: | ---: |
| `editor_sort_lines_large` p50_allocations | 20,283 | 305 |
| `editor_toggle_comment_large_selection` | 32,561 | 889 |
| `editor_moby_dick_workout` | 138,600 | 21,682 |
| `git_sidebar_refresh_many_untracked` | 52,357 | 17,739 |
| `git_sidebar_refresh_large_repo` | 23,920 | 9,802 |
| `multi_project_switch` | 17,416 | 9,938 |
| `editor_shaping_multi_caret` | 7,815 | 3,553 |
| `cold_startup_large_project` | 248 | 168 |

Each was checked to be passing its NON-timing gates before being rewritten, and
the gate was re-run against what was written — a rebaseline that is not re-run is
not evidence. The four scenarios whose `p50_net_heap_bytes` is red were
deliberately left alone: rebaselining those would enshrine the regression
[191](#td-2026-08-12-191) exists to find.

#### Original entry


Five fixes landed on 2026-08-11 ([159](#td-2026-08-06-159)) and none of their
baselines moved, because `microide_perf` on this box prints `advisory run
(runner_class=local-advisory)` and refuses to write baselines. The gates are now
loose by the size of the win:

| gate | baseline | measured |
| --- | ---: | ---: |
| `multi_project.switch_cycles` p50_allocations | 17,390 | ~11,254 |
| `git_sidebar_refresh_many_untracked` p50_allocations | 52,357 | 45,051 |
| `git_sidebar_refresh_large_repo` p50_allocations | 23,920 | 21,111 |
| `editor_moby_dick_workout` p50_allocations | 138,599 | 58,552 |
| `cold_startup_large_project` p50_allocations | 248 | 168 |

A 40 %-loose allocation gate is not a gate: it will accept re-adding the whole
`lexically_normal()`-per-entry cost it was tightened past. The deterministic half
of the suite can be re-recorded on any machine (allocation counts are
byte-identical run to run — that is the premise of the whole gating policy), so
this needs `--update-baseline=deterministic` on an authoritative runner, or the
runner-class check needs a documented way to say "deterministic metrics only,
this machine is fine for those". The last two rows predate this pass and show the
drift is not new. Same family as [141](#td-2026-08-06-141) (nothing reruns the
gate) and [161](#td-2026-08-07-161) (the timing half needs a quiet machine — this
half does not).

### TD-2026-08-11-183 — a git refresh materializes each changed file's path as a `std::filesystem::path` four times, and libstdc++'s `path` is not one allocation. [RESOLVED 2026-08-12 — code and rebaseline both.]

The rebaseline this entry was waiting on ran on 2026-08-12 (see
[184](#td-2026-08-11-184)): `git_sidebar_refresh_many_untracked` 52,357 -> 17,739
and `git_sidebar_refresh_large_repo` 23,920 -> 9,802 `p50_allocations`, taken on
the canonical lane with the deterministic-only mode so the wall/cpu half was not
touched.

The `ScenarioContext::Measure` scaffolding share this entry noted at the end
(10 % of `git.refresh_dispatch`'s allocations) is folded into
[159](#td-2026-08-06-159)'s standing tracer sweep.

After the ingress fix ([174](#td-2026-08-10-174)), `git.refresh_dispatch` at 3,000
untracked files is 45,051 allocations, and the remainder is one shape rather than
a list of sites:

| stage | allocations | what it holds |
| --- | ---: | --- |
| `GitPorcelainV2Parser::Parse` → `MakeEntry` | ~12,000 | `GitRepositoryPathIdentity` (a `path` + its generic text) |
| `GitRepositoryService::BuildSidebarSnapshot` | 6,000 | the snapshot entry's own relative `path` |
| `SidebarCoordinator::RefreshGit` | 21,767 | `root / relative` per entry, into `GitSidebarEntry::path` |
| `CachedGitSidebarPresentation` + `BuildGitSidebarViewModel` | 27,052 | grouping keys and row labels derived from that path again |

libstdc++'s `std::filesystem::path` is `_M_pathname` **plus** `_M_cmpts`, a
component list built eagerly by the constructor — so even a short relative path
is two allocations before any component needs its own storage, and `root /
relative` re-splits the whole joined string. Nothing downstream of the parser
needs a `path`: the presentation layer already works on generic '/'-separated
text (it says so in its own header comment), the tree-status map is keyed by that
text, and only the eventual "open this file" action wants a real path.

The fix is to carry the relative generic **text** through the pipeline and build
an absolute `path` at the point of use. It is a wide change (`GitRepositoryEntry`,
`GitSidebarState::RefreshSnapshot`, `GitSidebarEntry`, the view model, and every
consumer of `.path`), which is why it is an entry and not a commit. Worth it: git
refresh is the single largest gated phase in the suite, and it runs on every
external file change.

**Shipped 2026-08-12.** All four stages now carry `std::string` generic text:
`GitRepositoryPathIdentity` holds the text plus an `escaped_label` populated only
for a non-UTF-8 path (the valid case no longer stores a second copy of its own
label), `RefreshGit` joins root and relative by string concatenation into one
reserved buffer instead of `root / relative`, and the presentation layer's
grouping keys are `string_view`s into the row's own `relative_path` rather than
normalized copies. `GenericPathView` lost its scratch parameter — the stored text
*is* the key — and `DisplayLabelView` now names the escape-aware read.

The invariant this rests on is enforced at both ingresses, not assumed:
`MakeGitRepositoryPathIdentity` normalizes behind `PathTextNeedsNormalizing`, and
`CollectGitBranchOutgoingFiles` calls `lexically_normal()` — so the presentation
layer can trust the text it is handed. `GitSidebarCommandCenter/
TreeGroupingMatchesPathAlgebra` moved its fixture's normalization to where the
ingress does it, which is what caught that the contract had to be stated.

**Left open: the allocation rebaseline.** The gate is one-sided (`actual <=
expected + delta`), so the improvement passes against the old numbers and
`git_sidebar_refresh_many_untracked` keeps a `p50_allocations` of 52,001 it no
longer spends — exactly the "allocation drift down" the sweep exists to close.
Re-record on an idle `perf-runner-v1`; this pass had no idle machine, and a
rebaseline taken on a busy one would enshrine noise in the wall/cpu fields.

Also visible in that trace and worth its own look: `ScenarioContext::Measure` is
**10 %** of `git.refresh_dispatch`'s allocations. [163](#td-2026-08-07-163)
audited scaffolding share once and found one offender; this is a second, inside
the suite's biggest gate.

### TD-2026-08-11-182 — an undo entry stores one owned `std::string` per line it covers, which is the floor a 1,000-line edit cannot get under. [RESOLVED 2026-08-12.]

**Shipped 2026-08-12**, and the win is an order of magnitude larger than the
entry's "2n → 4" estimate, because going blob-native removed the *producers'*
per-line strings too:

| scenario | p50_allocations before | after |
| --- | ---: | ---: |
| `editor_sort_lines_large` | 20,283 | **305** |
| `editor_toggle_comment_large_selection` | 32,561 | **889** |
| `editor_shaping_multi_caret` | 7,815 | **3,553** |

`editor::LineBlob` is a run of lines as one byte buffer plus a line-start table.
The pipeline is blob-native end to end rather than converting at a boundary — a
conversion would have paid exactly the allocations this removes:

- `PieceTree`'s line walk is templated on its sink, so one pruned treap walk
  serves the vector and the blob; `ReplaceLineRange` likewise has one body for
  both, so the join, the byte ceiling and the line-count bookkeeping cannot
  drift apart.
- `push_joined()` composes a line from views with no temporary `std::string`,
  which is what took the per-line owned string out of toggle-comment, surround
  and the range-replace composer.
- Sort Lines sorts a PERMUTATION of line indices and appends in that order:
  three allocations for a region, against one per line for the slice plus the
  sort's moves.
- The group-merge splices (`prepend`/`append`/`replace_range`) are one pass over
  the bytes into a buffer reserved to its final size.

The public `ReplaceLines(vector)` stays for callers that already own a vector.
The allocation baselines for the three scenarios above still hold the old
numbers; they are part of the deterministic rebaseline
[184](#td-2026-08-11-184) describes.

#### Original entry


`toggle_line_comment.1000_lines` traces to 2.35 allocations per toggled line, and
two of them are structural, not wasteful:

  - `BuildToggledCommentRegion` builds the line's new text — one owned string per
    line, which `ReplaceLines` then consumes.
  - `BuildLineHistoryEntry` slices the replaced span into `before_lines` — one
    owned string per line, for undo.

Both are `std::vector<std::string>` members of `HistoryEntry`, so a line-shaped
edit costs 2n allocations and the constant is not reducible while the entry is a
vector of strings. Every large shaping op pays it: toggle comment, sort lines,
move line, indent/dedent, multi-caret shaping.

The exit is a blob-backed entry — one `std::string` holding the whole
before-image and one holding the whole after-image, plus a `std::vector<uint32_t>`
of line offsets — which turns 2n allocations into 4 regardless of n, and makes
the entry's memory footprint one contiguous buffer instead of n heap blocks (it
also removes n destructor calls per undo/redo). `ApplyHistoryEntry`,
`SetLastAppliedEditFromEntry`, the coalescing paths and the LSP-sync bridge all
read `before_lines`/`after_lines` as vectors today, so the change is a real
refactor with a clean measurement attached. Related: [131](#td-2026-08-05-131)
took the same argument for the *single-line* case and shipped the column-scoped
entry; this is its multi-line half. [157](#td-2026-08-06-157) capped the *range*
an entry covers; this caps the cost *per line inside* that range.

### TD-2026-08-10-181 — twelve phase allocation gates fail on an unchanged binary, and three of them measure a re-open rather than an open. [RESOLVED 2026-08-12 — item 1 shipped; item 2 split out as [190](#td-2026-08-12-190).]

Item 1 (declare `warmup_iterations` where the first iteration legitimately warms a
cache) shipped on 2026-08-10. Item 2 — the three merge scenarios that name an
operation they never run — is a scenario-semantics change with a rerecord attached
and is now its own entry, [190](#td-2026-08-12-190), so this one stops being
half-open forever.

The open question this entry raised last ("should phase allocation gates carry the
same below-baseline-iteration-count non-enforcement the retention metrics have?")
is answered by the same mechanism [186](#td-2026-08-12-186) built: a gate that
cannot be trusted in the current conditions is reported and explicitly not
enforced, with the reason on the verdict line, rather than either failing
meaninglessly or being deleted.

#### Original entry


Found while tracing [159](#td-2026-08-06-159): `compare_scroll_large_fixture`
FAILed a phase gate at `--iterations=2` and PASSed at 6, on the same binary. The
whole suite then does the same thing. **Twelve `p50_allocations` phase gates are
functions of the iteration count**, on code that did not change:

| phase | baseline (10 iters) | measured at 2 iters |
| --- | ---: | ---: |
| `merge_large.open_to_first_paint` | 185 | 27,293 |
| `merge_interleaved.open_to_first_paint` | 179 | 6,205 |
| `merge.open_many_conflicts` | 180 | 6,143 |
| `merge_large.scroll_burst` | 3,200 | 14,682 |
| `file_finder_cold.open_finder` | 65 | 367 |
| `linter.publish_diagnostics` | 303 | 646 |
| `open_tab.with_indent_detect` | 180 | 252 |
| `editor_sticky_scroll_scroll.fast_scroll_frame` | 9,814 | 11,547 |
| `multi_project.switch_cycles` | 17,390 | 19,497 |
| `diff.stage_hunk` / `diff.stage_selected_lines` | 2,070 / 2,088 | 2,283 / 2,310 |
| `column_selection.extend_down` | 0 | 15 |

Every one of them PASSes at `--iterations=10`. Re-verified: the same list, run at
10, is green apart from wall gates on a loaded machine.

**Why this is not just "use ten iterations".** The harness already knows this
class of problem and refuses to enforce `mean_rss_growth_bytes` and
`p50_net_heap_bytes` below the baseline's iteration count, saying so in the
verdict line ([148](#td-2026-08-06-148), [173](#td-2026-08-10-173)). Phase
allocation gates get no such guard — precisely because they are *presented* as
the deterministic ones. And the workflow this repository documents for finding
where a phase allocates (`perf-harness.md` § Finding *where* a phase allocates)
says to run `--iterations=2`. So the documented debugging loop produces twelve
red gates and nothing explains them.

**The three merge entries are a different and worse problem: those gates do not
measure what they are named.** A baseline of **185** allocations for "open a
large merge to first paint" is not an open. Each merge scenario re-runs its
`merge ...` command per iteration against a driver that reuses the already-open
merge tab, so iteration 0 pays the real cost (27,293) and iterations 1..9 pay a
re-show. The p50 lands on the re-show. `merge_model_build_interleaved` exists
*because* of this — its own comment says "NOTHING gated it: every merge scenario
shares one driver across its iterations and reuses the already-open tab, so the
build lands on iteration 0 and is absorbed by the warmup" — but the conclusion
drawn there was to add a scenario, not that the three existing gates are naming
an operation they never run. `max_allocations` (tolerance +50 %) does still see
iteration 0, so these are half-vacuous rather than vacuous.

**Two separate fixes, and only the first is cheap:**

1. Scenarios whose first iteration legitimately warms a cache declare
   `warmup_iterations`, as `editor_buffer_find_incremental` and
   `editor_shaping_multi_caret` already do. Done for
   `compare_scroll_large_fixture` (its `open_to_first_paint` is ~630 cold / ~318
   warm, and a 2-iteration p50 averaged the two into 474 against a +10 % gate).
2. The merge scenarios must close the merge tab between iterations so every
   iteration opens one, then have those phases re-recorded with
   `measurement_revision` bumped per [167](#td-2026-08-07-167). `ScenarioContext`
   has no close-tab verb and `WorkspaceShell::TestAccess` exposes none; both need
   adding. **Blocked on a quiet machine** — the rebaseline moves the wall/cpu half
   of three baselines, same constraint as [161](#td-2026-08-07-161)/[172](#td-2026-08-10-172).

Worth considering alongside (2): whether phase allocation gates should carry the
same below-baseline-iteration-count non-enforcement the retention metrics have.
The argument against is that it would soften a gate that is supposed to be exact;
the argument for is the table above, which is what the gate actually does today.

`column_selection.extend_down` is its own small case: a **zero** baseline is an
exact gate (`WithinTolerance` gives a zero-width envelope), so its 15 allocations
at 2 iterations are a hard fail with a meaningless "+0%" in the verdict line.
Either it warms up or the metric's zero-baseline reporting needs the treatment
`AddAbsoluteMetric` already gives net-heap.

### TD-2026-08-10-179 — a perf scenario measured a poll loop timing out, and called the number authoritative. [RESOLVED 2026-08-10.]

Found while chasing what looked like a +32.7 % allocation regression in
`linter_on_save`. It was not a regression, and the phase had never measured what
its name says.

**Nothing in this repository lints JavaScript.** There is no linter service, the
fixture (`tests/perf/fixtures/linter_project`) carries no linter config, and its
`node_modules/` is empty — so `WaitForDiagnostics` never returned true. Every run
since the scenario was written spun the full 120 ms deadline, found nothing, and
reported the poll loop's own allocations as a baseline-gated number.

That number is **a duration wearing an allocation's clothes**. The loop pumps a
frame per poll iteration, so its count is "how many times a wall-clock loop got
round" — which moves with machine load and not with any code:

    one unchanged binary, three runs:   3,561 / 3,644 / 3,711
    committed baseline (idler runner):  2,745      tolerance +10 %

Gated at the 10 % the suite reserves for the metric whose whole justification is
that it is *byte-identical run to run*. The tell that finally broke it open was
raising the timeout to 500 ms: the scenario then SKIPped, which is what said the
wait had always failed.

**Fixed** by driving the path the scenario exists to measure instead of waiting
for a tool that is not there. 24 diagnostics are published through the store
exactly as an LSP publish would, then three frames apply and paint them — store
update, decoration invalidation, squiggles, status counts. New
`ScenarioContext::PublishDiagnostics` / `HasDiagnostics` back it, and the scenario
`SkipScenario()`s if the publish did not land: the vacuity guard the old phase
never had. `linter.publish_diagnostics` now reads 303 allocations, identical
across three runs; `measurement_revision` bumped to 2 per
[167](#td-2026-08-07-167).

**Generalisable, and the sweep is not done.** This is a *fourth* shape for
[159](#td-2026-08-06-159)'s list and a new one for
`dev-docs/project/validation-traps.md`: **a measured phase whose body is a
wall-clock wait**. Grep the harness for `Measure(` wrapping a `WaitFor*`; each is
a gate on the runner, not on the code. `WaitForDiagnostics`,
`WaitForFileIndexPath` and `WaitForProjectSearchFinished` all have this shape, and
only this one has been audited. **Filed as TD-2026-08-10-180.**

### TD-2026-08-10-180 — the other two `WaitFor*` helpers have never been checked for the trap 179 found. [RESOLVED 2026-08-10 — clean, and the check is now a lint.]

[179](#td-2026-08-10-179) found one measured phase whose body was a wall-clock
poll loop, making its allocation count a function of machine load rather than of
code. The harness has two more helpers of exactly that shape —
`ScenarioContext::WaitForFileIndexPath` and
`ScenarioContext::WaitForProjectSearchFinished` — and neither has been audited.

Both poll to a deadline and pump frames per iteration, so any scenario that calls
one INSIDE `Measure(...)` has the same defect. `WaitForDiagnostics` had it;
whether the other two do depends on each call site.

**Method**: `rg -n 'Measure\(' -A4 tests/perf/*.cpp | rg 'WaitFor'` for the call
sites, then for each one run the scenario three times on an unchanged binary and
compare the phase's `p50_allocations`. A spread of more than a few allocations
means the phase is timing the runner. The fix shape is 179's: move the wait
outside the measured window and measure the deterministic work the wait was
waiting FOR, bumping `measurement_revision`.

Also worth checking in the same pass: a scenario whose wait always TIMES OUT is
measuring nothing at all, which is how 179 sat undetected. A wait whose success is
load-bearing needs a `SkipScenario()` on failure, not a `(void)` cast.

#### Audited 2026-08-10: `linter_on_save` was the only one

There are exactly three measured bodies in the suite that block, and the other
two are clean **by construction**, which is the useful part of the result:

- `search_first_result.search_to_first_result` wraps
  `WaitForProjectSearchFinished`. That helper deliberately does **not** pump the
  shell while spinning and drains exactly once after completion — its own comment
  says both, and gives the determinism reason. Its spin is a flag check plus
  `sleep_for`, so it allocates nothing. Three runs of one unchanged binary:
  **20,149 / 20,149 / 20,149**.
- `repo_open.idle_500ms` wraps `context.Wait(500ms)`, where the fixed idle IS the
  measurement. The shell reports `Idle`, so `Wait` sleeps in 20 ms slices without
  allocating. Three runs: **331 / 331 / 331**.
- `WaitForFileIndexPath` pumps frames per poll and WOULD have the defect, but no
  call site is inside a `Measure(...)`; all five sit in setup.

**The check is now mechanical rather than a one-time audit.**
`CheckPerfMeasureBodiesDoNotWaitOnWallClock` is a hard-fail architecture rule,
built as a sibling of `CheckPerfMeasureBodiesDoNotBuildTheirOwnInput` and sharing
its shape: it extracts each `Measure(...)` lambda body, flags `WaitFor*(`,
`context.Wait(` or `sleep_for(`, honours an in-body
`perf-measure-waits-on-clock: <reason>` exemption, and fails loudly if it finds no
`Measure` body at all rather than reporting green while blind. Four control
fixtures cover the defect, the fix, the declared exemption and the blind case; the
rule was also verified against the real tree by reintroducing 179's exact line,
which it flags at `tests/perf/PerfMain.cpp:820`.

The two clean cases carry the exemption comment with their three-run numbers in
it, so the claim is checkable where a reader is already looking.

### TD-2026-08-10-178 — the allocation tracer's "top sites" table was a ranking of the earliest sites. [RESOLVED 2026-08-10.]

The instrument [159](#td-2026-08-06-159)'s whole method depends on. Pointed at
`switch_and_idle.switch_and_settle` it printed:

    [alloctrace] 6188 allocations in [1, 1000000] bytes from 1024 distinct sites
    [alloctrace] WARNING: 17807 allocations were dropped, the site table is full

74 % of the phase unattributed — and the failure is worse than the warning
admits. The site table is open-addressed with a fixed 1024 buckets; once full,
every NEW site is dropped **whole** rather than merged anywhere. So a table
printed "most frequent first" contains only sites that appeared *before it
filled*, and the phase's real top site can be absent from it.

It was. After enlarging the table, site #1 is 720 allocations
(`ProjectTabTooltipLabel`, per project tab per painted frame) and the old table's
"#1" — 192 allocations — is #4. A sweep reading the old output would have ranked a
biased sample and moved on, which is exactly the reading
`dev-docs/project/validation-traps.md` exists to prevent.

**Fixed**: `kTraceBuckets` 1024 → 65536 (~14 MB of BSS in the perf harness build
only, faulted on first touch); the truncation warning now says what it *means* —
that the ranking below is not a ranking — with the drop percentage rather than
naming a constant to raise; the bucket index array moved off the stack (half a
megabyte would have blown a default thread stack); and
`DumpTracedAllocationSites` raises the existing `t_in_trace` recursion guard for
its whole body, because it allocates while holding the same non-recursive mutex
`RecordAllocationSite` takes — a self-deadlock without whole-run phase scoping,
not a miscount. The same phase now reports 23,995 allocations from 2,618 sites,
zero dropped.

### TD-2026-08-10-177 — a Make*Coordinator hook capturing another Make*Service() by value is a heap allocation per hook, per event. [RESOLVED 2026-08-10 — audited clean beyond the two sites, and linted.]

Found by tracing `editor_scroll_only_no_content_bump.scroll_frame`:
`MakeTabMouseCoordinator` and `MakePanelMouseCoordinator` were in the top six
sites at **288 bytes an allocation**. 288 is `sizeof(TerminalPanelService)` — nine
`std::function`s.

Both factories built one service and captured it BY VALUE into their terminal
hooks. A 288-byte capture overflows `std::function`'s small-object buffer (16
bytes on libstdc++), so each capture heap-allocates its own copy of the whole
service — twice in the tab coordinator, **seven times** in the panel one — and
both coordinators are constructed per mouse event, the wheel handler included.

Fixed at those two by constructing the service inside the lambda body from
`this`, which costs nothing (every one of `TerminalPanelService`'s own hooks is a
bare `this` capture and fits inline):

    scroll_large_file                     582 -> 259   -55.5 %
    editor_scroll_only_no_content_bump  7,926 -> 7,122  -10.1 %
    editor_indent_guides_paint          8,704 -> 8,046   -7.6 %
    editor_sticky_scroll_scroll        12,722 -> 11,918  -6.3 %
    terminal_scroll_long_output         5,897 -> 5,511   -6.5 %
    editor_fold_viewport_refresh       12,384 -> 11,612  -6.2 %
    editor_render_whitespace_paint     10,428 -> 9,784   -6.2 %

**What is left, and why it is filed rather than closed.** The shape is invisible
at the call site — `[some_service]` reads like any other capture — and this repo
has ~20 `Make*Service()` / `Make*Coordinator()` factories, several of which return
objects of the same order. Nobody has looked at the others.

#### Audited and linted 2026-08-10

`rg -n 'auto \w+ = Make\w+(Service|Coordinator)\(\);' src/workspace` finds five
factory results bound to locals. **None of the other three are captured** — they
are used directly in the enclosing function. A wider sweep for by-value captures
of non-`this` locals across `src/workspace` turned up only small values (ids,
pointers, an int) and the `std::move`-captures that async work legitimately needs.
So the two fixed sites were the whole of it.

`CheckFactoryResultsAreNotCapturedByValue` now keeps it that way: a hard-fail rule
that finds those locals and flags any standalone `[x]` capture-list entry,
code-masked, with the loud-missing-target guard. Four control fixtures — the
defect, the fix, a reference capture plus a comment mention, and the blind case.

**Two drafts of this rule were wrong, and how each was caught is the reusable
part:**

- The first used `[^]` inside the capture regex's negated class. ECMAScript reads
  a leading `[^]` as **"any character"**, so the pattern matched everything and the
  rule reported green against the very defect it was written for. The negative
  fixture did **not** catch this — it "passed" too. What caught it was
  reintroducing the real line in the real tree and watching the rule stay silent.
- The second flagged `[&terminal_panel]`. A reference capture is free, and the
  surrounding characters are the whole distinction, so the pattern now anchors on
  a capture-list opener or separator instead of scanning the interior. The third
  control fixture caught this before it shipped.

The generalisable rule, for `dev-docs/project/validation-traps.md`'s list: **a
lint whose regex was never run against a known positive is not a lint.** Write the
fixtures, then reintroduce the original defect in the real tree and watch it fail.
A fixture that passes proves the fixture, not the rule.

### TD-2026-08-10-168 — a third of the suite's allocation gates were red, because process isolation moved a cost the baselines never contained. [RESOLVED 2026-08-10.]

Found while sanity-checking an unrelated perf number: `editor_scroll_only_no_content_bump`
failed its p95/max allocation gate at **the exact commit that recorded its
baseline**, on the machine that recorded it. A full-suite run said **34 of 100
scenarios FAIL**, every one of them on `p95_allocations` / `max_allocations`,
every one of them with a `p50_allocations` that matched its baseline **exactly**.

That combination is the whole diagnosis. A code regression moves the median. Only
the tail moved, and it moved by a near-constant amount:

| scenario | committed max | measured max | p50 |
| --- | ---: | ---: | ---: |
| `cold_startup_no_project` | 681 | 9,364 | 101 (exact match) |
| `typing_small_file` | 3,703 | 12,382 | 249 |
| `compare_tab_open` | 2,445 | 11,114 | 187 |
| `merge_tab_open` | 1,733 | 10,416 | 1,196 |
| `window_resize_stress` | 3,853 | 12,422 | 1,264 |

~8,600 allocations, in exactly one iteration, in scenarios with nothing in
common. `cold_startup_no_project` is `PumpFrames(5)` and nothing else, so the
cost is the first frames a **process** ever paints — lazy first-paint state that
is global to the process, not to the scenario.

The proof is one flag: with `--no-isolate`, `typing_small_file` **passes** —
because `cold_startup_no_project` ran before it in the same process and absorbed
the cost. That is the regime the baselines were recorded in. Per-scenario process
isolation ([152](#td-2026-08-06-152)) then gave every scenario its own process,
so every scenario pays it again, in its own iteration 1, where it governs p95 and
max.

**Fixed** by warming the process — not the scenario — before the measured loop:
three pumped frames on the bare driver in `PerfHarness::RunScenario`. Deliberately
not `warmup_iterations`, which runs the whole scenario and would also warm what a
scenario means to measure cold (a project open, a cold finder index). After it,
every allocation gate in the run passes against the **unchanged committed
baselines** — which is the confirmation that matters: the fix restores the regime
the baselines describe rather than moving the baselines to match a new one.

**Generalisable, and it is [167](#td-2026-08-07-167)'s thesis with a second
witness**: a harness change can invalidate a baseline without touching a line of
product code, and nothing in the baseline file records which regime it was
recorded in. 152 landed a day before the rebaseline and the rebaseline still came
out of the old regime — so "rebaseline after the harness change" is not by itself
a defence. Related trap: the tail metrics were the only ones affected, and the
suite's habit of reading p50 first is what let it sit.

**Left open, and closed 2026-08-10 with 167.** Nothing gated this: a scenario
whose p50 matches its baseline exactly while its max is 13x should be a loud,
named condition in the harness, not something a human notices while chasing
something else. `CompareToBaseline` now attaches **TAIL-ONLY DIVERGENCE** to a
failing `p95_allocations`/`max_allocations` whose baseline-relative factor is
≥ 2x while `p50_allocations` is within 1 % of its baseline, saying that a code
regression moves the median and that the extra work is therefore a property of
the process. It does not change the verdict — the tail gate failed on its own
merits and a real tail regression exists — and it deliberately stays quiet when
the median moved (an ordinary regression, which the note would misattribute) and
when the tail merely drifted past a loose envelope. All three cases are covered
by `PerfBaseline/NamesTailOnlyAllocationDivergence`, using this entry's own
numbers (`cold_startup_no_project`, 681 → 9,364 with p50 matching exactly).

### TD-2026-08-10-174 — `lexically_normal()` is ~12 allocations and the codebase calls it 416 times, three of them on paths that run per keystroke. [RESOLVED 2026-08-12 — the shape now has a helper, and the four heaviest files are swept.]

Closed as an entry, not as a standing invitation to grep for the call. What was
missing was not effort but a HELPER: every pass so far hand-rolled the guard at
the sites it happened to look at, which is why the same shape kept being
rediscovered from a different end.

**2026-08-12 pass.** `util/PathMatch.h` grew the two guarded forms, on top of the
allocation-free `PathTextNeedsNormalizing` scan that already existed:

- `NormalizedPath(p)` — an owned path, still a copy when already normal (two
  allocations) but not twelve.
- `NormalizedPathView(p, scratch)` — a reference to the INPUT when it is already
  normal, so the common case allocates nothing at all.
- `PathEqualsOrWithinNormalized(candidate, root)` — the same guard applied to the
  containment test, which is where the eager-argument shape lived:
  `PathEqualsOrWithin(x.lexically_normal(), root)` builds a whole path before the
  test can reject it (the recurring "expensive value computed as an ARGUMENT and
  then thrown away by a guard inside the callee" shape).

Swept the four heaviest files — the two path-mutation coordinators (a rename walks
every open tab, every deferred handle and every restored editor state), the diff
tab coordinator and the plugin host — for 63 call sites.

The tests pin both halves: the answer must equal `lexically_normal()` for every
path shape, and the already-normal case must hand back a reference to its own
input, which is the observable form of "this did not allocate". The remaining
~350 calls are in cold paths (session restore, one-shot project setup, tests) and
are a `NormalizedPath` substitution away whenever one of them shows up in a
trace — which is what [159](#td-2026-08-06-159)'s sweep is for.

#### Original entry


#### 2026-08-11 pass: two more per-item sites, found by tracing rather than by grep

Both were named in this entry's own "still open" list, and both were found again
from the other end — by reading a phase ([159](#td-2026-08-06-159)) rather than by
grepping for the call.

- **`MakeGitRepositoryPathIdentity`** normalized every entry of every git refresh.
  git's porcelain output is already relative, '/'-separated and normal, so the
  call could only ever return its input. Guarded; it also copied the generic text
  into `display_label` and dropped the original, which is now a move.
  `git.refresh_dispatch` -14 % / -12 % on its two scenarios.
- **`DirectoryTree`'s per-item sites** (this entry named them explicitly). Its
  expansion-key probe ran `absolute()` — a **getcwd syscall**, not just
  allocations — plus `lexically_normal()` plus `generic_string()`, once per
  candidate entry of every rebuild and once per ancestor in the
  manually-collapsed walk. The two key sets are transparent-hash now and
  `ContainsPathKey` probes them through a view into the path's own text.

**Still open**, unchanged: the ~27 single normalizations in
`WorkspaceShellPlugins.cpp`, and the rest of the ~390. And note the shape
[183](#td-2026-08-11-183) describes — guarding the *normalization* does not help
when the cost is constructing the `path` at all.

#### 2026-08-10 pass 2: every remaining `x.lexically_normal() == normalized` scan

`rg -n 'lexically_normal\(\) [=!]= ' src` is the mechanical grep for the shape the
previous pass named and only half-swept: normalize the query once, then
re-normalize every candidate to reject it. Ten hits, all converted to
`util::SameAsNormalizedPath`, which answers a mismatch between two normal paths
with a string compare.

The one that runs unattended is **`ProjectChangeCoalescer::MergeFileChange`, on
the watcher thread**: its comparator normalized BOTH sides per candidate, over a
pending list the cap allows to reach 1,024, once per file change. A build writing
files paid ~24 allocations per comparison to answer "different file". Its own
header comment already named this ("a linear scan with two path normalizations
per comparison") as the *reason for the cap* rather than as something to fix.

The rest are open-tab / project-root / merge-output scans in the plugin edit
apply path (`WorkspaceShellPlugins.cpp`, 5 sites), session restore, project-change
invalidation, and the tab coordinator. `WorkspaceTabState.h`'s `EditorViewPathIs`
was a byte-identical private copy of `SameAsNormalizedPath`'s two guards, written
before the helper existed, and is now its caller.

**Still open**, and unchanged by this pass: the ~27 remaining `lexically_normal`
in `WorkspaceShellPlugins.cpp` that are single normalizations rather than scans,
`DirectoryTree`'s per-item sites, and the ~390 elsewhere. `GitBlameService` is
already clean — it memoizes both its root text and its path key thread-locally.


#### 2026-08-10 pass: the per-filesystem-entry and per-frame sites

Ranked by how often the call runs, as the entry's method says, and the top of that
ranking was not a keystroke path at all — it was **once per filesystem entry of
every tree walk in the app**: the initial file-index build (documented in
`FileIndexWatcher` as "the dominant single cost of opening a project"), the poll
re-walk, the inotify registration, the sidebar tree and the project scanner all
funnel through `ProjectTraversalFilter::Includes`.

Every entry paid a `lexically_normal()` of an already-normal path, a
`lexically_relative()` to derive the relative text, a `parent_path()` to find the
ignore matcher, a `generic_string()` key for the matcher cache, and one more path
per ancestor in the pruning walk. None of it is needed: a directory iterator hands
back an already-normal path, and the containment check has already proved the root
is a string prefix — so the relative part is that prefix removed and the ancestors
are that text trimmed at each separator.

Two new helpers in `util/PathMatch.h` do it as views into the caller's own text:
`NormalizedRelativeView` (the allocation-free companion to
`NormalizedPathEqualsOrWithin`, sharing its preconditions) and
`NormalizedParentDirectoryView`. New gated scenario
`project_traversal_filter_scan` pins it at 2,048 entries:

    project.traversal_filter_scan   46,208 allocations -> 0     (12.6ms -> 0.70ms)

A **zero** phase baseline, which `WithinTolerance` turns into an exact gate: one
re-added allocation on this path fails the run.

**A third helper, for the shape the entry's method does not cover.** Eight sites
shared one form: normalize a query path, then walk a collection comparing each
element's path to it (open tabs, review targets, editor groups). Each re-normalized
the ELEMENT per iteration — ~12 allocations to answer "no" — so the cost scaled
with the collection, not with the match. `util::SamePathNormalized` is the wrong
tool and was never what these used: it normalizes BOTH sides on a mismatch, and a
scan is mostly mismatches. New `util::SameAsNormalizedPath` takes an
already-normalized reference and answers a mismatch between two normal paths with a
string compare. `TabCoordinator::DiskSignatureMatchesOpenView` had already
hand-rolled it inline and is now its caller rather than its second copy;
`BranchReviewStateService::PruneForRepository` scanned its list twice with the same
per-element normalization and now shares one predicate.

**Per-frame sites, found via the tracer rather than by grep.**
`WorkspaceShell::ProjectTabTooltipLabel` normalized a catalog root once per project
tab per painted frame (and `ProjectCatalogRoot` returned that path BY VALUE);
`BuildPersistedEditorTabState` normalized once per open tab of every group on every
session save. Both guarded.

**Still open**: `rg -n 'lexically_normal' src | wc -l` is still ~400. What this pass
did NOT do is the per-item sites in `DirectoryTree` (the sidebar's own normalize
per entry), `GitBlameService`'s cache keys, and the ~27 in
`WorkspaceShellPlugins.cpp`. Rank by how often the call runs and confirm with the
phase tracer — and note that the tracer itself was lying until
[178](#td-2026-08-10-178).


Three independent fixes in the 2026-08-10 pass were the same one line:

| site | runs | fix |
| --- | --- | --- |
| `FileUriForPath` | 3x per keystroke (LSP sync) | guard with `util::PathTextNeedsNormalizing` |
| `BreakpointStore::PathKey` | per applied edit | same guard, plus a view-returning form for the probe |
| `RelativePathLabel` (2026-08-07) | 2x per tab per tab-strip rebuild | same guard, plus `NormalizedPathEqualsOrWithin` |

`lexically_normal()` costs a fresh `std::filesystem::path` plus a component list
holding a string per component — ~12 allocations — and it is a **no-op** for a
path whose text is already normal, which is nearly every path the editor holds:
the project catalog, the git status ingress and the branch-review store all
normalize once on the way in. `util::PathTextNeedsNormalizing` is the
allocation-free scan that confirms it, and it has now paid for itself three
times, found one at a time by tracing three unrelated phases.

`rg -n 'lexically_normal' src | wc -l` says **416**. Nobody has looked at the
other 413. The ones that matter are the ones on a per-keystroke, per-frame or
per-item path; the ones at file-open or project-activation are fine as they are
and should be left alone.

**Method, and why it is not just "add the guard everywhere"**: the guard is only
correct where the caller wants the *normalized text*, not a normalized `path`
object, and where the fallback still runs for an unusually spelled input. Two of
the three fixes also needed a second guard on the other side of the comparison
(`RelativePathLabel` needed the root to be normal too, and the first attempt at
it guarded the wrong side — skipping normalization when the two paths compare
equal is nearly worthless, because a scan is mostly mismatches and every mismatch
still normalised in order to be rejected). Rank the hits by how often the call
runs, confirm with the phase tracer, and read
[159](#td-2026-08-06-159)'s tab-strip section before starting.

### TD-2026-08-10-173 — the retention gate reported a 476 % regression on an unchanged binary, because a median over a settling series is not a measurement at five iterations. [RESOLVED 2026-08-10.]

    editor_indent_guides_paint  p50_net_heap_bytes  59,735 -> 297,236  (+398 %, tolerance +10 %)   --iterations=5
    editor_indent_guides_paint  p50_net_heap_bytes  59,735 ->  49,334  (PASS)                      --iterations=10

Found while diagnosing what looked like a real retention regression: **five of
twelve** editor scenarios were red on `p50_net_heap_bytes` at `--iterations=5`,
one of them at +398 %, on a metric the suite documents as *deterministic to the
byte*. The two edit-latency scenarios reproduced identically at `035f8e1c` — the
commit right after the v2.9.0 cut, before every recent perf change — so nothing
in the code had moved.

The series is the whole answer. Same binary, ten iterations:

```
12,286,512  341,770  297,236  63,389  -7,535  29,836  48,084  -20,348  50,584  25,080
```

p50 over the first 3 is 341,770; over 5, 297,236; over all 10, **49,334**. The
first iterations fill caches the later ones reuse, so the median walks down the
series and does not converge until they are full. A short run therefore gates a
settling reading against a steady-state baseline, and the failure line named the
metric, the percentage and the envelope — everything except the sample size.

**This is [148](#td-2026-08-06-148) again, on the other metric.** That entry
found exactly this for `mean_rss_growth_bytes` (100-114 KB at 6 iterations,
84-95 KB at 10) and taught `CompareToBaseline` to decline the comparison and say
why. `p50_net_heap_bytes` is the same shape — a statistic over a settling series
— and did not get the guard, so it kept generating false positives for four
days. Same "a fix applied to one of N instances of a pattern leaves the other
N−1" as [171](#td-2026-08-10-171).

Fixed by generalising the guard: `AnnotateSettlingGateForIterationCount` now
serves both metrics, naming the metric and its statistic. A run shorter than its
baseline reports the number, is not gated on it, and says
`rerun with --iterations=10`; a LONGER run stays gated with a note that the gate
is loose. Everything else in a short run stays armed, or dropping `--iterations`
becomes a way to turn the suite off. Covered by
`PerfBaseline/DeclinesTheNetHeapGateOnAShortRun` using the series above.

**Generalisable, and worth more than the fix**: the suite's habit is to reach for
`--iterations=3` or `5` when iterating. Every metric read at a short count is
suspect unless it has been shown to be steady from iteration 1, and only two of
them have been checked. `p50/p95/max_allocations` are safe by construction (each
iteration is independently counted, and the percentiles of a settling series are
what the tail gates are FOR). The duration metrics carry machine state, which is
already loudly annotated. But nothing systematically asks "does this statistic
converge?" of a metric before gating it, and the two that did not were found one
at a time, by accident, four days apart.

**Left open**: this said nothing while it was wrong, which is the same one-sided
gate [170](#td-2026-08-10-170) closed for skips. A red that is an artifact of the
harness costs a bisect; the run should be able to say "you asked for 5 of 10
iterations — 1 gate declined" in its summary line rather than only in a
per-metric note nobody greps for.

### TD-2026-08-10-172 — `git_sidebar_activate`'s timing baseline describes a fixture that no longer exists. [RESOLVED 2026-08-12 — the gate stopped asserting a fiction.]

The fixture contract and its ctest setup shipped 2026-08-10. What was left was a
wall/cpu rerecord on an idle runner, which meant the gate went on enforcing
numbers taken against a fixture that is not there any more — failing for a reason
that has nothing to do with the code, on every run, indefinitely.

Re-recorded 2026-08-12 with the mechanism [186](#td-2026-08-12-186) built: the
deterministic half is measured against the fixture that actually exists and gates
(p50_allocations 1,266), and the timing half is marked
`timing_is_advisory`, so it is reported and explicitly NOT enforced with the
reason on the verdict line rather than enforced against a fiction. An idle
`--update-baseline --reference-runner=perf-runner-v1` arms the timing half
whenever one is available.

That is the general shape worth keeping: when a gate cannot be made correct here,
making it say so beats leaving it wrong.

The scenario names `tests/perf/fixtures/git_status_project`. Nothing produced
that tree: `generate_git_workstation_fixtures.sh` builds six git fixtures and not
that one — its own comment says "same layout as **git_status_project**", so the
tree was renamed or dropped and the scenario was never repointed. The fixture
guard then skipped it QUIETLY on every run, while its committed baseline stayed
in the set and got differenced across releases (it is one of the three "regressions"
[167](#td-2026-08-07-167) had to explain away for v2.9.0).

Found by [170](#td-2026-08-10-170)'s one-policy skip, which is the entry's point:
the scenario had been not-running for long enough that nobody could say when it
stopped.

**Done**: the generator now builds `git_status_project` (200 tracked files, 40
modified, 10 untracked), the scenario runs, and its **allocation** half is
rerecorded — 733 → 2,409, deterministic to the sample (p50 == p95 == max), which
is the property that scenario's comment fought for.

**Open**: the wall/cpu half of its baseline is still the old tree's, carried over
by `--update-baseline=deterministic` because this session's machine was thermally
throttled (calibration moved 4x mid-run). Rerecord it with a full
`--update-baseline` on an idle runner.

**Also open, and larger** — RESOLVED 2026-08-10: the git fixtures were the one
family with no ctest setup. The other three generators are `FIXTURES_SETUP` tests
with `--ensure`; this one was a shell script a human had to remember to run, it
had no `.sha256` contract, and it `rm -rf`'d seven repositories every time. A
fresh checkout could not run any git-workstation scenario.

`generate_git_workstation_fixtures.sh` is now
`generate_git_workstation_fixtures.py`, sharing the one `ensure_fixtures`
implementation with the other three generators (which grew a per-spec hasher for
it), with seven committed `.sha256` manifests and a
`microide_perf_fixtures_git` ctest setup on the same `perf_fixtures` label.
`--ensure` is 3.8 s when everything matches; a full rewrite of all seven is 6.5 s
(the bash version was minutes).

**A git fixture's manifest is not a plain content hash**, and that is what makes
it close [155](#td-2026-08-06-155)'s still-open half. It digests the worktree
(skipping `.git`, whose object layout and index stat data are not reproducible)
and then folds in `git status --porcelain`. So the **index** is part of the
contract: the staging scenarios `git add` against these repositories, and a
left-behind index now invalidates the manifest and gets regenerated on the next
run instead of becoming the next measurement's starting state. Verified by
staging `git_large_diff_project` by hand and watching `--ensure` heal it. This is
the cheap version of that entry's "copy the tree into a per-run sandbox" — it
does not stop a scenario polluting a tree *within* one run, but it does stop
pollution surviving between runs, which was the part that made baselines
unportable.

**Two trees on this checkout were polluted, and nothing had noticed**, which is
the entry's own point arriving a second time. TD-2026-08-06-155's truncate-back
fix works (verified: five iterations of both external-change scenarios leave the
files at their generated size), but the residue from *before* that fix was never
cleaned up: `git_large_diff_project/src/large.cpp` still carried 130 leftover
appends (a 263-line worktree delta where the generator writes 3) and
`git_many_conflicts_project/current.cpp` 90. The six affected baselines were
recorded against those polluted trees. Regenerating dropped every one of their
allocation figures — `merge_open_many_conflicts` 327 → 263 p50 / 16,794 → 12,507
max, `external_change_refresh_open_diff` 31,997 → 31,878, and
`external_change_refresh_open_diff`'s `p50_net_heap_bytes` gate stops failing at
3.02 MB against 27,959 — and the deterministic half is rerecorded. The
wall/cpu half is untouched (this machine is loaded); it describes a slightly
larger diff than the fixtures now hold, so it is loose rather than wrong.

**Deleted while here**: `tests/perf/generate_git_fixture.sh`, a second,
unreferenced generator for the *same* `git_status_project` path that wrote a
different tree (1,000 clean tracked files, no worktree delta, no untracked). Two
generators disagreeing about one fixture path is most of how this entry happened;
running it would now also break the manifest, so it goes rather than gets
repointed.

**Still open**: git fixtures are not in `PerfMain`'s `kManifestBackedFixtures`
in-process integrity check, because the porcelain half of their digest is not
something the perf binary can compute without shelling out to git. The ctest
setup is the only enforcement, so a lane that runs `microide_perf` directly
(perf-canary) gets none — the same gap `file_finder_large` already has.

### TD-2026-08-10-171 — two architecture tests passed only on an idle machine. [RESOLVED 2026-08-10.]

    ArchitectureInvariants/PluginRules     221 s under TSAN, machine idle
    ArchitectureInvariants/TerminalRules   ~230 s
    microide_tests per-test watchdog       300 s

Found by the end-of-session sanitizer batch: `tsan` failed two shards with
"Subprocess aborted", and the abort was the test runner's own watchdog firing on
these two. Each ran alone in ~75 % of the budget, so the moment ctest scheduled
six sanitizer shards on a four-core box they went over. Nothing about them had
changed — they had been sitting one busy machine away from red.

The suite already had the answer and had applied it to exactly one of the three
rule groups: `WorkspaceArchitectureRuleList` exists so the test layer can register
**one ctest case per rule**, and its own comment says why ("formerly a single ~30 s
serial test"). Plugin and Terminal kept their aggregate `Run*ArchitectureRules`
entry points and stayed single cases. They now expose the same `NamedRule` list —
one source of truth each, iterated by both the aggregate runner and the
registration loop — and the ArchitectureInvariants test count goes 65 → 87, with
the slowest single case at ~4 s natively.

**Generalisable**: a fix applied to one of N instances of a pattern leaves the
other N−1, and a per-test timeout converts "slow" into "flaky under load" rather
than into a visible failure. Both are invisible on the machine where the fix was
written.

**And it happened again the same day, one layer down.** The next TSAN batch
failed one shard on
`ArchitectureInvariants/Workspace/CheckCoordinatorOperationsAreCalled` —
`Subprocess aborted` at 326 s, zero ThreadSanitizer warnings, so the watchdog and
not a race. That rule was *already* one ctest case per rule; splitting was the
wrong axis for it, because a single rule was the slow thing. It compiled a fresh
`std::regex` per `Operations` field and ran it over every file in that field's
include scope — ~200 fields x ~1,000 files, the whole tree rescanned two hundred
times. Collecting each file's `.`/`->` identifiers once and looking each field up
took it 3.3 s → 0.62 s.

The sweep the entry's own lesson demanded found the next one before it fired:
`CheckDescriptorCreationIsCloseOnExec` ran six separate `sregex_iterator` walks
of the whole tree, one per descriptor-creating form, at 5.3 s — *higher* than the
rule that had just gone red. One ordered alternation: 5.3 s → 1.09 s.

So the durable statement is not "split slow aggregate tests", it is: **a lint
whose cost is (patterns x files) is one busy machine away from the watchdog, and
the fix is to make the scan O(files), not to shard it.** The remaining
three-second rule, `CheckCoreIsNetworkFree`, is already a single pass; speeding
it further would need a hand-maintained substring pre-filter that goes blind when
somebody adds an alternative to the pattern, which is a worse trade than three
seconds.

### TD-2026-08-10-170 — a gated perf scenario whose fixture was missing did nothing and reported PASS. [RESOLVED 2026-08-10.]

    editor_moby_dick_workout   measured 7 allocations   baseline 138,599   verdict: PASS

Found by accident, comparing a scenario's measurement to its committed baseline
by hand. The fixture is gitignored and generated on demand; without it the
scenario body printed a line to stderr and `return`ed. The harness never learned,
so the empty iteration was recorded, compared, and graded — and a gate only fails
on a REGRESSION, so "did 0.005 % of the work" sailed through.

Three policies for "the fixture is not here" lived in one binary, which is the
condition `validation-traps.md` already warns about:

- `throw` (the git scenarios) — correct, the run dies.
- `EnsureFixtureOrSkip` + `--require-fixtures` — a quiet skip by default, a throw
  in CI. The flag defaults to off, so the quiet answer was the usual one.
- `std::cerr << "missing fixture"; return;` — **23 scenarios**, every one of them
  baseline-gated, none of them able to fail.

Now one policy. A scenario declares the skip (`ScenarioContext::SkipScenario`,
reached through the single `RequireFixture` guard); the harness stops after the
first iteration, discards the metrics, carries the reason across the isolation
fork, and PerfMain prints `SKIP` and **fails the run** for a baseline-gated
scenario, flag or no flag. `EnsureFixtureOrSkip` is deleted;
`--require-fixtures` now governs only the manifest-backed fixture-tree integrity
check, where a skip is genuinely right.

**Generalisable**: a one-sided gate cannot detect a measurement that collapsed.
Everything in this suite is checked for being too slow and nothing for being
impossibly fast, so any path that makes a scenario stop doing its work is
invisible by construction. The fixture guard was one such path; a scenario whose
`Measure` body silently early-returns is another, and nothing covers it yet.

### TD-2026-08-10-169 — `switch_and_idle` retains 10.4 % more heap than its baseline, and no code change explains it. [RESOLVED 2026-08-10 — the reading was the instrument, and the true value is a quarter of the baseline.]

    switch_and_idle  p50_net_heap_bytes  113,417 -> 125,164  (+10.4 %, tolerance +10 %)

The one gate still red after [168](#td-2026-08-10-168) that is not a duration.
It is 0.4 percentage points over its envelope, it reproduces exactly across runs
(125,164 then 125,380), and it fails identically with and without 168's process
warm-up, so it is neither noise nor an artifact of that fix.

What makes it worth an entry rather than a rebaseline: like 168, it fails at a
commit whose product code has not moved since the baseline was recorded. Either
it is the same "recorded in another regime" story with a second mechanism, or
something genuinely retains ~12 KB more per project switch. The two are
distinguishable — `MICROIDE_PERF_BIG_ALLOC_BYTES` over the phase, or an A/B of
the metric across the isolation commit — and neither has been done.

Do not rebaseline it before that: a retention gate that gets widened whenever it
trips is [139](#td-2026-08-06-139)'s exact failure, and this scenario's whole job
is catching a project switch that does not free what it opened.

**Resolved 2026-08-10, and the caution above was right for the wrong reason.**
[173](#td-2026-08-10-173) is the mechanism: `p50_net_heap_bytes` is a median over
a settling series, so its value depends on how many iterations were taken. At ten
iterations, on the same machine, the series is

```
1,166,649  35,589  29,935  30,414  30,414  36,607  30,414  30,414  29,934  30,414
```

— p50 **30,414**, deterministic to the byte across five of the ten samples. Not
125,164, and not the committed 113,417 either. So the answer to "does something
retain ~12 KB more per project switch" is no; what moved was the sample the
median landed on.

The gate is rebaselined **down**, 113,417 → 30,414, which is the opposite of the
widening this entry warned about: it tightens the scenario's whole point by 3.7x.
A retention regression that would have hidden inside the old envelope now trips
it.

**Generalisable**: the entry's two hypotheses were "a real regression" and
"recorded in another regime", and the actual answer was a third — the metric is
not a scalar, it is a function of the iteration count, and neither the baseline
nor the failure line said which one produced it. That is now recorded in the
baseline (`iterations`) and enforced by 173's guard.

### TD-2026-08-07-167 — nothing records that a scenario changed what it measures, so cross-release perf claims are computed from incomparable numbers. [RESOLVED 2026-08-10.]

Cutting v2.9.0 needed one number: how much faster is this release than v2.8.1.
The obvious way to get it — diff `p50_allocations` across the two tags'
`tests/perf/baselines/` — reported three regressions, and all three were
artifacts:

- `terminal_alt_screen_toggle` +172.6 % and `terminal_scroll_long_output`
  +112.3 %. Neither is a regression. `f38ef7fd` changed what those scenarios
  *do*: they used to scroll and toggle an empty buffer (the harness never spawns
  a real shell, so `yes`/`bash -lc` left a terminal holding one blank line), and
  they now feed 4,000 lines through the emulator. Different measurement, and the
  only way to know is to read the commit that moved the baseline.
- `git_sidebar_activate` +17.5 %, from `23ccb088` (per-scenario cold child
  process) and `b4bac8e0` (isolated app-root) — harness changes that move counts
  for anything that used to inherit warm state, in both directions.

A baseline file records the value and nothing about whether the value means the
same thing it did last release. The changelog for v2.9.0 works around this by
naming the two excluded scenarios in prose, which is exactly the kind of fact
that survives one release and is then lost.

**Fixed** with the cheap version the entry proposed: a `measurement_revision`
the author bumps, declared in `Scenario` (not in the JSON — a value that lives
only in a generated file does not survive a rebaseline) and written into every
baseline. Absent reads as 1, which is correct by construction for every baseline
that predates the field.

Two readers, and the second is the one the entry asked for:

- `CompareToBaseline` **refuses to gate** across a revision change. Every metric
  is still reported, none is enforced, and the run goes red on the mismatch
  itself with "rerecord", not on a phantom regression. That is what makes the
  bump self-enforcing: an author who changes a scenario and forgets gets a red
  run naming the field, rather than a green one hiding a rebaseline.
- `tools/perf-release-diff.py OLD_REF [NEW_REF]` differences two tags' committed
  baselines, excludes revision-mismatched scenarios from the totals and names
  each one. Run against the case that produced this entry it says exactly what
  the changelog had to say in prose:

  ```
  91 comparable scenarios, summed p50_allocations: 7,181,569 -> 2,922,208 (-59.3%)

  3 scenario(s) NOT COMPARABLE — excluded from the totals:
    git_sidebar_activate: measurement_revision 1 -> 2
    terminal_alt_screen_toggle: measurement_revision 1 -> 2
    terminal_scroll_long_output: measurement_revision 1 -> 2
  ```

The three scenarios this entry had to explain away are declared revision 2 and
rerecorded, so the v2.8.1 → v2.9.0 comparison is now correct **retroactively**
rather than only from here on.

**The `definition_hash` alternative was rejected**, and for a reason worth
keeping: a hash over the scenario body would fire on a comment, a rename, or a
refactor that changes nothing measurable, and a signal that fires on
no-op edits is one people learn to clear without reading. The declaration is a
judgement — "this changes what is measured" — and only a human can make it.

Related, and handled: **wall-clock cannot be compared across v2.8.1 at all** —
those baselines predate `p50_cpu_calibration_ns`, so there is no way to normalise
for the machine clock state. The tool applies the same refusal there (91
scenarios reported as having no comparable duration metric, with the reason)
rather than differencing raw milliseconds. That one ages out on its own as tags
accumulate.

### TD-2026-08-07-161 — the whole baseline set is stale in three independent ways. RESOLVED 2026-08-07: both halves rerecorded; the timing/resident half on an idle runner.

Nothing in `tests/perf/baselines/` describes what `microide_perf` now measures,
and the gaps are large enough that most allocation gates would pass a complete
regression — [147](#td-2026-08-06-147)'s exact defect, across the suite rather
than in six scenarios.

Three separate causes, all from 2026-08-07:

1. **[159](#td-2026-08-06-159)'s nine fixes.** Every editor-scroll, compare, diff
   and git scenario moved, several by 45-78 %. `editor_surround_multi_caret` went
   76,456 -> 933 after [157](#td-2026-08-06-157) — a gate 82x loose.
2. **[152](#td-2026-08-06-152)'s per-scenario child process.** Every scenario now
   starts cold, so wall p95/max and `p50_net_heap_bytes` move on anything that
   used to inherit a warm allocator and page cache from the scenario before it.
   `typing_small_file` p95_allocations read 2,142 in-suite and 3,417 isolated.
   This one is not a code move at all: it is the number becoming true.
3. **The finder's separator split**, which changes what `file_finder_*` rank.

**Not taken in this session** because the runner was busy — load average 13-20
from three unrelated processes at ~95 % CPU each — and a baseline recorded on a
loaded box records the load. On an idle perf-runner-v1:

```
tools/run-checks.sh tests   # confirm green first
# DEFAULT iteration count, bare (no xvfb), nothing else running:
./build/microide-perf-make/microide/microide_perf --update-baseline \
    --reference-runner=perf-runner-v1
# then re-gate against what it just wrote — a rebaseline is not evidence of itself:
./build/microide-perf-make/microide/microide_perf --reference-runner=perf-runner-v1
```

Certify with `harness.cpu_calibration_ns` (not load average) and check the
verdict lines for clock-drift warnings before committing the result. Expect the
resident numbers to DROP to their solo values and the allocation counts to move
by the amounts 152 predicted; both are the point of that change, not a regression
to investigate.

**2026-08-07: the deterministic half is done, on a busy box, deliberately.**

The all-or-nothing `--update-baseline` was the reason nothing had been rerecorded:
a baseline's two halves have different requirements on the machine, and blocking
both on the stricter one left the gates that never needed a quiet box stale for
days. `--update-baseline=deterministic` rewrites only `p50/p95/max_allocations`,
`p50_net_heap_bytes` and the per-phase allocation counts, carrying every
timing/resident number over from the committed record unchanged (see
`MergeDeterministicMetrics`). It refuses to mint a baseline that does not exist
yet — with nothing to merge into, it would create a timing gate out of exactly the
measurement it exists to avoid taking.

The suite was rerecorded that way. 79 of 85 gated scenarios moved; the largest
were `branch_review_presentation_markers` -99.6 %, `editor_surround_multi_caret`
-98.8 %, `git_sidebar_refresh_many_untracked` -81.7 %, `diff_open_1000_file_changes`
-78.0 %. Several small scenarios moved UP (`cold_startup_no_project` 101 -> 801,
`menu_hover_switch` 54 -> 314): that is cause 2, the number becoming true, and it
was confirmed by running each one both isolated and with `--no-isolate`.

**2026-08-07, later: the timing and resident half is done too, on an idle box.**

The prediction above held. Gated bare on an idle perf-runner-v1 at the default
iteration count, the pre-rebaseline suite read **99 PASS / 1 FAIL / 3 advisory**:
`typing_large_file` failed `mean_rss_growth_bytes` at 103,310 bytes against 79,644
(+29.7 % against +25 % allowed), and `editor_snippet_expand` passed at 91.6 % of
the same envelope. Both are resident gates whose baselines predate per-scenario
process isolation, exactly as called.

**No allocation gate was anywhere near its limit in that run** — zero deterministic
headroom notices. That is what separates "the baseline is stale" from "the code
regressed", and it is the check to repeat before believing any future rebaseline:
rerecording a suite whose deterministic gates are tight would be rebaselining a
regression away.

The full `--update-baseline` was then run under the same conditions (clock steady
at ~0.94x the previous baselines' calibration, apart from the soak/idle scenarios
which lose the boost by construction) and re-gated against what it wrote:
**100 PASS / 0 FAIL, every gated metric below 75 % of its envelope.** Tolerances
were diffed field by field afterwards and none moved — `--update-baseline` is
documented to reset hand-edited ones, so that is a check, not an assumption.

The deterministic metrics moved slightly in that rewrite (±4–8 on the git/diff
scenarios, +91 on `linter_on_save`). That is **not** harness nondeterminism: the
deterministic half was recorded at `0d52373f`, six code commits before HEAD,
including `b4bac8e0` (the [165](#td-2026-08-07-165) app-root isolation fix, which
by design changes what state the shell reads). `git_sidebar_activate` reads 733 in
6/6 standalone runs and in both full-suite runs; the committed 729 was simply older
code.

**The +80 menu anomaly did not recur.** Two full-suite isolated runs, an hour
apart, had disagreed by exactly +80 allocations on both `menu_hover_switch`
(234 vs 314) and `menu_popup_hover_rows` (254 vs 334). Across the two full-suite
runs on the idle box they agree exactly — 54 and 74, matching their committed
baselines and each other. Both scenarios fell to those values in `384f238d`, which
stopped the tab strip and the sidebar rail rebuilding per frame, so the surface
that carried the disagreement is substantially less work now. Left recorded rather
than deleted: a full-suite run that disagrees with a standalone one is a lead, not
noise (memory: perf-scenario-context-dependence). Diff the `perf_counters` block of
the two runs if it returns.


### TD-2026-08-06-159 — 63 of the suite's 70 interactive phases have never been read through the allocation tracer. OPEN — and every pass so far has found something.

#### 2026-08-12 pass: `switch_and_idle.switch_and_settle`, and the answer is the session WRITER

Traced because the scenario is one of the four whose `p50_net_heap_bytes` is red
([191](#td-2026-08-12-191)), so the question was "what does a project switch newly
hold onto". The phase does **18,004 allocations over 2 iterations from 1,586
distinct sites**, and every one of the top twelve resolves into the same stack:

```
AppendLe<std::uint32_t>(std::vector<std::byte>*, std::uint32_t)
  <- persistence::PrimitiveWriter::WriteString(std::string_view)
  <- persistence::PrimitiveWriter::WritePath(const std::filesystem::path&)
  <- workspace::EncodeProjectSessionRecord(const PersistedProjectSessionState&, ...)
```

Eight sites at 126 allocations / 588 bytes each, four more at 120 allocations /
2-3.7 KB — i.e. ~4.7 bytes per allocation, which is not geometric growth of one
buffer but many buffers each grown from empty a few bytes at a time. So switching
project **encodes the session record on the switch path**, and the encoder's
output buffer is unreserved.

Two things to check when this is picked up, in this order:

1. Whether the encode belongs on the switch path at all. The state writes are
   queued off-thread, but the ENCODE runs where the queue is filled.
2. Whether `PrimitiveWriter` should take a reserved buffer. A record whose size
   is knowable up front (path lengths are all in hand) turns ~1,000 tiny
   allocations into one.

Also worth noting for whoever reads a tracer table next: 1,586 distinct sites in
one phase is close enough to the bucket ceiling to check the drop warning before
trusting the ranking ([178](#td-2026-08-10-178)). This run printed none.

#### 2026-08-11 pass: the two biggest remaining phases, and a rule about the tracer's own output

Read `multi_project.switch_cycles` (17,390, the largest unread interactive phase)
and `git.refresh_dispatch` (52,001, the largest of all). Four fixes; the hit rate
stays at 100 %.

| phase | before → after | what it was |
| --- | ---: | --- |
| `multi_project.switch_cycles` | 19,522 → 11,254 (-42 %) | three separate per-switch rebuilds of things that had not changed |
| `git.refresh_dispatch` (many_untracked) | 52,357 → 45,051 (-14 %) | `lexically_normal()` per changed file on git's already-normal output |
| `git.refresh_dispatch` (large_repo) | 23,920 → 21,111 (-12 %) | the same call |

**A project switch rebuilt three tables that a switch cannot change.** All three
hang off `RefreshPluginSurfacesForReactivation`, and all three are the same
mistake — "the plugin host was torn down, so re-derive everything the plugin host
could contribute to", applied to state the plugin host does not own:

  - `WorkspaceLanguageContract::Refresh` rebuilt the ~40-language default table
    from scratch (~5,000 allocations, 12 % of the phase) to then layer nothing on
    it. It now skips the rebuild when there is no contribution and no user
    override, which also skips the **revision bump** — and that bump drops the
    shared editor-view cache and re-applies preferences to every open tab
    ([110](#td-2026-08-03-110)). A revision that advances when nothing changed is
    not free; it is the price of everything downstream keyed on it.
  - `RefreshAvailableColorschemeNames` re-listed the themes directory (~4,900
    allocations): resolve nine candidate paths, stat each, then a
    `platform::ListDirectory` walk that normalizes a path and stats every entry,
    then sorts them — to produce a handful of stems, for a picker whose only
    switch-sensitive input is the in-memory plugin theme list.
    `render::ThemeNameCatalog` memoizes it against the directory's own mtime.
  - `DirectoryTree::IsExpanded` built its lookup key per candidate entry with
    `absolute()` (a getcwd **syscall**) + `lexically_normal()` + `generic_string()`,
    on paths the tree walk had just built from its own absolute normal root.

**Read the instrument, again — and the frames this time.** `addr2line` without
`-i` names the function *containing* the return address, which under LTO is
routinely a different function than the one that ran: one trace attributed
allocations to `RenderMergeScrollbars` calling `EditorViewRenderer::Render`,
which cannot happen. Two rules follow, both of which cost time here:
resolve with `-i` and read the inline chain, and **filter frames by module** —
a backtrace interleaves `libstdc++` and the binary, and feeding a libstdc++
offset to `addr2line -e <binary>` produces a confident, wrong answer rather than
an error.

**Read, inherent** (recorded so the next pass does not re-read them):

- `toggle_line_comment.1000_lines` — 2.35 allocations per toggled line, and two
  of them are the floor of the current representation: one owned `std::string`
  for the line's new text, one for the undo entry's before-image. The entry's own
  earlier note ("89 allocations per line, only ~8 accounted for") is long
  obsolete. The exit is [182](#td-2026-08-11-182), a blob-backed `HistoryEntry`.
- `git.refresh_dispatch`'s remainder is one shape: every changed file's path is
  materialized as a `std::filesystem::path` four times over on its way from the
  porcelain parser to the sidebar presentation, and libstdc++'s `path` is a
  string **plus** a component list. That is [183](#td-2026-08-11-183).

**Next unread**, re-ranked from the committed phase baselines:
`diff.open_large_patch` / `diff.open_large_compare` (25,709 each),
`diff.open_first_changed_file` (24,317), `commit.open_staged_sidebar` (20,749),
`sort_lines_ascending.10000_lines` (20,015), `diff.next_hunk_burst` (12,516).

#### 2026-08-10 pass 2: the four named-next phases, and a fifth shape

All four phases this entry named as "next by committed `p50_allocations`, unread"
were read. Two fixes, two read/inherent, and the hit rate stays at 100 %.

| phase | scenario total, before → after | what it was |
| --- | ---: | --- |
| `move_line_down.multi_caret_burst` | 7,865 → 3,553 | `PieceTree::ExtractLineRange` allocated its own walk stack per call |
| `compare_large.scroll_burst` | 6,167 → 2,667 | the welcome surface rebuilt per frame AND per mouse event |
| `merge_model.build_interleaved` | — | read, inherent |
| `status_registry.apply_update` | — | read, inherent |

**`move_line_down` — a fifth shape: a walk that allocates its own stack.**
`ExtractLineRange`'s pruned in-order treap walk built a local
`std::vector<Frame>`, so every `SliceLines` paid that vector's geometric growth
(~4 allocations) before copying a single line. `CopyRange` sitting twenty lines
away already knew not to — its `copy_stack_` is a member for exactly this reason
([133](#td-2026-08-05-133)) — and the two frames are the same struct. With 32
carets × 12 ops, ten of the phase's top twelve sites resolved into that one
local. It carries: `mid_file_edit` -32 %, `first_line_edit` -38 %,
`toggle_comment_large_selection` -3 %. **Look for this shape anywhere a tree walk
is written as an explicit stack** — it reads as free and is four allocations.

**`compare_large.scroll_burst` — a surface rebuilt to hit-test it.** 29 % of the
phase was `RenderViewModelBuilder::BuildWelcomeView`, called twice per frame:
once from the render path (the focused group holds a compare tab, so the editor
pane draws the placeholder) and once from `CursorKindForPosition` →
`ProbeWelcomeSurface`, which built the whole model *and* its layout to answer
"pointer or I-beam?" per mouse event. Nothing in it varies per frame — the
shortcut rows and chords come from the static command-spec table, the headings
are literals, the recents are already exists-filtered behind an MRU-revision
cache — so a rebuild materialized ~30 strings to reproduce the previous frame
byte for byte. Now a builder-owned memo keyed on (MRU instance, MRU revision,
project root), same shape as `StickyScrollLines`.

Two traps that came out of it, both worth generalising:

- **A revision is not a key.** The first version keyed on `RecentsService`'s
  revision alone. It starts at 0 on every instance, so a test constructing one
  service per case was handed the previous case's model — caught by an existing
  test, and the reason `RecentsService` now carries a process-unique
  `instance_id()`. Any memo keyed on a per-object counter needs the object's
  identity beside it, and an address is not that (a stack object can land on a
  freed one's address).
- **A borrowed model must be copied out before acting on it.** The welcome click
  handler passes a listed path to `OpenProjectTab`, which changes the root the
  memo is keyed on. Returning a reference turned a copy into a use-after-invalidate
  until the path was copied first.

**Read, inherent** (recorded so the next pass does not re-read them):

- `merge_model.build_interleaved` — 8,673 allocations, and 43 % of them are
  `util::SplitLines` over the three inputs (1,244 lines each, one owned
  `std::string` per line). The rest is ~8 vectors per hunk over 139 hunks:
  `SliceBaseLines` plus two `ApplySideChangesToSlice` results, each of which
  becomes a `MergeHunk` member. All of it is the model's own storage — `MergeModel`
  outlives the input strings, so the lines must be owned. Cheaper only with a
  shared-blob + views representation of `MergeModel`, which every consumer of
  `base_lines`/`incoming_lines`/`current_lines` would have to move with.
- `status_registry.apply_update` — 16,002 allocations over 2 iterations from
  **three** sites, and 160,000 lookups contribute none of them. It is one
  `rebuild_index()`: 4,000 key-string copies + 4,000 hash nodes + 1 bucket array
  per iteration. That is `unordered_map<std::string, size_t>`'s floor, and the
  keys duplicate strings the order vector already owns. A `string_view` key is
  the obvious exit and is not safe here (a vector reallocation moves SSO strings'
  data); a hash-keyed flat table needs collision handling the current
  rebuild-and-retry cannot express. The rebuild runs once per structural change
  on a synthetic 4,000-item registry no real plugin surface reaches.

**Next unread**, re-ranked from the committed phase baselines rather than the
stale table below: `multi_project.switch_cycles` (17,390),
`compare_selection.scroll_burst` (~13,600), `first_line_edit`/`mid_file_edit`
enter_backspace_burst (now ~7,500-8,200 after this pass), `diff.next_hunk_burst`.

Reading these also turned up [181](#td-2026-08-10-181): twelve phase allocation
gates read differently at `--iterations=2` than at 10, and three of them are
named for an operation they never run. **Check a phase's iteration sensitivity
before trusting a number the tracer workflow's own `--iterations=2` produced.**

#### 2026-08-10 pass: two phases read, and the instrument was broken

Read `switch_and_idle.switch_and_settle` and
`editor_scroll_only_no_content_bump.scroll_frame`. Both had something, keeping
this entry's hit rate at 100 %.

**Read the instrument first.** The tracer's top-sites table was a ranking of the
sites that appeared before its 1024-bucket table filled, not of the sites that
allocated most — the real #1 was missing from it entirely. That is
[178](#td-2026-08-10-178), fixed before either phase was read. **Any conclusion
drawn from a trace taken before that fix should be re-taken.**

| phase | scenario total | what it was |
| --- | ---: | --- |
| `switch_and_idle.switch_and_settle` | 28,183.5 -> 25,567 | the project tab strip building a `reserve()`d vector per tab per frame to ask it `.empty()`; a catalog path returned by value per tab per frame; a `lexically_normal()` on it; and a session save deep-copying every breakpoint to read each once |
| `editor_scroll_only_no_content_bump.scroll_frame` | 7,926 -> 7,122 | a `TerminalPanelService` heap-copied into each of nine callbacks, per mouse event ([177](#td-2026-08-10-177)) |

The second one was carried by six other scenarios (`scroll_large_file` -55.5 %,
`editor_indent_guides_paint` -7.6 %, `editor_sticky_scroll_scroll` -6.3 %,
`terminal_scroll_long_output` -6.5 %, `editor_fold_viewport_refresh` -6.2 %,
`editor_render_whitespace_paint` -6.2 %), which is this entry's recurring pattern:
a per-frame or per-event site found in one phase is almost never confined to it.

**A fourth shape for the list**, alongside "work computed before the guard that
discards it", "a value copied N times", and "an argument built eagerly for a callee
that usually does not look at it": **a measured phase whose body is a wall-clock
wait**. `linter_on_save` was gating on the iteration count of a poll loop that had
never once succeeded — see [179](#td-2026-08-10-179), and
[180](#td-2026-08-10-180) for the two `WaitFor*` helpers still unaudited.

**Read, inherent** (recorded so the next pass does not re-read it):

- `sort_lines_ascending.10000_lines` — 20,000 allocations over 10,000 lines, in
  exactly two flat sites of 10,000 each, both `PieceTree::SliceLines`. One is
  `SortLines` reading the range to sort it, which becomes the edit's
  `after_lines`; the other is `BuildLineHistoryEntry` re-reading the same range
  for `before_lines`. It looks like a duplicate read and is not: `SortLines`
  sorts **in place**, so by the time the history entry is built the vector it
  holds is no longer in the original order, and `before_lines` must be. Two owned
  copies of every line is the floor for "undo stores whole lines" — the same
  conclusion `toggle_line_comment.1000_lines` reached at 2 per line. Cheaper only
  with a permutation-shaped `HistoryEntry`, which is a general-structure change
  for one operation.

**Next by committed `p50_allocations`, unread**: `merge_model.build_interleaved`
(8,673 in 0.6 ms), `status_registry.apply_update` (8,001),
`move_line_down.multi_caret_burst` (7,547), `compare_large.scroll_burst` (6,457).
The three `git.refresh_dispatch` instances are already recorded as read/inherent
above.


[149](#td-2026-08-06-149) ended with an instruction — "generalise the sweep: the
instrument is cheap now, and no interactive scenario other than the drag and this
one has been read through it." The 2026-08-06 sweep did **7 of 70**. Every one of
the seven had something, and six became fixes worth 15–99.95 % of their phase.

That hit rate is the entry. A phase nobody has aimed the tracer at is not a clean
phase; it is an unread one, and the six fixes so far were all the same two shapes
(work computed before the guard that discards it; a value copied N times where the
algorithm needs one or two) rather than anything subtle.

**The unread worklist**, ranked by committed `p50_allocations`. Note the baselines
below are pre-sweep for the two the shared fixes already moved
(`editor_sticky_scroll_scroll.fast_scroll_frame` is now 16,306,
`editor_render_whitespace_paint.scroll_overlay_frame` 14,270) and the whole table
is stale until the pending rebaseline lands:

| phase | p50 allocations | p50 wall |
| --- | ---: | ---: |
| `diff.open_first_changed_file` | 112,175 | 17 ms |
| `commit.open_staged_sidebar` | 91,135 | 16 ms |
| `toggle_line_comment.1000_lines` | 89,044 | 22 ms |
| `diff.open_large_compare` | 51,039 | 14 ms |
| `diff.open_large_patch` | 50,972 | 12 ms |
| `diff.next_hunk_burst` | 24,597 | 44 ms |
| `editor_sticky_scroll_scroll.fast_scroll_frame` | 22,506 | 89 ms |
| `multi_project.switch_cycles` | 19,855 | 17 ms |
| `editor_render_whitespace_paint.scroll_overlay_frame` | 19,230 | 85 ms |
| `compare_selection.scroll_burst` | 17,178 | 94 ms |
| `editor_indent_guides_paint.scroll_paint_frame` | 17,082 | 67 ms |
| `first_line_edit.enter_backspace_burst` | 13,816 | 23 ms |
| `mid_file_edit.enter_backspace_burst` | 11,633 | 24 ms |
| `merge_interleaved.scroll_burst` | 8,928 | 108 ms |

**The one lead this entry named is closed, and it was stale when written.**
`toggle_line_comment.1000_lines` measures **32,869 allocations** against a
committed phase baseline of 32,869.5 — about **2 per line** on a 1,000-line
selection (one for the undo entry's `before_lines` string, one for the rewritten
line), which is the floor for an operation that must record what it replaced. The
89,044 below is a whole-SCENARIO number from before the phase gates were armed
([153](#td-2026-08-06-153)), so it counted the fixture open too; dividing it by
the line count was never meaningful. Traced 2026-08-10 and dropped from the
worklist. The rest of the table is likewise pre-rebaseline and should be re-read
from the committed phase baselines before anything is ranked by it.

The original text follows. `toggle_line_comment.1000_lines` is **89 allocations
per line** on a 1,000-line selection. Reading `ShapingActions::ToggleLineComment`
accounts for about five of them — it walks the range twice through
`TextBuffer::LineRef`, and the uncomment branch takes two `substr`s per line — and
the before/install/after of the replace is three more. The other ~80 per line are
unexplained, which is the exact signature that preceded the last four finds. That
number is *after* the 2026-08-06 line-op fix took 15 % off it.

**Method** (see `dev-docs/performance/perf-harness.md` § Finding *where* a phase
allocates):

```bash
MICROIDE_PERF_ALLOC_TRACE=1:1000000 MICROIDE_PERF_ALLOC_TRACE_PHASE=<phase> \
  ./build/microide-perf-make/microide/microide_perf --scenarios=<scenario> --iterations=2
addr2line -e ./build/microide-perf-make/microide/microide_perf -f -C -p -i <addr>
```

The `-i` matters — everything interesting is inlined, and without it every frame
resolves to the outermost non-inlined caller. Divide each site's count by
(iterations × inner-loop count) before believing it. **Flat, roughly-equal sites
are the tell**: N sites of identical size usually means one value materialised N
times, so count them and subtract the ones the algorithm actually needs, rather
than looking for a cheaper algorithm.

**Definition of done**, so this does not become an open-ended chore: every phase in
the table above read once, each read either producing a fix or a one-line note in
this entry saying what the allocations are and why they are irreducible. A phase
recorded as "read, inherent" is a real result — `git.refresh_dispatch` was one
(flat sites, ~3 allocations per sidebar entry, no defect) — and it stops the next
pass re-reading it.

**Do not read these**: the pure-unit scenarios (`user_config_record_decode`,
`dap_protocol_encode_decode`, …). Their whole run is the work, the total already
is the phase, and they are micro-benchmarks of algorithms that were chosen
deliberately, not interactive paths anyone waits on.

#### 2026-08-07 pass: the whole table above is now read

Every phase in the worklist was read. Nine fixes landed; the numbers below are
scenario totals against the committed baseline (which the fixes have now made
stale everywhere — the suite needs a rebaseline, see
[153](#td-2026-08-06-153)/[152](#td-2026-08-06-152) for the runner conditions).

| phase | scenario total, baseline → now | what it was |
| --- | ---: | --- |
| `toggle_line_comment.1000_lines` | 89,588 → 33,586 | a same-count line replacement written line by line, so an N-line edit paid N piece-tree splices; plus `operator[]` twice per line |
| `diff.open_first_changed_file` | 112,519 → 24,710 | sort key re-derived per comparison; per-entry `lexically_normal()` on already-normalized paths; two whole-state deep copies per refresh |
| `commit.open_staged_sidebar` | 91,135 → 20,999 | the same three |
| `diff.open_large_compare` / `diff.open_large_patch` | 75,789 → 51,067 / 56,922 → 32,876 | both sides materialized whole to feed a 64-line signature scan |
| `compare_selection.scroll_burst` | 28,515 → 13,642 | a per-row visual-column table built for a line whose columns already are its bytes |
| `merge_interleaved.scroll_burst` | 9,304 → 4,552 | same (via the shared diagnostics underline path) |
| `editor_indent_guides_paint.scroll_paint_frame` | 21,224 → 10,003 | the same table, plus a plugin hover query dispatched per frame with no provider registered |
| `editor_render_whitespace_paint.scroll_overlay_frame` | 22,694 → 11,447 | the same table, plus `operator[]` per visible row in the view-model builder |
| `editor_sticky_scroll_scroll.fast_scroll_frame` | 27,101 → 14,438 | carried by the shared fixes |
| `first_line_edit` / `mid_file_edit.enter_backspace_burst` | 14,103 → 13,241 / 11,938 → 11,076 | a path key normalized per keystroke for an owner with nothing stored |

Two more moved without being on the list, from the same shared fixes:
`editor_scroll_only_no_content_bump` 16,515 → 8,827 and
`editor_fold_viewport_refresh` 25,955 → 13,640.

**Read, inherent** (no fix; recorded so the next pass does not re-read them):

- `diff.next_hunk_burst` — the compare tokenizer filling its per-row token cache,
  256 rows a frame. Half of it is `HighlightLine`'s own returned token vector,
  which is moved into the cache, and half is the second vector an *identical*
  unchanged row needs because the left and right caches both own their tokens.
  Removing that half means letting the right row's tokens alias the left's, which
  changes the render read sites; filed below rather than done.
- `multi_project.switch_cycles` — directory traversal on the root rebuild:
  `platform::ListDirectory` and `SymlinkLoopGuard::TryEnter` per directory, flat
  and equal, plus `DirectoryTree::NormalizePathKey` (which is
  `std::filesystem::absolute` + `lexically_normal` + `generic_string`, ~226 bytes
  a call). The traversal is the work a project switch has to do.
- `merge_interleaved.scroll_burst`'s residue after the fix — small, evenly spread
  per-frame chrome (status bar model, bottom-panel tab strip, ordered sidebar
  views), no site above 17 %.

**Still open, found by this pass and deliberately not taken** — both closed
2026-08-10, and neither needed the guard the entry was dreading:

- `LspService::ClearLspCodeLensesForFile` builds a `FileUriForPath` per keystroke
  before discovering there is no lens to clear. The entry proposed a guard that
  has to reason about which in-flight responses the generation bump is protecting
  (`NextOverlayGeneration` *creates* the entry, so the obvious "nothing tracked
  yet" test is one-shot). **The URI was the cost, not the call.**
  `FileUriForPath` ran `lexically_normal()` — ~12 allocations, a fresh path plus a
  component list holding a string per component — plus `generic_string()`, on a
  path the editor normalized when it opened the file, three times per keystroke
  (inlay generation, code-lens generation, open-document resolution). Guarding it
  with `util::PathTextNeedsNormalizing`, the allocation-free scan, leaves the
  common case at the one allocation the result needs and makes the clear paths
  free, so the in-flight-response reasoning never has to be done.
- The compare token cache's duplicate vector, above. One flag beside the two
  caches marks the rows whose right tokens ARE their left tokens, so the
  predicate stays where the tokenizer already computes it (it needs both lines'
  text and both syntax states) instead of being re-derived per visible row per
  frame in a render TU, and the two read sites are pointer assignments:
  `diff.next_hunk_burst` 23,493 → 12,516 (−46.7 %).
- `internal::HasGitMarker` still shows up on every frame-pumping phase; that is
  [158](#td-2026-08-06-158), unchanged.

#### 2026-08-10 pass: the detection nobody asked for

Pointing the tracer at `first_line_edit.enter_backspace_burst` — one of the two
leads above — found something larger sitting underneath it. **25 % of the phase**
was `SignatureDetectHead` materialising up to 64 owned strings before every
`DetectDefinitionId` call, from both `TextViewport::language_id()` and
`EnsureInitialHighlightState()`, once per content revision, i.e. per keystroke.
The signature scan that reads them runs only when a filename match is
**ambiguous**, which for an ordinary `.cpp`/`.py`/`.rs` path is never. So the
head was built and thrown away, every keystroke, ~65 allocations a time.

`DetectDefinitionId` now takes the `LineSpan` and reads a bounded head line
through `LineWindow` into one reused scratch buffer at the point the scan needs
it — so the ambiguous case allocates nothing either, rather than 65. Combined
with the `FileUriForPath` guard:

    first_line_edit_latency_large_file    13,189 ->  8,909   -32.5 %
    editor_typing_minified_line            3,164 ->  2,196   -30.6 %
    mid_file_edit_latency_large_file      11,024 ->  8,257   -25.1 %
    typing_small_file                        261 ->    224   -14.2 %
    typing_large_file                        270 ->    233   -13.9 %
    editor_scroll_only_no_content_bump     8,546 ->  7,926    -7.3 %
    editor_indent_guides_paint             9,200 ->  8,734    -5.1 %
    editor_fold_viewport_refresh          12,984 -> 12,390    -4.6 %
    editor_sticky_scroll_scroll           13,342 -> 12,723    -4.6 %
    editor_render_whitespace_paint        10,938 -> 10,450    -4.5 %

**A third shape for the list.** The two the entry names are "work computed before
the guard that discards it" and "a value copied N times". This is neither: it is
**an argument built eagerly for a callee that usually does not look at it**. Grep
for a function that materialises a container to pass as a parameter, then read
the callee for an early return that precedes the first use of it —
`resolve_from_matches` returns `matches.front()` before touching `lines` whenever
the filename match is unambiguous.

**And the same shape once more, from the same trace.**
`BreakpointStore::ShiftForAppliedEdit` runs on every applied edit and returns
immediately when the file has no breakpoints — the common case — but built its
lookup key with `PathKey`, i.e. `lexically_normal().generic_string()`: ~13
allocations to discover there was nothing to shift, ~720 per edit-burst phase.
`by_path_` already hashes transparently, so a probe wants a view and not a
string, and on an already-normal path the key IS the path's own text. That is
the third independent site in this pass whose fix was
`util::PathTextNeedsNormalizing` — `FileUriForPath`, `RelativePathLabel`
([the 2026-08-07 tab-strip pass](#td-2026-08-06-159)) and now this one. **Grep
`lexically_normal` before the next pass**: every remaining call on a per-keystroke
or per-frame path is a candidate, and the guard is allocation-free.

    first_line_edit_latency_large_file     8,909 -> 8,333   -6.5 %
    mid_file_edit_latency_large_file       8,256 -> 7,681   -7.0 %

**What is NOT covered by this pass**: the entry's title counts 63 unread
*phases*, and the table was the ranked subset. The phases below
`merge_interleaved.scroll_burst`'s 8,928 have never been read. The hit rate on
the ranked ones was 9 fixes in 14 phases, so the tail is worth a pass, but a
smaller one — start from a `--report-json` run of the whole suite sorted by
phase `p50_allocations` after the pending rebaseline, not from this table.

#### 2026-08-07 tail pass: the top of the unread remainder

Taken from a `--report-json` of the whole suite (116 phases) after the
deterministic rebaseline, ranked by `p50_allocations`, skipping the pure-unit
scenarios and the phases the pass above already read. Four fixes in six phases.

| phase | scenario total, → now | what it was |
| --- | ---: | --- |
| `settings_overlay.rebuild` | 417,644 → 6,153 | rows cleared and re-pushed, so eight strings per row were freed and reallocated per keystroke in the Settings search box; three `unordered_map`s built per pass (one node per element, 500 of them); `"plugin:" + plugin_id` per row |
| `breakpoints_model.rebuild` | 110,617 → 2,706 | `SnapshotAll` deep-copying every file's breakpoint vector (88 KB a pass) to walk it once; then the same clear-and-re-push row churn, four strings and a `filesystem::path` per row |
| `status_registry.apply_update` | 172,005 → 16,007 | **the scenario, not the product** — see below |
| `snippet.many_mirror_shift` | 23,430 → 3,634 | two `unordered_map`s per keystroke inside a snippet: one node per mirror, on a construct whose point is having many mirrors |

The recurring shape this time was **a model rebuilt from scratch when it could be
overwritten**. Three of the four were the same two lines: `rows_.clear()` frees
every string every row owns, and the next rebuild — which for a search box is the
next keystroke — allocates them all again. Overwriting in place and truncating
only the tail is the fix, and the thing to test with it is the SHRINKING rebuild,
because reused storage is exactly how a stale field survives.

**`plugin_status_item_update` was a gate on its own scaffolding.** 95 % of that
phase's allocations were the measured loop composing
`"plugin.status.item_" + std::to_string(...)` per call — two allocations per
iteration against a lookup that makes none. A real regression in
`ApplyStatusItemUpdate` could not have moved the number. The input is built
outside the measured window now. Worth checking for elsewhere: a `Measure()` body
that builds its own input is measuring `operator+`.

**Read, inherent** (no fix; recorded so the next pass does not re-read them):

- `sort_lines_ascending.10000_lines` — two symmetric `SliceLines` walks of the
  same range, one to sort and one for the undo before-image, at two allocations
  per line. The sort needs the lines materialised and the entry needs the pre-edit
  image; they cannot share one copy without a shared-string line representation.
- `value_tree.build_expand` — 88 % is `DebugValueTree::nodes_`, one 184-byte hash
  node per tree node. Filed as [162](#td-2026-08-07-162) rather than done. The row
  list is NOT the cost here: `value_tree.rebuild` already reads 0 allocations, and
  applying the in-place row reuse that fixed the other three moved this scenario
  by 3 allocations, so it was reverted rather than carried as complexity.

#### The next pass: grep first, then confirm with the tracer

**Do [163](#td-2026-08-07-163) before this one.** It measures how much of each
phase is the scenario's own scaffolding, and the worklist below is ranked by
phase allocation count — a phase that is mostly `operator+` in its `Measure()`
body sorts high for the wrong reason and would be read for nothing.

Then change the method. Both previous passes read phases in ranked order, which
works but only reaches what the suite already measures: **116 phases across 103
scenarios, out of an application with far more rebuild paths than that.** A path
no scenario measures is not a path that is fast; it is an unmeasured one. Both
shapes the passes found are greppable, so grep `src/` first and use the tracer to
confirm and size what the grep turns up:

- **Clear-and-repush.** A `<container>_.clear()` (or `.assign(...)`) followed by
  `push_back`/`emplace_back` in a rebuild whose element type owns a `std::string`,
  `std::filesystem::path`, or a nested vector. Three of this pass's four fixes were
  exactly this, and the recipe is in [159](#td-2026-08-06-159)'s tail-pass section
  above. Rank the hits by how often the rebuild runs: a per-keystroke rebuild
  (any filter/search surface) is worth much more than a per-open one.
- **A hash map keyed by something dense.** `unordered_map<std::size_t, …>` /
  `unordered_map<std::uint32_t, …>` whose keys come from a counter or from
  positions in a vector — that is a hash table over its own array indices.
  `unordered_map<std::string, …>` built fresh inside a function that then walks it
  once is the other half (a sorted vector plus `lower_bound` allocates twice, not
  once per element).

**Still unread, if the ranked read is preferred anyway** — everything below
`snippet.many_mirror_shift`'s 23,430 in the 2026-08-07 ranking. The interactive
ones, excluding phases already recorded as read or inherent:

| phase | p50 allocations |
| --- | ---: |
| `switch_and_idle.switch_and_settle` | 16,034 |
| `merge_model.build_interleaved` | 12,297 |
| `multi_tab.open_tabs` | 12,084 |
| `settings.apply_contract_family_all_tabs` | 10,752 |
| `settings.apply_cheap_family_all_tabs` | 10,564 |
| `compare_selection.open_to_first_paint` | 7,659 |
| `compare_large.scroll_burst` | 7,017 |
| `multi_tab.cycle_tabs` | 6,700 |
| `value_tree.paging` | 5,094 |
| `file_finder_type_query.type_and_rank` | 4,742 |
| `linter.wait_diagnostics` | 4,676 |
| `external.refresh_open_diff` | 4,457 |
| `merge.accept_interleaved_burst` | 4,320 |
| `terminal.feed_output` | 4,260 |

`settings.apply_contract_family_all_tabs` is the one to start with: it is adjacent
to the Settings work this pass already did, and it is the scenario gating
TD-2026-08-03-110's shared `LanguageContractView`, so anything found there has an
existing guard to check it against.

#### 2026-08-07 grep-first pass: the fresh-hash-set shape

The grep for "a hash container built inside a function, walked once, thrown away"
turned up `assist_merge::RankedUnion`, and the tracer sized it:

    assist.ranked_union   240,086 -> 126  (-99.95 %)

`RankedUnion` merges the LSP and plugin result lists for completion, code actions
and references — per keystroke while a completion popup is open. Past a small
threshold it de-duplicates through a `std::unordered_set<Key>`, which is **one
heap node per distinct key**: merging two 6,000-item lists was 6,002 allocations
per merge, and the merge itself is the entire function. The keys are short enough
to sit in the string's SSO buffer, so every one of those allocations was the
container's node, not the data.

The replacement is `util::FlatDedupSet` (`src/util/FlatDedupSet.h`): open
addressing over two flat arrays, the slot table sized once from the caller's
upper bound at a load factor of 1/2, and the hash stored in the slot so a probe
compares keys only on a hash match. Two allocations for any list length, and the
probe stays in cache instead of chasing a pointer per step. It grows if the bound
was under-estimated, so a wrong bound is slow rather than wrong (covered by
`FlatDedupSet/GrowsPastAnUnderEstimatedBound`).

**Where else this shape is** — the grep's other function-local hash containers,
none of which is on a measured hot path today, so none was changed:
`SdlTtfTextBackend`'s font-scan `seen` sets, `WorkspacePluginRuntime`'s
`unique_languages`, `GitRepositoryService::conflicted_paths`, `ControlSpec`'s
duplicate-id `seen`, and `WorkspaceShellPlugins`' `active_language_servers` /
`active_debug_adapter_types`. `FlatDedupSet` is a drop-in for each when one of
them turns up in a trace.

#### 2026-08-07: `settings.apply_*_all_tabs`, the entry's own suggested start

    settings.apply_cheap_family_all_tabs      10,563 -> 4,036  (-61.8 %)
    settings.apply_contract_family_all_tabs   10,752 -> 4,224  (-60.7 %)
    settings_change_many_tabs (total)         56,856 -> 43,801 (-23.0 %)

`FindSettingInfo(id)` materialized the **entire settings catalogue** — every
built-in spec and every plugin contribution, four `std::string`s plus an
enum-value vector each, ~200 of them — and then linear-scanned it for one id and
threw the rest away. That was 42 % of the phase, from a function whose whole job
is one lookup.

It builds only the matching `SettingInfo` now: `FindBuiltinSettingSpec` first
(the same static-span scan the read path already used), then the contributed
list. `AllSettingInfos` keeps the loops, hoisted into two shared constructors so
the two paths cannot drift.

The other half is the same shape one level up: `WriteSettingValue` called
`FindSettingInfo` and read exactly one field off it, `info->scope` — then
immediately did a *second* lookup (`FindBuiltinSettingSpec`) for the parse. It
takes `FindSettingScope(id, host)` now, which returns the enum and copies nothing.
That is a variant of this entry's own first shape — "work computed before the
guard that discards it" — with the guard replaced by a caller that simply never
reads 99 % of what it asked for.

#### 2026-08-07: `multi_tab.open_tabs` — normalising the tabs that do NOT match

    multi_tab.open_tabs            12,084 -> 6,414   (-46.9 %)
    multi_tab_cycle (total)        18,953 -> 13,283  (-29.9 %)

Opening a file walks **every open tab in every group, twice** — once to decide
whether any view of that path exists (`ReloadEditorTabsForPath`'s any-match
probe) and once in `DiskSignatureMatchesOpenView` — and each step compared by
calling `lexically_normal()` on the tab's path. That is ~12 allocations, and it
was paid per tab per open, so opening the Nth tab cost O(N) normalisations and a
session cost O(N²). The dominant caller is Ctrl+P to a file that is *already*
open, which is exactly the case that walks the whole strip.

The subtlety is which side to guard, and the first attempt got it wrong. Skipping
the normalisation when the two paths compare equal as text is correct but nearly
worthless: a scan is mostly **mismatches**, and every mismatch still normalised in
order to be rejected. The fix that works is the other guard —
`util::PathTextNeedsNormalizing`, the allocation-free scan — because a path whose
own text is already normal cannot become the (already normal) target by
normalising, so the string compare has already answered for it. Only an unusually
spelled path reaches `lexically_normal()` at all. First guard alone: -4.5 %. Both:
-46.9 %.

`EditorViewPathRef` / `EditorViewPathIs` in `WorkspaceTabState.h` carry it, so the
comparison sites stop materialising a `std::filesystem::path` per tab as well —
copying one costs about what normalising one does (a pathname string plus a
component list holding a path per component).

#### 2026-08-07: the tab strip asked for one label and built two

    switch_and_idle.switch_and_settle   16,034 -> 12,307  (-23.2 %)
    multi_tab.open_tabs                  6,414 ->  5,992
    resize.compact_to_regular            1,824 ->  1,512  (-17.1 %)

`RelativePathLabel` inside `TabTextModel` was the single top allocation site of
both `multi_tab.open_tabs` and `switch_and_idle.switch_and_settle`, appearing
**twice per tab** on every tab-strip geometry rebuild. Two independent causes:

  - `TabDisplayTitle(...)` was `TabTextModel(...).display_title` — it built the
    whole text model, including the tooltip's project-relative label, and returned
    one field. The tab strip asks for the title and the tooltip through *separate*
    providers, so every tab resolved its relative label twice.
    `BuildWorkspaceTabDisplayTitle` / `BuildWorkspaceTabTooltipLabel` split it,
    and `TabForLabels` + `TabLabelPath` share the lookup by reference so neither
    entry point copies a `std::filesystem::path` to read it.
  - `RelativePathLabel` itself spent ~30 allocations arriving at what is, for
    already-normalized inputs, a substring: two `lexically_normal`s, a
    `lexically_relative`, a third `lexically_normal`, a `generic_string` — each
    materialising a component list. It now takes the substring directly when both
    spellings are already normal (`util::PathTextNeedsNormalizing`) and the path
    sits under the root (`util::NormalizedPathEqualsOrWithin`), and falls back to
    the general form otherwise. A `"."` root is excluded: its members carry no
    `./` prefix once normalized, so there is nothing to trim.

#### 2026-08-07: `OrderedSidebarViews` ran per frame and built a hash map to do it

The scaffolding audit ([163](#td-2026-08-07-163)) turns out to double as
[159](#td-2026-08-06-159)'s ranked worklist: it records each phase's top *product*
sites, so summing them across all 115 phases gives a cross-phase table of where
the suite allocates. `OrderedSidebarViews` came out of that — not the largest
single site, but present in **every frame-pumping phase** (`compare_selection`,
`diff.next_hunk_burst`, `editor_indent_guides_paint`,
`editor_scroll_only_no_content_bump`, …) because `SidebarModeRow` calls it on the
frame path.

Per call it built four containers to answer a question about six built-in views:
`SidebarViews(...)`'s vector, an `unordered_map` index over the policy list (one
heap node per policy, plus its bucket array), an `ordered` vector, and the result
— and `resolve()` returned a whole `SidebarViewPolicy` **by value**, so the
not-found branch constructed a `std::string` from the id to fill a field the
caller never reads.

The map is gone (both sides are bounded by the sidebar view set, so a linear scan
is a few dozen `string_view` compares against no allocation); `resolve` returns
the two fields that are read; and the ordered list is built inline from the two
sources rather than from a third vector that is immediately filtered and dropped.
Two allocations a call instead of ~10.

**Read, inherent** (grep hits that are not this shape, recorded so the next pass
does not re-open them):

- `GitBlameService`'s `blame_by_line` is an `unordered_map<std::size_t, …>` keyed
  by line number, which reads as a dense key — but it holds only the *loaded
  window* of a file that may be 50k lines, so it is genuinely sparse. A vector
  indexed by line would cost the whole file to cache forty rows.
- `CompareModel::BuildExactLineOps`' `line_occurrences` / `ids` maps are keyed by
  `std::string_view` and are one node per *distinct* line, which is the intern
  table the DP needs. `FlatDedupSet` would help, but the map values are read
  back (occurrence counts, equality-class ids) rather than only tested for
  presence, so it wants a flat *map*, not the set. Filed as
  [164](#td-2026-08-07-164).
- `WorkspaceKeybindingRegistry`'s `resolved_contexts` is keyed by a packed
  (keycode, modifiers) chord — sparse, not a counter. It is one node per distinct
  chord per keybinding *reload*, not per key press.

### TD-2026-08-06-157 — a multi-caret edit's undo entry is as big as the DISTANCE between its carets. RESOLVED 2026-08-07 for the two apply paths; the shaping ops moved to [160](#td-2026-08-07-160).

Found by pointing the phase-scoped tracer at `editor_surround_multi_caret`, the
next interactive scenario after the two [149](#td-2026-08-06-149) named.

Both multi-caret apply paths (`TextViewport::TryMultiCaretPairInsert`,
`TextViewport::ApplyMultiCaretEdit`) and every shaping line op
(`ShapingActions.cpp`, via `ResolveLineRange`) describe the edit as **one
contiguous line-range replacement** spanning the first to the last affected caret
line. `TextViewportUndoHistory::Entry` is that shape and only that shape:
`start_line` plus a `before_lines` / `after_lines` pair.

So the cost of one keystroke is proportional to how far apart the carets are, not
to how many there are. Eight carets 8,400 lines apart in the 50k-line fixture
capture 8,401 lines for the before-image and 8,401 for the after-image — and the
entry **retains** both for the life of the undo stack, against a
`kMaxHistoryBytes` budget that will then evict real history. `Ctrl+Shift+L` on a
common token in a large file puts carets across the whole document; every
keystroke after that copies the whole document twice.

The redundant multiples are gone (four copies on the pair-insert path, two on the
line-op path — see the 2026-08-06 commits): what is left is the two the model
requires.

**The interior lines are identical in both images.** `BuildEntryForDocumentChange`
trims a common prefix and suffix, which cannot touch the middle, and
`FinishActiveGroup` explicitly *re-fills* the gaps between disjoint group ranges
from the current buffer — the group frame already carries the edit as a set of
disjoint ranges (`UndoGroupFrame::disjoint_entries`, whose own comment says it
exists so a non-contiguous edit never materialises a whole-buffer snapshot) and
then stitches that set back into one contiguous entry at the end.

**The fix is a multi-range Entry**, which is what VSCode's undo model is (a set of
`ISingleEditOperation`s, not a line span): keep `Entry`'s existing single-range
fields as the common case and add a list of additional disjoint ranges, empty for
every entry except a non-contiguous multi-caret edit. `FinishActiveGroup` then
stops stitching, and both apply paths stop building a spanning aggregate.

**Why it is filed rather than done.** The consumers of the contiguous assumption
are not all in the history: `ApplyHistoryEntry`'s replace, `before_line_count()` /
`after_line_count()` (cache invalidation, wrapped-row splice, group bookkeeping),
`EntryContentBytes` for the byte budget, the coalescing/merge predicates, and
`BuildAppliedEditLineSpan`. Undo is the part of an editor where a wrong answer is
least recoverable, so this wants its own change with a diff-against-oracle test,
not a ride-along.

Instrument first: nothing currently reports the span/edited-line ratio, so there
is no measurement of how far apart real carets get. A counter on the aggregate
(`editor.multi_caret_span_lines` vs `editor.multi_caret_edited_lines`) would say
whether the common case is a 30-line Ctrl+D run or a whole-file select-all.

**Done 2026-08-07.** `Entry::extra_parts` is the multi-range form: each part
records both its pre- and post-edit start index, and every apply — buffer splice
and derived-cache update alike — walks the parts highest-first, so a part's
recorded index is still valid when it is reached. `FinishActiveGroup` stops
stitching (the frame already held the disjoint set); both multi-caret apply paths
capture per *footprint* rather than per span, where a footprint is a maximal run
of overlapping-or-touching caret line ranges — which is what keeps two carets on
one line in a single capture window, and every window untouched when it opens.

Every consumer that assumed one contiguous splice now asks `is_multi_range()`:
the coalescing predicates refuse the merge, and `BuildAppliedEdit` /
`BuildAppliedEditLineSpan` publish nothing — the same decision
`ApplyMultiCaretEdit` already made for a multi-region edit, so marker consumers
keep their existing resync fallback rather than dragging markers on preserved
lines.

    editor_surround_multi_caret   76,456 -> 933 allocations   (8.0 -> 5.1 ms)
    editor_shaping_multi_caret    57,362 -> 19,952 allocations

The counters this entry asked for shipped as
`editor.multi_range_undo_lines_kept` / `..._lines_spanned` (plus an entry count).
On `editor_surround_multi_caret` they read **14 against 16,802** — eight carets
1,200 lines apart, so the ratio really is the whole-file case, not a 30-line
Ctrl+D run.

Covered by `TextViewport/MultiCaretUndoMatchesSnapshotOracle` (seven edits mixing
two-carets-on-one-line, adjacent lines, line-count changes in both directions, a
plain single-caret edit and a grouped shaping op; buffer snapshotted after each,
then undo / redo / undo replayed against those snapshots) and
`MultiCaretUndoEntryDoesNotSpanTheGap` (the cost claim, which the oracle alone
does not test).

### TD-2026-08-07-160 — a multi-caret shaping op edits every line BETWEEN the carets. RESOLVED 2026-08-07.

Split out of [157](#td-2026-08-06-157), whose fix covered the two multi-caret
apply paths but not the third shape it named.

`ShapingActions::ResolveLineRange` widens to `min..max` over every caret, and
`ToggleLineComment` / `IndentSelection` / `OutdentSelection` / `MoveLineUp|Down`
then rewrite **every line in that range**. With carets on lines 10 and 100 and no
selection, `Ctrl+/` comments all 91 lines. VSCode comments two.

So this is a fidelity bug first and a cost bug second, and the two have the same
fix: emit one edit per caret line (or per contiguous run of caret lines) inside an
undo group, which the group frame already aggregates into the multi-range entry
157 shipped. The undo side therefore needs no further work — only the ops do.

`editor_shaping_multi_caret` is the measurement: 32 carets 25 lines apart, and it
still replaces the whole 800-line span twelve times (19,952 allocations after
157). Expect it to fall to roughly the caret count once the ops stop widening.

Watch for the deliberate case: a op invoked with a genuine SELECTION spanning
lines must keep operating on the whole selection. The widening is only wrong for
*carets*, and `ResolveLineRange` currently cannot tell the two apart because it
folds them into one range.

**Done 2026-08-07.** `ResolveLineRanges` returns a sorted set of DISJOINT regions,
one per caret, with overlapping-or-touching neighbours merged — so two carets on
one line collapse to one region and two on adjacent lines are one edit rather than
two abutting ones. Each op emits one edit per region inside an undo group, which
the group frame already folds into [157](#td-2026-08-06-157)'s multi-range entry,
so the whole gesture stays one undo step. A genuine selection is one region and
still operates on every line it spans.

Decisions are **per region**, matching VSCode (one `LineCommentCommand` per
selection): a caret in a commented block uncomments while a caret in an
uncommented one comments, and each region comments at its own indent. A move
region pinned against the edge of the buffer no-ops alone while the others move.
`SortLines` generalises the same way — a one-line region has nothing to sort,
which is the pre-existing single-caret answer.

    editor_shaping_multi_caret   19,952 -> 10,043 allocations

Reading the residue then found the other half, which is not a shaping bug at all:
every entry recorded inside an undo group captured the full view state three times
and each capture deep-copied the secondary-caret vector, all of it discarded by
`FinishActiveGroup`. With one region per caret that is 3xN copies of an N-element
vector per keystroke — a 256-caret grouped indent allocated 12.9 MB.
`CaptureViewStateForGroupedEntry` captures the caret set only when no group is
active.

    editor_shaping_multi_caret   10,043 -> 8,519 allocations

Covered by `EditorEssentials/Shaping/MultiCaretOpsDoNotTouchLinesBetweenCarets`
(the fidelity claim, across comment/outdent/sort plus the merge and per-region
cases), `.../MoveLineSkipsOnlyThePinnedRegion`, and
`TextViewport/GroupedChildEntriesDoNotCopyTheCaretSet` (budget placed from both
measurements: 2.7x above the fixed cost, 12.6x below the defect).

### TD-2026-08-07-165 — every perf scenario's shell read the developer's real `~/.local/state/microide`. [RESOLVED 2026-08-07.]

Found while chasing what looked like an allocation regression in
`repo_open_rss_idle` (baseline 369, measured 627, +70 %). It was not a
regression, and it was not this machine being loaded: allocation counts are
deterministic, and it reproduced to the byte across runs. It was the developer's
home directory.

`PerfHarness::Driver` holds a `workspace::WorkspaceShell` **by value**, so
`Driver driver;` in `RunScenario` runs the shell's constructor — which resolves
the user-level state root and loads `recents` from it. The isolated app root
(`EstablishIsolatedAppRoot`, which points XDG_CONFIG/STATE/CACHE/DATA_HOME at a
fresh `/tmp` tree) was established inside `InitializeDriver`, **one statement
later**. Every scenario's shell therefore read the real state directory, and the
allocation count of any scenario that opens a project scaled with how long
microide had been used on that machine. Writes went to the isolated root, so the
directory was never obviously wrong — only the reads leaked, which is why this
survived a suite whose whole premise is that allocation counts are exact.

`strace -e trace=openat` is what settled it: the scenario child's very first
`openat` is `/home/gef/.local/state/microide/recents`, and the *next* one is the
isolated root being created.

**The fix** is the ordering: `RunScenario` establishes the isolated app root
before declaring the `Driver`, and `InitializeDriver` keeps its own call only as
a fallback for a caller that builds a `Driver` itself. `repo_open_rss_idle` then
measures exactly its committed 369 on this machine, with no baseline change — the
committed baseline was right all along and the runner was lying.

**What this invalidates.** Any allocation figure recorded on a machine with a
non-empty `~/.local/state/microide` is suspect for scenarios that construct a
shell. `settings_change_many_tabs` measured 43,801 before this fix and 17,201
after, on identical code. The 2026-08-07 rebaselines taken during this session
were re-recorded afterwards; earlier baselines were recorded on this same
workstation and should be re-checked when the suite is next rerecorded
([161](#td-2026-08-07-161)).

**The general lesson**, for `dev-docs/project/validation-traps.md`: an "exactly
deterministic" metric is only deterministic with respect to the inputs the
harness actually controls. This one controlled the environment variables and then
read state before installing them. A cheap standing check is to assert, once per
run, that the resolved user state root is under the isolated root — the harness
knows both.

### TD-2026-08-07-164 — the exact-line diff interns every distinct line through two node-per-element hash maps. [RESOLVED 2026-08-10 — and the entry named the wrong scenarios.]

**Shipped**: three `unordered_map`s deleted from `CompareModel.cpp`, no second
container written.

The entry proposed a `util::FlatHashMap<K, V>` and filed the item rather than
doing it because that is "a second container to get correct and test". It is not
needed. `FlatDedupSet` already stores its keys in an append-only vector, so a
key's index in `keys()` **is** a dense equality-class id — the new `Intern(key)`
returns it, and any caller wanting a value per distinct key hangs a plain
`std::vector<V>` off that id. One container, one new method, four new tests.

What went, per call site:

- `BuildExactLineOps`'s `line_occurrences` map became a `std::vector<uint32_t>`
  indexed by class id. Under `ignore_whitespace` the counts are now per
  equality class rather than per exact line, which is a deliberate correctness
  improvement: the rarity weight and the relation the DP matches on finally
  agree.
- Its two id maps became one `FlatDedupSet<std::string_view>`. The
  `ignore_whitespace` branch used to key an `unordered_map<std::string, ...>` on
  a whitespace-stripped **owned copy** of every distinct line, purely so the map
  had something to hash; a whitespace-insensitive Hash/Eq pair keyed on the
  original view removes the copies outright.
- `BuildUniqueLineAnchors` — the anchored fallback path, which is the one large
  diffs take — had the same shape and got the same treatment.

**Measured, and this is the part worth keeping**: the entry says to size the work
against `diff.open_large_compare` (26,967) and `diff.open_large_patch` (26,952),
and predicts "~2 allocations per distinct line on every compare/diff open". Those
two phases moved by **7 allocations each**:

    diff.open_large_compare    27,541 -> 27,534
    diff.open_large_patch      27,526 -> 27,519
    merge_model.build_interleaved  12,297 -> 8,673  (-29.5 %)

The reason is `kMaxLineLcsMatrixCells`: `BuildExactLineOps` is only ever reached
with slices whose product fits 250k cells, so on a 14k-line file it never sees
more than ~500 lines a side and its maps were never big. The "~28k node
allocations before the diff starts" in the original entry was computed from the
file size, not from what the function is called with. The real win landed on the
merge model, which feeds it whole interleaved documents.

**Generalisable**: an entry that estimates a cost from the input size is
estimating the caller's input, not the callee's. Every capped inner algorithm in
this repo breaks that inference. Read the phase counter before sizing the fix —
here the fix was worth doing anyway, but it would have been ranked very
differently.

The original entry follows.

Found by [159](#td-2026-08-06-159)'s grep-first pass, alongside the `RankedUnion`
fix that pass took.

`CompareModel::BuildExactLineOps` builds two `unordered_map`s before its DP:
`line_occurrences` (line → how many times it appears on either side) and `ids`
(line → equality-class id). Both are one heap node per **distinct line**, so a
14k-line compare pays ~28k node allocations before the diff starts, and both are
thrown away when the function returns. That is the same shape the grep-first pass
fixed in `RankedUnion` — except `util::FlatDedupSet` does not fit, because these
are maps whose *values* are read back, not sets that are only tested for presence.

The answer is a flat open-addressed **map** with the same layout `FlatDedupSet`
already uses (slot table sized once from `left_count + right_count`, hash stored
in the slot, keys and values in their own arrays). Doing it as
`util::FlatHashMap<K, V>` and making `FlatDedupSet` its degenerate case is
probably right, but that is a second container to get correct and test, which is
why this is filed rather than done.

Worth ~2 allocations per distinct line on every compare/diff open. Size it with
the tracer against `diff.open_large_compare` (26,967) and `diff.open_large_patch`
(26,952) before starting: the 2026-08-07 pass already took the whole-side
materialisation out of both, so what remains there is a mix and these maps are
only part of it.

### TD-2026-08-07-162 — the debug value tree stores its nodes in a hash map keyed by a dense counter. [RESOLVED 2026-08-07 — and the paging path moved further than the expand path.]

    value_tree.build_expand   22,643 -> 2,003  (-91.2 %)
    value_tree.paging          5,097 ->    93  (-98.2 %)

Found by [159](#td-2026-08-06-159)'s tail pass, reading `value_tree.build_expand`.

`DebugValueTree::nodes_` is an `unordered_map<std::uint32_t, Node>` and `AddNode`
is **88 % of that phase** — one 184-byte hash node per tree node, on every
`variables` response during a debug stop. The keys come from `next_id_++`: they
are dense, monotonic, and never reused, so this is a hash table over its own array
indices. The same shape the tail pass fixed in three other places, except here the
container is the model's identity rather than a scratch index.

A `std::vector<Node>` indexed by id makes `FindNode` an O(1) load and the growth
amortised, but it is not a drop-in: `EraseSubtree` removes nodes mid-life, so the
vector needs tombstones (or a free list) and `FindNode` needs to distinguish a
live slot from a dead one; every `FindNode`/`FindNodeByReference` caller holds
`Node*` across calls that can insert, which is safe against a map and is NOT safe
against a vector that reallocates. That last point is the real work — it wants an
id-based audit of the callers, not a container swap.

Worth ~20,000 allocations per `debug_value_tree_expand_large` iteration.

**Not** the row list: `value_tree.rebuild` already reads 0 allocations, and the
in-place row reuse that fixed the Settings overlay and the Breakpoints panel moved
this scenario by 3 allocations. It was tried and reverted.

**What shipped.** `nodes_` is a `std::vector<Node>` and the slot for node `id` is
`nodes_[id - id_base_]`. `id_base_` is the piece that reconciles a vector index
with ids that must stay *globally* monotonic (the existing defence against a stale
async reply aliasing a fresh node): it advances to `next_id_` whenever the tree is
emptied, so within one populated tree the live ids are exactly `[id_base_,
next_id_)` — dense and in insertion order — while an id from a previous stop sorts
below `id_base_` and still resolves to `nullptr`.

Three consequences, each of which is now a test:

  - `EraseSubtree` **tombstones** (`live = false`, and a move-assigned default
    `Node` releases the three strings and the child vector) rather than erasing,
    because removing an element would renumber every later node.
  - the `kMaxLoadedNodes` budget therefore had to move from `nodes_.size()` to a
    live count. Counting slots would let a tree that repeatedly replaces one small
    container's page report itself truncated while holding 200 values. The slot
    array carries its own ceiling (`kMaxNodeSlots`, 2x the node budget) so the dead
    space stays bounded inside one stop; a stop boundary reclaims it.
  - `AddNode` can reallocate, so a `Node*` may not be held across one. That was
    safe against a map and was exactly what `ApplyVariables`' attach loop did (with
    a comment explaining why it was safe). It now reads everything it needs from
    the parent before the loop and re-resolves once after — which also removes a
    pointer chase per child.

The paging scenario moved further than the expand scenario because its 5,000
children carry no `variablesReference`, so nothing lands in `reference_to_node_`;
the expand scenario's residue is that map (one node per *container* child) plus the
row list. `reference_to_node_` is keyed by adapter-assigned ints, which are not
dense and not ours, so it stays a hash map.

Covered by `BoundedResourceCaps/DebugValueTreeBudgetCountsLiveNodesNotErasedSlots`
(pages a small container past `kMaxLoadedNodes` in *slots* and asserts it is not
truncated), `.../DebugValueTreeStaleNodeIdDoesNotAliasAfterClear`, and
`.../DebugValueTreeErasedNodeIdStaysDead`. The node-storage cluster moved to
`DebugValueTreeNodes.cpp` — the change pushed `DebugValueTree.cpp` five lines over
the debug-subsystem TU cap, and the rule says carve a companion rather than raise
the cap.

### TD-2026-08-07-163 — no phase gate has been checked for how much of it is the scenario's own scaffolding. [RESOLVED 2026-08-07 — the answer is one, and it is the audit's own artifact.]

**Result: `plugin_status_item_update` was unique.** All 115 phases audited, 0
errors, and exactly one at or above the 20 % threshold — `value_tree.paging` at
28 %, which is 50 allocations of `MakeVariables` building the 5,000 DAP variables
the scenario streams, against a product path that TD-2026-08-07-162 had just cut
by 98 %. A fixed input cost became a large *share* of a small total; nothing is
wrong with the gate. 97 of the 115 phases read exactly 0 %.

The committed table is `dev-docs/performance/perf-phase-scaffolding-audit.md`;
the tool is `tools/audit-perf-phase-scaffolding.py`.

**Three things the audit needed that were not in the plan:**

  - the tracer printed only its top 12 sites, with the tail as
    "... and N more site(s)" and no counts, so nothing could attribute *all* of a
    phase. `MICROIDE_PERF_ALLOC_TRACE_SITES` makes the dump exhaustive.
  - the innermost repo frame is not always the answer. LTO drops file/line for
    some inlined bodies but keeps the symbol, so the walk has to fall back to the
    demangled name (`microide::tests::` vs `microide::`) or it sails past the
    product frame and lands on the harness that called it.
  - and when even the symbol is gone — the whole lambda body flattened into
    `ScenarioContext::Measure` — the honest answer is **unattributed**, not a
    bucket. Charging those to the enclosing frame is what made
    `assist_merge::RankedUnion`, a header template instantiated in the scenario
    TU, read as "100 % scaffolding" when it is 100 % product. Eight phases have
    such sites; all are small next to their totals.

**And the audit found something bigger than what it was looking for.** Building it
meant reading a lot of allocation counts closely, which is how
[165](#td-2026-08-07-165) surfaced: every scenario's shell was reading the
developer's real `~/.local/state/microide`, so allocation gates on a shell
scenario were partly a measurement of the operator's machine. The product-site
half of the audit's output also turned out to be [159](#td-2026-08-06-159)'s
ranked worklist for free — summing each phase's top *product* sites across all 115
gives a cross-phase table of where the suite allocates, which is what surfaced
`OrderedSidebarViews` running per frame.

**Making it standing** is still open, and the entry's own recommendation stands: a
lint over `tests/perf/*.cpp` for string concatenation / `std::to_string` inside a
`Measure()` lambda. Filed as [166](#td-2026-08-07-166) rather than done, because
the audit is now cheap to re-run and the finding rate was 1 in 115.

<details><summary>The original entry</summary>

`plugin_status_item_update` measured 172,005 allocations. **95 % of them were the
measured loop building its own input** — `"plugin.status.item_" + std::to_string(i)`,
two allocations per call, against an `ApplyStatusItemUpdate` that allocates none.
Moving the construction outside the `Measure()` window took the phase to 16,007
with no product change at all.

That gate could not have failed on a regression in the function it is named after.
A doubling of `ApplyStatusItemUpdate`'s cost would have moved the number by under
5 %, inside its own tolerance. It is the same class of defect as the architecture
lints that were structurally incapable of firing (see
`dev-docs/project/validation-traps.md`): green, and blind.

It was found by accident, on the third phase [159](#td-2026-08-06-159)'s tail pass
happened to trace. **There are 116 phase gates and no reason to believe it is
unique**, so the question is not "is there another one" but "which ones, and how
bad".

**Method.** The tracer already resolves every site's stack; the only new thing is
bucketing. For each phase:

```bash
MICROIDE_PERF_ALLOC_TRACE=1:100000000 MICROIDE_PERF_ALLOC_TRACE_PHASE=<phase> \
  ./build/microide-perf-make/microide/microide_perf --scenarios=<scenario> \
  --iterations=2 --no-isolate
```

then `addr2line -e <binary> -f -C -p -i` each site (the `-i` matters — everything
is inlined) and bucket its allocations by whether the innermost `microide::` frame
resolves into `tests/perf/` or into `src/`. The scaffolding share is the
`tests/perf/` bucket over the total. Script it; 116 phases is a loop, not a chore.

Machine state does not affect this, so **it does not need an idle runner** — unlike
[161](#td-2026-08-07-161), it can be done any time.

**Reading the result.** A high share is not automatically a defect: the pure-unit
scenarios (`user_config_record_decode`, `dap_protocol_encode_decode`,
`lsp_*_parse`, …) legitimately measure construction, because construction IS the
thing under test. The finding is a phase where the scenario builds a *convenience*
value — a formatted id, a path string, a fixture line — that the product would
have had in hand already. ~20 % is a reasonable threshold to look at; 95 % is not
a judgement call.

**Definition of done**, so this does not become open-ended: a committed table of
every phase with its scaffolding share, and every phase above the threshold either
fixed (input hoisted out of the measured window, baseline rerecorded with
`--update-baseline=deterministic`) or annotated in that table with one line saying
why its construction is the work. A phase recorded as "95 % scaffolding, and that
is the point" is a real result.

**Then consider making it standing.** The audit finds today's; nothing stops the
next scenario from being written the same way. Options, cheapest first: a note in
`guidelines/performance.md`; an architecture lint over `tests/perf/*.cpp` for
string concatenation / `std::to_string` inside a `Measure()` lambda; or a harness
assertion that refuses to write a baseline for a phase over the threshold. The
lint is the one that fits the repo's existing habits — and per
`validation-traps.md`, it would need positive and negative control fixtures.

</details>

### TD-2026-08-07-166 — nothing stops the next perf scenario from measuring its own `operator+`. [RESOLVED 2026-08-10 — and the lint found one on its first run.]

`CheckPerfMeasureBodiesDoNotBuildTheirOwnInput` walks every `Measure(...)` lambda
body under `tests/perf` and rejects `std::to_string`, `std::format`/`fmt::format`,
and `+` adjacent to a string literal. The entry's own preference, and the shape
`ExtractBraceDelimitedBody` already supported.

The legitimate case the entry insisted on covering — a pure-unit scenario where
CONSTRUCTION IS THE WORK — opts out by naming itself: a
`perf-measure-builds-input: <reason>` comment inside the body. The exemption
lives in the file a reader is already looking at, and it greps.

Four control fixtures, because a lint with an opt-out has one extra way to go
quiet: the defect, the fix, the declared exemption, and the blind case (no
`Measure` body found at all, which the rule reports as a violation rather than a
pass).

**It was not only insurance.** First run flagged `multi_tab.open_tabs`, which
composed `"pkg0/file_" + std::to_string(i) + ".txt"` and a
`std::filesystem::path` per tab — twenty paths' worth of construction inside a
gate whose name says it measures opening a tab. Hoisted out of the window. The
entry predicted a finding rate of 1 in 115 and got 1 in 115.

[163](#td-2026-08-07-163)'s standing half. The audit found today's scaffolding-
heavy gates (one, and it is legitimate); nothing prevents the next scenario from
being written the way `plugin_status_item_update` was — a `Measure()` body that
composes its own input string per iteration, so the gate measures `operator+`
against a product function that allocates none.

Cheapest first, per that entry's own list: a note in `guidelines/performance.md`;
an architecture lint over `tests/perf/*.cpp` for string concatenation /
`std::to_string` / `std::format` inside a `Measure(...)` lambda body; or a harness
assertion refusing to write a baseline for a phase over the threshold. The lint
fits the repo's habits and `ExtractBraceDelimitedBody` already gives it the lambda
body; per `validation-traps.md` it needs negative and positive control fixtures,
and the positive control has to cover the legitimate case — a pure-unit scenario
where construction IS the work.

Not done with 163 because the audit is now one command and the finding rate was
1 in 115, so the lint is insurance rather than a discovery tool.

### TD-2026-08-06-158 — the status bar probes the filesystem for `.git` on every painted frame. [RESOLVED 2026-08-10 — both halves, and the event the durable fix needed did not exist.]

    editor_scroll_only_no_content_bump.scroll_frame   6,950 -> 6,450  (-7.2 %)

Three changes, and the middle one is a product bug the entry did not know it was
sitting on:

1. **The allocation.** `internal::HasGitMarker` assembles `<root>/.git` in a
   stack buffer and `stat`s it on POSIX, instead of building a
   `std::filesystem::path` (its own string plus libstdc++'s component list) per
   call. `root.native()` is the string that already exists, so the fast path
   allocates nothing. It benefits every git operation on the background executor,
   not just the frame path — the fallback stays for Windows and for a path longer
   than `PATH_MAX`.

2. **The event the entry assumed existed.** The plan was "`.git` appearing or
   disappearing is an event; wire the marker state to it". It was not an event.
   `GitRepositoryMetadataTracker::ReadCurrentTicks` returns nullopt when there is
   no usable `.git`, and **both** transitions — an in-session `git init`, an
   `rm -rf .git` — landed in the "no baseline to compare" branch, which
   re-baselines and reports nothing. So a `git init` in the built-in terminal
   published no repository change at all, and the sidebar's staleness mark never
   fired for it either. The tracker now emits `HeadChanged` on the presence flip.

3. **The syscall.** With a real event, the probe caches on (project root,
   `sidebar.git.repository_marker_generation`), a counter bumped by
   `ApplyProjectChangeBatch` for every repository change. One stat per repository
   change instead of one per painted frame, and the in-session `git init` the
   original per-root cache got wrong is now covered by a test that asserts both
   directions *and* that an unchanged repository is not re-probed.

Note the ordering the entry got backwards: the cache was not unsafe because
caching was wrong, it was unsafe because the invalidation signal was missing. The
signal was worth having on its own.

The original entry follows.

`StatusBarModelService::Refresh` runs from `WorkspaceShell::PrepareFrameOnce`, and
when there is no git snapshot yet it calls `is_git_repo_valid`, which stats
`<root>/.git`. That is deliberate and the comment says why — caching it by
project root went stale after an in-session `git init` or `.git` removal until a
real git refresh superseded it.

The allocations are gone (it no longer builds a `GitRepository`, which copied the
path and normalised it), but two things remain:

  - `internal::HasGitMarker` still constructs a `std::filesystem::path` for
    `root / ".git"` — its own string plus libstdc++'s component list, ~167 bytes
    across the pair. It is ~1.4 % of `editor_scroll_only_no_content_bump`'s
    per-frame allocations, and it also runs on the background executor on
    essentially every git operation.
  - the **syscall** is still one `stat` per painted frame, which no counter
    reports and no gate covers.

The durable answer is to stop polling: the project already runs a file-index
watcher, so `.git` appearing or disappearing is an event, and
`GitRepositoryService::MarkStale()` is already the "something changed" seam. Wire
the marker state to that and the probe becomes a bool read. Until then this is a
stat on a hot dentry, which is cheap but is not nothing at 120 Hz.

### TD-2026-08-06-138 — the whole-document line-width table is built TWICE per file open. [RESOLVED 2026-08-06 — and the headline was wrong: it is twice per *re*-open.]

`editor.line_width_full_measures` read **exactly 2x the document's line count** on
every scenario that opens a large file, in one iteration:

| scenario | counter | document | 2x? |
| --- | ---: | --- | --- |
| `editor_mouse_selection_drag` | 100,002 | 50,001 (50k_cpp) | ✅ exact |
| `editor_occurrences_scan` | 100,002 | 50,001 | ✅ exact |
| `editor_add_cursor_next_match` | 100,002 | 50,001 | ✅ exact |
| `editor_column_selection_burst` | 100,002 | 50,001 | ✅ exact |
| `large_file_restore_deep_scroll_first_paint` | 100,002 | 50,001 | ✅ exact |
| `editor_indent_detect_open` | 25,732 | 12,866 (1mb mixed) | ✅ exact |
| `editor_save_normalization` | 25,732 | 12,866 | ✅ exact |
| `diff_next_hunk_large_file` | 28,890 | ~14.4k (large diff) | 2 x 14,445 |
| `diff_stage_hunk_large_patch` | 28,890 | ~14.4k | same |
| `diff_stage_selected_lines` | 28,890 | ~14.4k | same |

**Two scenarios were in the first draft of this table and do NOT belong**, which is
worth recording so nobody re-adds them: `editor_toggle_comment_large_selection`
(16,000) and `settings_change_many_tabs` (20,088). The counter is bumped from *two*
sites in `TextLayoutCache.cpp` — `MaxVisualColumns`'s full rebuild, which walks the
whole document, and the incremental edit path, which bumps by `inserted_count`.
Toggle-comment opens the same 50k document, but its 16,000 is **16 toggles x 1,000
selected lines** through the *edit* path, which is that path working exactly as
designed. Check a counter against the document's line count before reading it as a
full rebuild.

**[RESOLVED 2026-08-06.]** The entry said "twice per file open" and named a
hypothesis — indent detection landing after the first table is built, so `tab_size`
changes and the first build is thrown away. **Both were wrong, and the entry's own
instruction is what caught it: confirm the trigger before fixing anything.**

One counter per rebuild reason (`editor.line_width_table_builds` plus
`_rebuild_cold` / `_rebuild_tab_size` / `_rebuild_line_count`, which sum to it)
answered it in a single run. `tab_size` never fired. Both builds were **cold** —
i.e. the table was *empty* at each one, so something wiped it whole in between.

And a genuinely fresh open builds it **once**. Instrumenting
`TabCoordinator::OpenFileInNewTab` showed every 2x iteration reached it with
`existing=1`: the perf driver persists across iterations, so iteration 0 opens the
file and every iteration after that *re*-opens a file that is already open. Which is
where the real defect was:

1. **Re-opening an already-open file re-read it from disk.**
   `OpenFileInNewTab` called `ReloadCleanEditorTabsForPath` unconditionally on the
   existing-tab branch: read the whole file, build a fresh viewport, swap it in and
   drop every derived cache (widths, highlights, folds, undo history) to arrive at
   byte-identical content. That is what Ctrl+P to a file you are already looking at
   did. VSCode focuses the tab. The same cost was paid once per open buffer by the
   focus-regain sweep, `ReloadCleanOpenBuffersFromDisk`.
   Fix: `ReloadEditorTabsForPath` stats the file and returns when every open view
   already records that signature — the same mtime+size equality the self-write echo
   suppression in `WorkspaceShellProjectChanges` already trusts. Clean-only form
   only; the from-disk form is the banner's explicit "discard my edits and reload".

2. **A viewport copy deep-copied the width table and then threw it away.**
   `TextViewport`'s copy constructor copied `layout_cache_` and then called
   `InvalidateVisualColumnCache()`; move construction and move assignment did the
   same to a table they had just stolen for free. The reload copies a viewport
   twice, which is exactly where the second cold build came from.
   Fix: drop only the fold-dependent half (`DropWrappedRowLayouts`). Widths are a
   function of the document's bytes and the tab size, both of which a copy shares;
   only `folding_model_` is reset. Same distinction `SetFoldingModel` already makes.

3. **`ClampScrollState()` read `MaxVisualColumns()` to clamp an offset already at 0.**
   The clamp can only lower `horizontal_scroll_`, so at 0 the answer is 0 either
   way — but reaching it builds the whole table. Every file opens at column 0 and
   every restored background tab stays there. Fix: return early.

Measured on perf-runner-v1, p50 wall / p50 allocations for the whole scenario:

| scenario | wall | allocations |
| --- | --- | --- |
| `editor_add_cursor_next_match` | 5.51ms → 0.26ms | 3,660 → 1,005 |
| `editor_indent_detect_open` | 3.28ms → 0.81ms | 1,961 → 708 |
| `large_file_restore_deep_scroll_first_paint` | 9.38ms → 3.24ms | 3,880 → 791 |
| `editor_column_selection_burst` | 11.14ms → 6.90ms | 2,949 → 278 |
| `editor_mouse_selection_drag` | 12.19ms → 4.47ms | 5,482 → 2,101 |
| `editor_occurrences_scan` | 17.51ms → 10.02ms | 4,676 → 1,641 |

`editor.line_width_table_builds` is 0 across every scenario that reopens a file, and
1 where the content really did change. The three diff/staging scenarios and
`external_change_refresh_open_diff` went from 14,465-14,475 full measures to 0.

Two things to carry forward:

- **The counters in a scenario report cover the whole scenario function, not the
  `Measure(...)` region.** This entry's table read "per file open" because the open
  is in the setup, and the setup is inside the counter window. Check what the
  scenario body does before attributing a counter to the measured work.
- **A trailing regression is not the same as a leading one.** The reload skip means
  a tab's derived caches now survive across the harness's iterations instead of
  being reset by each one, and the resident-growth gates moved accordingly. See
  [TD-2026-08-06-142](#td-2026-08-06-142).

### TD-2026-08-06-139 — five allocation gates drifted UP, unexplained, and passed. [RESOLVED 2026-08-06 — one was real and is a deliberate trade; the other four never happened.]

**[RESOLVED 2026-08-06.]** Both halves were bisected. The entry's headline reading
of the drag half — "~3 allocations per move, on a drag path that should be
allocation-free per move" — **is wrong**, and the way it is wrong is the reusable
lesson: `p50_allocations` is the whole *iteration*, and this scenario opens a
50k-line file before it moves the mouse 160 times.

**The drag: +472, real, and the trade is right.**

Reproduced exactly at both window endpoints, standalone, 10 iterations:
`932ad5d2` = 5,024 and `a460e6cf` = 5,496. Bisected in seven steps
(`--scenarios=editor_mouse_selection_drag --iterations=10`, oracle > 5,200) to
**`f2284f38` "perf(fold): derive folds incrementally from block words instead of
rescanning"**, which measured 7,030 (+2,006); later commits in the same window
brought it back to the +472 the entry recorded.

**The measured phase never moved.** `mouse_selection_drag.160_moves` reads
**960 allocations / 30,720 bytes at every commit in the window** — both endpoints,
the first bad commit, and every bisect step. The drag path is unchanged. The whole
+472 is scenario *setup*: `FoldingModel`'s block partition, built when the file is
opened. `Block` holds four `std::vector`s (bracket closers/openers, indent
dedents/openers) and the 50k fixture is ~195 blocks of 256 lines, so building the
partition is a few hundred small heap allocations. `PrefixState`, the struct
immediately below it in the header, already avoids exactly this — "sliced out of
shared pools so a document with thousands of blocks does not hold thousands of
small vectors" — and `Block` did not get the same treatment. Filed as
[TD-2026-08-06-144](#td-2026-08-06-144).

It stands as measured. ~472 one-time 32-byte allocations at file open bought the
removal of an O(document) fold rescan from *every keystroke* (the commit measures
that at ~26% of `mid_file_edit_latency_large_file` on the same fixture). Speed
first, then correctness, then CPU, then memory: that is the trade the priority
order names, made in the right direction.

**The four diff/staging scenarios: not code, and not in that window.**
`diff_next_hunk_large_file` measures **87,421 `p50_allocations` at both endpoints,
byte-identical**, standalone at 10 iterations against the same fixture. A flat
+1,407 appearing on four scenarios that a controlled A/B cannot reproduce is a
property of the *run*, not of the code — both numbers in the entry's table came
from full 93-scenario runs, and these scenarios are demonstrably context-sensitive:
measured today at HEAD, the same scenario reads **86,741 in-suite against 87,421
standalone**, a 680-allocation offset from process state alone.

The mechanism is not a mystery either. **Six scenarios were added to the suite
inside that window** — `editor_long_line_buffer_search`,
`editor_long_line_horizontal_scroll`, `editor_long_line_select_all_edit`,
`editor_typing_minified_line`, `merge_model_build_interleaved`, `perf_gate_canary`
— four of which drive the minified fixture, one line of megabytes, whose resident
behaviour is documented as allocator-placement-sensitive in
[TD-2026-08-05-136](#td-2026-08-05-136). The heap the diff group meets is not the
heap it met before they existed.

The "shared-constant shape" reasoning in the original entry was sound — one cause,
not four. The conclusion that the cause was a shared *code* change was not; the
same shape is exactly what a changed run context produces.

**What made this expensive, and what fixed it.** Attributing the drag half took a
90-commit bisect because the harness could measure the regression but not point at
it: the counters said a phase allocated 960 times and nothing said where.
`MICROIDE_PERF_BIG_ALLOC_BYTES` could not help — it traces the *largest*
allocations one backtrace per hit, and this was hundreds of 32-byte ones. The
aggregating allocation-site tracer added with
[TD-2026-08-06-141](#td-2026-08-06-141)
(`MICROIDE_PERF_ALLOC_TRACE=<min>[:<max>]`) answers the same question in one run,
and answering "which sites allocate in this phase" is how the *remaining* 960 got
attributed to `ComputeEditorPaneLayouts` — see
[TD-2026-08-06-145](#td-2026-08-06-145).

**Two things to reuse.** *(1)* Check whether a counter moved inside the measured
phase before attributing it to the measured work; a scenario's `p50_allocations`
covers its setup too, and `phase_metrics` in `--report-json` is where the answer
is. *(2)* A drift that a controlled A/B of the two endpoints cannot reproduce is
not a code change, however clean its shape looks in a table.

The original entry follows.

From the same sweep as [TD-2026-08-05-135](#td-2026-08-05-135), and the half a
blind rebaseline would have buried. Measured against the committed baselines
before they were rewritten:

`p50_allocations`, as committed before the sweep against what is recorded now:

| scenario | pre-sweep | now recorded | delta |
| --- | ---: | ---: | ---: |
| `editor_mouse_selection_drag` | 5,010 | 5,482 | **+9.4%** |
| `diff_stage_selected_lines` | 60,528 | 61,935 | +2.3% |
| `external_change_refresh_open_diff` | 61,652 | 63,060 | +2.3% |
| `diff_stage_hunk_large_patch` | 62,447 | 63,854 | +2.3% |
| `diff_next_hunk_large_file` | 85,505 | 86,912 | +1.6% |

Two separate things. The four diff/staging scenarios took a **flat +1,407-1,408
allocations each** — the same absolute number on four scenarios with different
workloads and different totals, so it is one shared change in the diff/staging path
and not four regressions. (It measured a flat +1,326 in the first sweep run and
+1,407 in the one that recorded these; the shared-constant shape is the signal, not
the exact value.) `editor_mouse_selection_drag` is its own: +472 over 160 mouse
moves, ~3 allocations per move, on a drag path that should be allocation-free per
move.

All five passed their envelopes (the default 10% allocation tolerance), which is why
nobody saw them. They are now recorded at their measured values, so this is drift
that has been *accepted into the baseline* — the gate will not catch it again.

**Bisectable cheaply**: both scenarios run in seconds and each step has an exact
oracle (`--scenarios=<name> --iterations=10`, compare `p50_allocations` against the
numbers above). The window is `932ad5d2..a460e6cf` — from the 2026-08-04 commit that
last recorded these baselines to the 2026-08-06 sweep that rewrote them — which is
**90 commits**, so ~7 bisect steps at roughly two minutes each (ccache-warm rebuild
plus a one-scenario run). Script it rather than doing it by hand.

No candidate commit is named here on purpose. The obvious-looking drag refactors
(`d6b38b5d`, `57eff4c3`) both land on 2026-07-30, i.e. **before** the baseline
commit, so they are already inside the recorded number and cannot be the cause —
which is the trap this note exists to close. Check `git merge-base --is-ancestor
932ad5d2 <candidate>` before spending a session on a suspect.

### TD-2026-08-06-140 — the wall gate cannot catch a regression under 2x, and now has the data to. STEP ONE SHIPPED 2026-08-12; step two is the envelope re-cut and needs an idle runner.

**Shipped 2026-08-12 — the normalisation.** `NormalizeWallAgainstBaselineClock`
re-expresses each iteration's wall in the baseline's machine state, weighted by
that iteration's own cpu/wall ratio:

```
normalized_wall = wall * (1 + (clock_factor - 1) * cpu_fraction)
```

Full correction where wall is work, none where wall is sleep, decided per
scenario from measured data rather than from a per-scenario opt-out list somebody
has to maintain. The ratio is clamped to [0, 1] (a threaded scenario can burn
more CPU than wall, and that extra is not this thread's clock exposure), and the
same `ClampNormalizationFactor` sanity clamp the CPU path uses applies. A
baseline with no recorded clock compares raw, exactly as before.

`PerfBaseline/NormalisesWallByItsWorkFraction` pins all three regimes: a
cpu-bound scenario is fully corrected, the same rise at an UNCHANGED clock still
fails (the negative control), and a sleep-dominated scenario gets essentially no
correction. The verdict line now says `cpu+wall normalised`.

**Step two — cutting the 100/150/200 envelopes down to something that gates — is
deliberately not in this change**, exactly as the entry asks: it needs its own
rebaseline plus a multi-run stability measurement, and a too-tight wall gate is
how a suite goes red on half its runs and stops being read. That measurement
needs an idle runner, which is the same constraint as
[161](#td-2026-08-07-161)/[172](#td-2026-08-10-172)/[184](#td-2026-08-11-184).

#### Original entry


Wall envelopes sit at 100/150/200% because wall carries everything the code does not:
scheduler jitter, the 1.44x per-thread clock swing measured in
[TD-2026-08-05-137](#td-2026-08-05-137), and sleep. A 100% p50 envelope does not
detect a constant-factor regression under 2x, and the harness doc is explicit that
this is given up on purpose.

That trade was made when the harness could not tell those apart. It now can:

- the calibration probe says what the machine's clock was, per iteration, and every
  baseline records the clock it was captured at;
- `cpu_ms / wall_ms` per scenario says what fraction of a scenario's wall time is
  actually work — `idle_soak_30s` is ~0.0005 (it sleeps for 27 of its 30 seconds),
  ordinary editor scenarios are ~1.0.

So wall can be normalised the same way CPU is, scaled by how much of it is work:
`expected * (1 + (clock_factor - 1) * cpu_fraction)`. Full correction where wall is
work, none where wall is sleep, decided per scenario from measured data rather than
from a per-scenario opt-out.

The point of doing it is what comes after: with the machine's contribution removed,
the wall envelope can come down from 100% to something that gates. **That second
step is the risky one** — it needs its own rebaseline plus a per-scenario review of
the new envelopes, and a too-tight wall gate is how a suite becomes red on half its
runs and stops being read at all. Do the normalisation and the tightening as two
changes, with a multi-run stability measurement between them.

Deliberately not bundled into the 2026-08-06 pass: CPU normalisation is a strict
improvement that needed no envelope changes, and mixing an envelope re-cut into the
same rebaseline would have made both unreadable.

### TD-2026-08-06-141 — nothing reruns the perf gate, so drift is only ever found by accident. [RESOLVED 2026-08-06.]

**[RESOLVED 2026-08-06.]** The entry asked for a scheduled run plus a dated series
of `--report-json` files. Both shipped, and so did the part the entry warned about
("the thing to get right is that the output must be *read*").

- **`tools/perf-gate.sh`** builds, runs the full gate with
  `--reference-runner=perf-runner-v1`, and writes a dated commit-stamped report
  into `${MICROIDE_PERF_DRIFT_DIR:-~/.local/state/microide/perf-drift}`.
  `--install-timer` installs a weekly systemd **user** timer with
  `Persistent=true`, so a machine that was off on the scheduled day runs on next
  boot — a missed week is otherwise indistinguishable from a clean one in the
  record, which is this entry's own failure mode one level down. `--status` prints
  the last summary; `--drift` re-reports without running anything. Reachable as
  `tools/run-checks.sh perf-gate`.
- **`tools/perf-drift.py`** is the sweep that closed
  [TD-2026-08-05-135](#td-2026-08-05-135), automated: report vs report, or report
  vs the committed baselines, split by what a finding *means* — allocation drift up
  (deterministic, so every row is a code change), allocation drift down
  (improvements the gate is still handing slack for), loose gates, envelope
  pressure. Reports are ordered by their own `metadata.timestamp_utc`, never by
  mtime, which a copy or an rsync rewrites.
- **Each report now records what it was gated against** — a per-scenario `baseline`
  block with expected / actual / tolerance / delta / `envelope_used_percent` /
  passed. Without it an old report says what was measured but not what it was
  measured *against*, and re-deriving the envelope from today's baselines is
  exactly the information a rebaseline destroys.
- **A single run now reports envelope pressure.** Any *passing* gate at or above
  75% of its tolerance is printed at the end of the run, allocation gates first and
  unqualified (deterministic ⇒ a near-miss is a code change), duration/resident
  gates separately and labelled machine-sensitive. This is the leading indicator
  [TD-2026-08-06-139](#td-2026-08-06-139) needed: its drag drift was 94% of its
  envelope and the run printed `PASS` and nothing else.
- **The run exits non-zero** on a gate failure *or* on flagged deterministic drift,
  writes `latest-summary.txt`, and raises a `notify-send` notification.

One more piece of instrumentation came out of using it:
`MICROIDE_PERF_ALLOC_TRACE=<min>[:<max>]` aggregates call stacks for every
allocation in a size band and prints the table most-frequent-first. The counters
could say a phase allocated 960 times and nothing could say where, which is why
TD-2026-08-06-139 needed a 90-commit bisect.

**The first recorded run found the gap in the runner itself.** It landed while an
unrelated 24-job compile was running and reported 17 wall/CPU failures across 8
scenarios — every one of which passed on a quiet machine an hour later. That is
worse than no run: it writes a contended measurement into the series, where every
later comparison reads it as the product's past. The calibration probe did *not*
catch it (only 9.5% of iterations sat above 1.25x the run's own median; the probe
is short enough to slip between a competitor's slices), so the guard is a
1-minute-load check before the run rather than a post-hoc statistic:
`MICROIDE_PERF_MAX_LOAD` (default 2.0), and the script refuses to measure above it
and names what is competing. `--force` measures anyway and files the result under
`contended/`, out of the series — the same treatment `--scenarios=` runs get under
`subset/`. Allocation counts are unaffected by contention and stay trustworthy in
both.

Three things NOT done, on purpose. The timer is installed by an explicit
`--install-timer`, not by the build — a check that installs a scheduler as a side
effect of a `cmake` run is worse than no scheduler. The iteration count is fixed at
10 rather than exposed as a tuning knob: a baseline records a p50/p95 captured at
some sample size, so re-measuring at another one compares percentiles of
differently-sized samples and reads as drift that is not there. And the first
authoritative run is **deferred until the machine is idle** rather than taken from
the contended one — the whole point of the series is that its first entry is not a
guess.

The original entry follows.

The process half of [TD-2026-08-05-135](#td-2026-08-05-135). Baselines drifted
40-80% loose, and eleven gates plus five upward regressions accumulated, because the
full gate only runs when somebody decides to run it — and the failure mode is silent
by construction: gates trip on *increases*, so a baseline that has gone loose is
green forever.

CI cannot re-measure (baselines are absolute timings from the pinned
`perf-runner-v1` host, which is the maintainer's workstation — see the "Hosted perf
gating" note in `active-work.md`), and that is not changing. But the gate does not
need CI to run on a schedule; it needs *something* to run it on the reference
machine and report:

```
./build/microide-perf-make/microide/microide_perf --iterations=10 \
    --reference-runner=perf-runner-v1 --report-json=<dated path>
```

It already exits non-zero on failure and prints a per-scenario verdict, and a
scheduled run has a second, larger payoff: a dated series of `--report-json` files
is the drift record nobody has today. Comparing two of them is the sweep that closed
TD-2026-08-05-135, done automatically instead of by hand.

Roughly an hour of work. The thing to get right is that the output must be *read* —
a scheduled run whose failures nobody sees is the same defect one layer up.

### TD-2026-08-06-142 — a tab's derived caches have no measured ceiling, and nothing caps their sum across tabs. RESOLVED (measurement) 2026-08-06; the cap is WON'T DO as of 2026-08-12.

The entry's own instruction was "build the measurement first; a cap chosen without
it would be a guess". The measurement shipped and answers the question: ~4.13 MiB
per WARMED large tab, dominated by per-line highlighter state (46 %) rather than
by the width table the entry expected. Every component is bounded by its document;
only the sum across tabs is not.

Closed on the cap because the decision the measurement enables is "no cap", not
"cap unchosen":

- Memory is LAST in the priority order, behind speed, correctness and CPU.
- Every eviction candidate is a cache that exists to keep a tab fast. Dropping a
  background tab's width table and visible-line LRU buys 1.2 MiB and pays for it
  on the next switch to that tab — a latency cost on the interaction the user is
  waiting on, to reclaim memory nothing is short of.
- The growth converges: 4 MiB per warmed tab, and tabs are warmed by being
  looked at.

If this is ever revisited, the counters (`editor.tab_derived_cache_*`) and the
`editor_tab_derived_cache_residency` scenario are what make it a measurement
rather than an intuition, and they stay.

#### Original entry


**The entry's first ask, in the order it insisted on**: "That is the first thing
to build; a cap chosen without it would be a guess." So the measurement shipped
and the cap did not.

`TextViewport::DerivedCacheBytes()` reports retained heap by container
**capacity** — a vector that shrank still owns its buffer, and the question is
about what is retained, not what is in use — broken down by which cache holds it.
`FoldingModel` and `TextViewportUndoHistory` get the same accounting;
`TestAccess::EditorDerivedCacheResidency` sums across every open tab in every
group and publishes `editor.tab_derived_cache_*`.

The answer, from the new advisory scenario `editor_tab_derived_cache_residency`
— one 50k-line C++ tab, folding on, a full 320-page scroll sweep and a typing run
so every cache reaches steady state rather than whatever the first screen touched:

| component | bytes | share | bounded by |
| --- | ---: | ---: | --- |
| highlight states | 1.92 MiB | 46 % | the **document** (one `SyntaxState` per line) |
| layout cache | 1.21 MiB | 29 % | the document (width table) + 256 rows (LRU) |
| fold model | 0.83 MiB | 20 % | the document |
| highlight tokens | 0.17 MiB | 4 % | the token LRU |
| undo history | 1.1 KiB | — | **use**, not size; capped at 256 MiB per tab |
| **total** | **4.13 MiB** | | nothing, across tabs |

So the unbounded sum the entry names is ~4 MiB per warmed large tab against
`kMaxOpenTabsPerGroup` = 512 and `kMaxEditorGroups` = 2. The entry's guess that
the width table would dominate ("400 KB on the 50k fixture") was low and pointed
at the wrong component: the per-line **highlighter state** is the biggest single
holder, and it is the one the entry did not size.

**Reported, not gated, on purpose.** A gate here would be a cap, and the entry is
right that a cap chosen before the measurement exists is a guess. What is now
possible and was not: pick a ceiling from the table above, and use the counters
to check that dropping a background tab's caches actually buys what it is
supposed to.

**Still open**, and unchanged by this: no aggregate cap, and no eviction of a
background tab's reconstructible caches. The entry's "cheap first cut" (drop the
width table and the visible-line LRU for tabs past some count) is now a 1.2 MiB-
per-tab decision with a number attached rather than an intuition — but memory is
last in the priority order and the growth converges, so it stays low priority.

The original entry follows.

Surfaced by [TD-2026-08-06-138](#td-2026-08-06-138), which stopped re-opening an
already-open file from re-reading it. The reload was also, incidentally, the thing
that periodically reset every derived cache a tab holds. Removing it is right — that
reset was pure waste on a file that had not changed — but it removed the only
bound anyone was relying on, and nobody had noticed there was no other one.

Measured, `editor_fold_viewport_refresh` (a 50k-line file, 96 scroll steps per
iteration), `rss_growth_bytes` per iteration over 40 iterations:

```
23052 3988 4332 4296 4456 3548 1932 1312 1084 1048 1064 280 76 336 320 284 312
  460 592 1200 1696 2008 1916 296 388 188 164 124 220 420 312 220 240 216 356
  304 288 192 172 180                                        (KB, 65.4 MB total)
```

**It converges** — the tail is a few hundred KB per iteration against 23 MB on the
first — so this is cache warming, not a leak, and the committed baseline
(`mean_rss_growth_bytes` 99,669) simply described a product that threw the warm
caches away every iteration. That is the right read of the numbers and it is why
this is a TD rather than a revert.

What is actually missing:

- **No aggregate cap.** Per tab: a per-line width table (one `size_t` per line —
  400 KB on the 50k fixture), a 256-entry visible-line `LayoutLine` LRU, a
  `line_highlight_states_` vector sized to the document, highlight checkpoints, and
  the fold model's per-line caches. Each is individually bounded *by its document*.
  Nothing bounds the sum across open tabs, and `kMaxOpenTabsPerGroup` is the only
  ceiling in the room — it bounds tab count, not bytes.
- **No measurement.** No counter or scenario reports per-tab derived-cache
  residency, so "how much does an open tab cost to keep open?" has no answer today.
  That is the first thing to build; a cap chosen without it would be a guess.
- **A cheap first cut exists**: the width table and the visible-line LRU of a tab
  that is not visible are reconstructible in O(document) and O(rows). Dropping them
  for background tabs past some count trades an open-a-cold-tab rebuild for the
  memory, which is the trade the priority order (speed, then correctness, then CPU,
  then memory) says to make only once memory is actually the constraint.

Priority is genuinely low — memory is last, and the growth converges — but the
entry exists so the next person to read a resident-growth baseline knows why it
moved and that the ceiling is unmeasured rather than chosen.

### TD-2026-08-06-143 — `MaxVisualColumns` stamps a fresh content revision onto a table it did not verify. [RESOLVED 2026-08-06.]

**[RESOLVED 2026-08-06.]** Fix (a), the correct-by-construction one, and it cost
nothing: `MaxVisualColumns` now goes through `LineWidthsAreCurrent` — the same
three-term predicate every other reader of the width table already used — instead
of checking two of its three terms and then stamping the third.

The measured worry ("at the price of a rebuild on any path that currently gets
away with it") did not materialise, and the reason is structural rather than
lucky: every edit path either splices the table through
`UpdateVisualColumnCacheAfterEdit`, which stamps the *post*-edit revision because
the invalidation that bumps it runs first, or drops the table whole. The three
non-`TextViewportEditEngine` `ContentEdit` sites all take the second route.

Both halves of the entry's proposal shipped, because the predicate and a
cross-check answer different questions:

- **The predicate** proves the table is never *read* at a revision it was not built
  for. Its silence is auditable rather than assumed:
  `editor.line_width_rebuild_stale_revision` is a fourth rebuild-reason counter
  that must read 0 in every run, and a non-zero value names the offending path.
- **`MICROIDE_VERIFY_LINE_WIDTH_TABLE`** is the entry's option (b): a debug-only,
  opt-in, whole-table cross-check of every entry (and the memoized maximum) against
  a fresh measurement. The predicate cannot prove the entries are *right*, because
  the splice path derives widths instead of measuring them; this is what a soak run
  can falsify.

The regression test mutates the lines behind the cache's back on purpose — the
defect is the absence of a guard, not the presence of a caller — and carries an
unchanged-revision control so it cannot pass for a cache that simply rebuilds every
time.

The original entry follows.

`TextLayoutCache` carries `cached_max_visual_columns_content_revision_`, and
`LineWidthsAreCurrent()` — the freshness predicate every *reader* of the per-line
width table goes through — checks it. `MaxVisualColumns()` does not:

```cpp
if (cached_max_visual_columns_tab_size_ != tab_size ||
    cached_visual_line_columns_.size() != lines.size()) { ...rebuild... }
// ...scan the (possibly stale) table for its maximum...
cached_max_visual_columns_content_revision_ = content_revision;   // <-- stamped
```

An edit that changes content without changing the line count, and whose path does
not call `UpdateVisualColumnCacheAfterEdit` (nor `InvalidateVisualColumnCache`),
therefore does two things: it gets a maximum computed from pre-edit widths, and it
**marks the stale table current for the new revision** — so every later
`LineFactsIfCurrent` caller believes it, and a caret column conversion or a rendered
row reads a width for text that is no longer there.

No such path is known today. The invariant ("every content edit either splices the
table or drops it") is real and every current edit path honours it — it was
re-checked by hand across `TextViewportEditEngine`, the multi-caret delete, and
`ResetState` while closing TD-2026-08-06-138. What is missing is anything that
*keeps* it true:

- The only check is a debug `assert` in `VisibleLineLayoutRefCached`, which fires
  only for a line that is actually rendered, only under `NDEBUG` off, and only on
  lines ≤ 4096 bytes.
- Nothing covers the max itself, or any line the frame did not draw.

Two candidate fixes, in increasing cost: (a) have `MaxVisualColumns` treat a
revision mismatch as a rebuild reason like the other two — correct by construction,
at the price of a rebuild on any path that currently gets away with it, which is
worth measuring before choosing; (b) a debug-only whole-table cross-check behind an
env flag, so a soak run proves the invariant instead of assuming it.

This became more load-bearing on 2026-08-06: viewport copies used to wipe the width
table, which masked any staleness at every copy. They no longer do
([TD-2026-08-06-138](#td-2026-08-06-138)), so a stale table now survives further.

### TD-2026-08-06-144 — `FoldingModel::Block` holds four `std::vector`s, and the struct below it already knows not to. WON'T DO — measured 2026-08-06, closed 2026-08-12.

Closed as a verified won't-do rather than left open, because the measurement that
would decide it has already been taken and it says no. The entry's premise ("a few
hundred 32-BYTE allocations", i.e. lists holding one or two things) is wrong: the
counters added by the attempt measure a mean word length of ~32 ENTRIES, stable
across scenarios. At an inline capacity small enough to be free (6) three quarters
of the words spill and the allocation count does not move; at one large enough to
matter (~64) it costs ~420 KB per open tab against a fold model that
[142](#td-2026-08-06-142)'s accounting measures at ~0.83 MB total.

The surviving proposal — four model-owned pools plus `(offset, count)` per block —
buys ~780 allocations (~30 us) ONCE PER FILE OPEN, in exchange for a bespoke slab
allocator with in-place reuse and compaction living inside the incremental fold
model. The priority order (speed, then correctness, then CPU/memory) does not
justify that, and this is the kind of entry that gets re-filed every few months
unless the answer is written down.

What shipped and stays: `editor.fold_block_words_stored` /
`editor.fold_block_word_entries`, whose ratio is the number above, and the
`SmallVector`-inside-a-nested-class trap recorded in the header.

#### Original entry


**Tried, measured, reverted.** The obvious cheap fix — `Block`'s four lists become
`util::SmallVector<T, N>`, inline storage with a heap spill, which suits them
exactly (all four word types are trivially copyable PODs, and the sizes are
unbounded so `InlineVector` would be wrong) — does not work, for a reason the
entry could not have known without measuring.

Two counters were added to find out: `editor.fold_block_words_stored` and
`editor.fold_block_word_entries`. Their ratio is the mean word length, and on the
50k-line C++ fixture it is **~32 entries**, stable across scenarios (32.3 / 32.8 /
34.2):

```
editor_fold_recompute        stored=128  entries=4200  mean=32.8
editor_fold_viewport_refresh stored= 32  entries=1096  mean=34.2
```

The entry describes "a few hundred **32-byte** allocations", which reads as
"these lists hold one or two things". They hold thirty-two. That changes every
conclusion:

- At an inline capacity small enough to be free (6), **three quarters of the
  words spilled** and the allocation count did not move at all — 25,975 against a
  25,955 baseline on `editor_fold_viewport_refresh`.
- An inline capacity large enough to matter (~64) costs `64 × 8 × 4` bytes per
  block, i.e. **~420 KB per open tab** on a 196-block document, against a fold
  model that TD-2026-08-06-142's new accounting measures at ~0.83 MB total. A 50 %
  memory regression to save ~780 allocations that happen **once per file open**.

**The entry's own pooling proposal survives this, but barely.** Four model-owned
pools plus `(offset, count)` per block would still allocate the same bytes, just
in ~40 geometric growths instead of 784 discrete blocks — worth ~780 allocations
(~30 µs) at open and ~12 KB of malloc headers per tab. Against that: a bespoke
slab allocator with in-place reuse and compaction, living inside the incremental
fold model, whose correctness needs the cache-free-oracle diff test. The priority
order (speed, correctness, CPU, memory) does not justify that for 30 µs on an
operation measured in milliseconds.

**What shipped from the attempt**: the two counters, and one trap recorded in the
header for whoever tries `SmallVector` here anyway. These word types are nested in
`FoldingModel`, and a nested class's default member initialisers are parsed only
once the outermost class is complete — so inside `FoldingModel`'s body
`is_default_constructible_v<WordCloser>` cannot be answered and GCC answers
`false`, failing `SmallVector`'s static_assert with a false negative whose error
message says nothing about the real cause.

The original entry follows.

Found while closing [TD-2026-08-06-139](#td-2026-08-06-139), which bisected a +472
allocation drift to the incremental fold model and then found the cost was entirely
in the block partition built at file open.

```cpp
struct Block {                          // one per ~256 lines
  ...
  std::vector<WordCloser> bracket_closers;
  std::vector<WordOpener> bracket_openers;
  std::vector<WordDedent> indent_dedents;
  std::vector<WordIndent> indent_openers;
};

// The walk state at a block boundary, sliced out of shared pools so a document
// with thousands of blocks does not hold thousands of small vectors.
struct PrefixState { std::uint32_t bracket_offset, bracket_count, ...; };
```

`PrefixState` is the next struct in the file and it already carries the answer in
its own doc comment. `Block` did not get the same treatment, so a 50k-line document
(~195 blocks) pays a few hundred 32-byte allocations to build the partition, and
holds four vector headers plus four heap blocks per block for the life of the tab.
Measured: **+472 allocations** on the `editor_mouse_selection_drag` fixture, all at
open.

The build side is already pooled — `AppendBlockWord` fills four
`build_*_` scratch members and then `assign`s them into the block, which is four
allocations away from being free. What is missing is the storage side: four
model-owned pools plus `(offset, count)` per block.

**Why this is low priority and not a bug.** The cost is one-time per file open and
buys the removal of an O(document) fold rescan from every keystroke — the trade the
priority order names, in the right direction. ~472 allocations is ~20 µs against a
file open measured in milliseconds.

**The one thing that makes it non-trivial**, and the reason it is filed rather than
done: blocks are rebuilt *individually* (a keystroke rebuilds the one block it
lands in), and a rebuilt block's word can change size, so pooled slices need either
in-place reuse when the new word fits plus append-and-compact when it does not, or
a fixed per-block capacity. That is a small allocator, and it needs the
cache-free-oracle diff test (see the fold notes in
`dev-docs/performance/performance-findings.md`) run against it, not a second
`FoldingModel`.

### TD-2026-08-06-145 — a mouse-drag rebuilds the editor pane layout three times per motion event. [RESOLVED 2026-08-06 — and the drag phase now allocates nothing at all.]

**What shipped.** Option (a), then the two allocations it exposed underneath.

*(a) The pane layout is off the heap.* The group count is a structural cap, not a
data property, so `EditorGroupRectsLayout::groups`, `EditorPaneLayouts` and
`EditorSplitDividerLayouts` became `util::InlineVector<T, N>` — fixed capacity,
all slots value-initialised, no heap fallback, no staleness risk. The cap itself
is now one definition, `workspace::kMaxEditorGroups`, shared by the surface split,
the per-group tab-strip caches and every inline capacity, so raising it cannot
leave one of them behind. All seventeen call sites got the fix, not just the drag.

*(b) was not needed.* With (a) in, the memoisation the entry ranked as "strictly
better and strictly riskier" buys only the arithmetic — three float splits per
event — against the risk it named (a missed generation bump serving a stale pane
rect, i.e. a click landing in the wrong editor group). Not worth it. The entry's
own advice held.

*The other two per move were a different bug.* Attributing them needed the trace
scoped to the measured phase, which the tracer could not do — a scenario's setup
out-allocates its phase by an order of magnitude and the phase's own sites were
not in the printed top twelve at all. `MICROIDE_PERF_ALLOC_TRACE_PHASE=<substring>`
fixes that (see `dev-docs/performance/perf-harness.md`), and named both in one
run:

- `ConsumePendingRenderInvalidation` did `const RenderInvalidation result =
  pending_;` — a **copy** where a move was meant, so the rect list was built twice
  and freed twice on every handled event, app-wide.
- `RequestRedrawRect`'s `std::vector` push. The buffer cannot be kept across
  events because it is handed to the caller inside `EventResult`, so the first
  push allocates every time to carry 16 bytes.

`RenderInvalidation::rects` is now `util::SmallVector<SDL_FRect, 8>` — inline
storage **with** a heap spill, unlike `InlineVector`, because damage rects are
genuinely unbounded and dropping one paints stale pixels. Eight is measured, not
guessed: two new counters (`workspace.redraw_rects_queued`,
`workspace.redraw_rect_spills`) report the input path's redraw work, which nothing
did before — a hover repainting seven controls and one repainting one read
identically in every other counter. `menu_hover_switch` queues 7 rects per event,
which is what moved the inline size from 4 (160 spills, every event) to 8 (zero).

| | at HEAD | after (a) | after (b) |
| --- | ---: | ---: | ---: |
| `editor_mouse_selection_drag` measured phase | 960 | 320 | **0** |
| `editor_mouse_selection_drag` p50_allocations | 5,482 | 1,402 | **1,075** |
| `menu_hover_switch` p50_allocations | 8,352 | — | **8,104** |
| `typing_large_file` p50_allocations | 366 | — | **350** |

**One thing to reuse.** `InlineVector` and `SmallVector` are both in `src/util`
and they are not interchangeable: the first is for a cap that is a design fact
(exceeding it is a bug), the second for a size that is usually small but
genuinely unbounded. Picking the first where the second belonged is how a
container silently drops data. `SmallVector` is restricted to trivially copyable
elements on purpose — that is what keeps growth a raw copy with no lifetime or
exception-safety window — so it does **not** serve
[TD-2026-08-06-144](#td-2026-08-06-144), whose `Block` holds `std::string`s.

The four scenarios above are now gated against baselines 4-5x looser than the code
they gate; see [TD-2026-08-06-147](#td-2026-08-06-147).

The original entry follows.


Measured at HEAD with `MICROIDE_PERF_ALLOC_TRACE=32:32` on
`editor_mouse_selection_drag`: the measured phase is **960 allocations for 160
mouse moves — six per move, every one exactly 32 bytes**, allocated and freed
inside the phase. Four of the six resolve to one call, reached three times per
motion event:

```
WorkspaceShell::ComputeEditorPaneLayouts(SDL_FRect const&)   WorkspaceShellEditorSplits.cpp:119
  <- EditorMouseCoordinator::HandleSelectionMotion            WorkspaceEditorMouseCoordinator.cpp:568
  <- WorkspaceShell::CurrentFocusedEditorRedrawRect           WorkspaceShellRedraw.cpp:607
       <- WorkspaceShell::RequestFocusedEditorRedraw          WorkspaceShellRedraw.cpp:258
  <- the coordinator's pane-layout callback                   WorkspaceEditorMouseCoordinator.cpp:671
```

Each call allocates twice: `ComputeEditorSurfaceGroupRects` returns an
`EditorGroupRectsLayout` holding a `std::vector<EditorGroupRects>`, and
`EditorPaneLayoutsFromGroupRects` returns a `std::vector<EditorPaneLayout>`. Both
are almost always **one element** — the common case is a single editor group — so
this is two heap round-trips per call to carry one struct.

This is **not** new and not the TD-2026-08-06-139 drift: the phase measured
960 allocations at every commit across that entry's 90-commit window. It is
pre-existing per-input-event work on a path where the whole point is that a drag
tracks the pointer.

Sixteen more call sites take the same function (`WorkspaceShellHoverTargets.cpp`
alone has four; `WorkspaceShellMouse.cpp`, `WorkspaceShellCursor.cpp`,
`CaretRedraw.cpp`, `WorkspaceShellRedraw.cpp` have the rest), so a fix here is
worth more than the drag.

Two shapes, in increasing risk:

- **(a) Stop allocating.** The vectors are 1-2 elements in every real
  configuration. An inline-capacity small vector, or `*Into(rect, out&)` overloads
  writing into reusable shell scratch, removes the heap traffic without changing
  when anything is computed. No staleness risk.
- **(b) Stop recomputing.** Memoize on `editor_surface` plus a layout generation
  bumped by `editor_groups` / `focused_group_index` / banner-visibility changes.
  Strictly better, and strictly riskier: a missed bump serves a stale rect, and
  a stale pane rect is a click landing in the wrong editor group.

Start with (a). It is the whole allocation win and none of the staleness risk, and
it leaves (b) available with a measurement to justify it.

### TD-2026-08-06-146 — ccache + `-flto=auto` ICEs, so an A/B over history skips commits. [RESOLVED 2026-08-06 — the walk recovers and says so, and the cache stopped thrashing.]

**Two fixes, because the entry named two different costs.**

*1. The walk no longer silently loses a commit.* `tools/perf-compare.py` matches
the ICE signature in its own build log (`original not compressed with zstd`,
`lto1: internal compiler error`), retries the build **once** with
`CCACHE_DISABLE=1`, and prints what it did in both directions — that it worked
around a toolchain fault rather than measuring a broken commit, and that the
commit is fine. Silence there would have been the whole defect restated one layer
up: the point is not that the retry succeeds, it is that the numbers this side
produced are attributable.

*2. The cache stopped thrashing, which is where the corruption pressure came
from.* `ccache -sv` on this workstation before the fix:

```
Hits:                                    10016 / 164477 ( 6.09%)
Uncacheable calls:                      214295 / 378952 (56.55%)
  Could not use precompiled header:     213864 / 214295 (99.80%)
Cache size (GiB):                          5.0 /    5.0 (99.76%)
Cleanups:                                27408
Errors:                                    180
  Input file modified during compilation:  180
```

**56 % of every compile in this tree bypassed the cache entirely**, because ccache
refuses a PCH-using translation unit unless told to tolerate
`pch_defines,time_macros` — and that was exported by `tools/run-checks.sh` only.
A bare `cmake --build`, an IDE, `perf-compare.py`, a bisect script: all missed.
The sloppiness now lives in the build itself (`CMAKE_CXX_COMPILER_LAUNCHER`
becomes `cmake -E env CCACHE_SLOPPINESS=… ccache`), so every caller gets it. The
launcher is not written into `compile_commands.json`, so clangd is undisturbed.
`max_size` was still at the stock 5 GiB despite `CLAUDE.md` telling everyone to
raise it — 27,408 cleanups against 7,916 live files is a cache evicting itself
continuously — and is now 20 GiB on the reference runner.

**What the evidence actually supports, stated narrowly.** The GCC message comes
from `ZSTD_getFrameContentSize` rejecting an LTO section that is not a valid zstd
frame, which is what a truncated or torn object file looks like. The 180
`Input file modified during compilation` errors are direct evidence of source
churning underneath a running build on this box (see the standing rule in
`validation-traps.md` about editing during a build), and a cache at 99.8 % capacity
with 27k evictions is the condition under which a torn entry is most likely to be
both written and read. That is a coherent account, not a proven root cause — which
is exactly why fix 1 exists: it does not depend on the diagnosis being right.

The original entry follows.

Bisecting [TD-2026-08-06-139](#td-2026-08-06-139) hit this on roughly one commit in
four:

```
lto1: internal compiler error: original not compressed with zstd
lto-wrapper: fatal error: /usr/bin/c++ returned 1 exit status
```

It is a ccache/LTO interaction (a cached object stored under different compression
settings than the one the LTO link expects), not a source problem: the same commit
builds clean with `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` or with ccache
disabled. Both the `microide-perf` preset and `tools/perf-compare.py` build with
LTO on, so **any** walk over history is affected, not just a hand-rolled bisect.

Why it matters more than an occasional retry: a `git bisect run` script reports
these as `skip` (exit 125), and a bisect that skips a quarter of its candidates can
converge on a range rather than a commit, or on the wrong commit if the real one is
in the skipped set. Nothing about the failure says "this is your toolchain" — it
looks like the commit is broken.

Worked around for that bisect by configuring with
`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` and **verifying both window endpoints
re-measured identically** (5,024 / 5,496 with and without LTO), which is the check
that makes the workaround honest: an allocation count does not depend on inlining
decisions, but that has to be shown rather than assumed for the metric being
bisected. A wall-time bisect cannot use this workaround.

Options: pin `CCACHE_COMPRESS`/`CCACHE_COMPRESSLEVEL` consistently, add
`-fno-fat-lto-objects` handling to the ccache config, disable ccache for LTO
configurations, or have `perf-compare.py` retry once with the cache bypassed and
say so. Worth one session; the cost today is silent unreliability in the one tool
used to attribute a regression to a commit.

### TD-2026-08-06-147 — the pane-layout fix left four allocation gates 4-5x loose. [RESOLVED 2026-08-06 — full-suite rebaseline, 69 allocation gates tightened.]

**What shipped.** One full-suite `microide_perf --iterations=10
--reference-runner=perf-runner-v1 --update-baseline` pass, bare, on
perf-runner-v1, exactly as the entry asked. 99 of 100 baselines rewritten
(`editor_moby_dick_workout` is opt-in and did not run).

It found far more than the four scenarios the entry named. **69 allocation
baselines were loose, every single one of them, and not one had drifted the other
way** — 51,488 allocations of slack removed from the committed set:

| scenario | was | now | gate was this loose |
| --- | ---: | ---: | ---: |
| `menu_hover_switch` | 8,910 | 54 | **165x** |
| `menu_popup_hover_rows` | 3,015 | 74 | **41x** |
| `editor_mouse_selection_drag` | 2,101 | 1,035 | 2.0x |
| `cold_startup_no_project` | 166 | 101 | 1.6x |
| `scroll_large_file` | 1,375 | 899 | 1.5x |
| …64 more | | | 1.05-1.2x |

The long tail matters as much as the headline. Sixty-four gates sitting 5-20%
loose is a suite that cannot see a 5-20% regression anywhere, which is most of
what an allocation gate is for.

**The pre-flight check the entry demanded, done before the pass, not after.**
`--update-baseline` rewrites every tolerance from the `Scenario` struct, so a
hand-widened envelope living only in the JSON vanishes silently. Audited all 100
committed files against the tolerances the code would write — resolving the named
constants (`tolerance::kJitterWallP95` = 250, `kJitterWallMax` = 400), which is
the step that makes the audit mean anything — and **every committed tolerance was
already expressed in code**. Nothing was lost.

**How the run was certified, given the box was not quiet.** Another session held
an interactive process pinned to cpu15, inside the harness's own affinity set
(0-3, 12-15). Rather than guess, the run was judged by the instrument built for
exactly this: `harness.cpu_calibration_ns`, recorded per iteration. 25 scenarios
showed a within-run probe spread ≥1.10x (up to 2.00x) — individual iterations
getting preempted — but the per-scenario *medians* held at 481-506k ns across the
whole suite, and p50 wall/cpu are medians. Allocation counts, the metric this
entry exists for, are deterministic and cannot move for this reason at all.

**And then it was checked rather than argued.** A second, completely independent
full-suite gate run against the newly written baselines: **99 PASS, 0 FAIL**. A
rebaseline that produces a gate the next run cannot hold is the documented failure
mode (memory: `perf-baseline-drift-and-iteration-count`); this one holds.

Two things fell out of the verification run:

- Every new baseline records `"iterations": 10`, so
  [TD-2026-08-06-148](#td-2026-08-06-148)'s short-run guard is now live across the
  suite instead of inert. Zero `NOT ENFORCED` notes at the default count, which is
  the guard behaving.
- `typing_large_file`'s resident baseline went 80,555 → 83,285, which is 148's
  item 1: the gate had been spending half its envelope on drift.
- One gate came back at 94% of its envelope, and the headroom instrument said so
  in the run that passed it. Filed as
  [TD-2026-08-06-150](#td-2026-08-06-150) — it is a real property of that
  scenario, not this pass.

The original entry follows.

[TD-2026-08-06-145](#td-2026-08-06-145) cut `editor_mouse_selection_drag` from
5,482 `p50_allocations` to 1,075 and moved three other scenarios. The committed
baselines still carry the pre-fix numbers, so those gates now permit a **complete**
regression back to the old behaviour and stay green:

| scenario | committed baseline | measured after 145 | slack |
| --- | ---: | ---: | ---: |
| `editor_mouse_selection_drag` | 5,482 | 1,075 | **5.1x** |
| `menu_hover_switch` | 8,352 | 8,104 | 1.03x |
| `typing_large_file` | 366 | 350 | 1.05x |
| `menu_popup_hover_rows` | 2,529 | 2,529 | — |

Not rebaselined here on purpose. The committed set was recorded from a full-suite
run, and standalone numbers differ from in-suite ones for reasons that are a lead
rather than noise (TD-2026-08-06-139 measured an 680-allocation offset from
process state alone on one scenario) — so writing four standalone numbers into a
set recorded in-suite makes those four inconsistent with the other 89. It wants
one full-suite `--update-baseline` pass on a quiet box, bare, not under xvfb.

Note `--update-baseline` rewrites tolerances from the `Scenario` struct, so any
hand-widened envelope in the four above is reset by the pass — check before, not
after.

### TD-2026-08-06-148 — `typing_large_file`'s RSS gate rides its envelope, and the verdict depends on `--iterations`. [RESOLVED 2026-08-06 — item 2 fixed; item 1 folded into 147's rebaseline.]

**What shipped.** The second option the entry offered, not the first: a baseline
now records the iteration count it was captured over, and a run shorter than that
does not gate `mean_rss_growth_bytes`.

Making the trim *proportional* was considered and rejected, because it does not
work. The series settles rather than carrying outliers — per-iteration growth
decays — so a mean over iterations `1..N-1` falls with `N` no matter what fraction
is trimmed. Worked through on a harmonic decay, dropping the top 25% instead of the
top 1 gives 0.2375A at N=6 and 0.1565A at N=10: still a different number, so the
verdict would still depend on `--iterations`. Nothing short of a tail-only
statistic is iteration-count-stable, and that throws away the coverage the trimmed
mean exists for (a scenario that retains on *some* iterations —
TD-2026-08-05-136). Declining an incomparable comparison is the honest move.

Four parts:

- `BaselineRecord::iterations`, written as `"iterations"` at the root of the
  baseline JSON. Omitted rather than zeroed when unknown, so a baseline predating
  the field gates exactly as it did.
- A short run reports `mean_rss_growth_bytes` and annotates it
  `NOT ENFORCED: this run averaged 6 iterations against a baseline recorded over
  10 …`. `MetricComparison` grew `enforced` + `note` for this; an unenforced metric
  never turns a scenario red, is excluded from the headroom ranking (it has no
  envelope to consume), and is recorded in `--report-json` — a report that carries
  only `passed` cannot tell a gate that held from one that was declined.
- The note prints on **passing** verdict lines too. A longer-than-baseline run
  stays gated (it can only read low) and gets its own `resident gate is loose` note.
- `--update-baseline` now refuses `--iterations` below 10 outright, the same way it
  refuses the GPU and windowed lanes. A baseline recorded short bakes the settling
  passes into every metric and records p95s the gate can never hold
  (perf-baseline-drift-and-iteration-count).

**One thing found on the way, and it was the bigger hole.** `tools/perf-compare.py`
— the vs-main oracle, the tool the whole repo is told to trust over the committed
baselines — never reported `mean_rss_growth_bytes` at all. Its merge step
recomputed p50/p95/max from the concatenated iterations and silently dropped the
mean, so the A/B was blind to a resident regression *in exactly the metric the gate
enforces*, and TD-2026-08-05-136 had already established that the percentiles it
did report are unstable (1.76x across three runs of one binary) and sometimes
outright blind (p50 of 0 on a scenario retaining ~972 KB an iteration). Now
computed and reported first among the resident rows.

Item 1 — the baseline sitting ~13% below what the code does — is a stale number,
not a mechanism, and is fixed by [TD-2026-08-06-147](#td-2026-08-06-147)'s
full-suite rebaseline. That pass is also what puts an `"iterations"` count into the
committed set; until a baseline is re-recorded the new guard is inert on it, by
design.

The original entry follows.

`mean_rss_growth_bytes` for `typing_large_file` has a baseline of **80,555** and a
+25% envelope (100,693). Measured at HEAD, five runs a side, on the same quiet
box:

| `--iterations` | readings |
| --- | --- |
| 6 | 99,942 / 100,762 / 101,581 / 113,050 / 113,869 — **fails ~half the time** |
| 10 (default) | 83,740 / 91,477 / 92,388 / 93,753 / 95,118 — passes with ~6% margin |

Pre-existing: the same split reproduces at `08b7f338`, before the
[145](#td-2026-08-06-145) work, so this is not that change. Both sides of a
controlled A/B overlap heavily (base 83.7-93.8k, after 91.9-95.1k at 10
iterations).

Two things are wrong here, and only one of them is the stale baseline.

1. The baseline is ~13% below what the code actually does, so the gate is spending
   half its envelope on drift and has ~6% left for a real regression.
2. **A non-default `--iterations` changes the verdict.** `mean_rss_growth_bytes`
   is a trimmed mean (TD-2026-08-05-136), and at 6 iterations the trim leaves more
   of the settling passes in. Nothing warns about this: a developer who runs
   `--iterations=6` to save time gets a red gate that means nothing, and the
   failure message says "measured=113869 (+41%)" with no hint that the sample size
   is the reason.

The second is the one worth fixing — either make the trim proportional so the
metric is iteration-count-stable, or refuse to gate `mean_rss_growth_bytes` below
the iteration count its baseline was recorded at and say so.

### TD-2026-08-06-149 — a menu-bar hover rebuilds the whole menu-bar layout ~10 times per motion event. [RESOLVED 2026-08-06 — the hover phase now allocates nothing at all.]

**What shipped.** Option (a) only, and it was the whole win.

| | at `34bcb510` | after |
| --- | ---: | ---: |
| `menu_hover_switch` measured phase (160 moves) | 8,000 | **0** |
| `menu_hover_switch` p50_allocations | 8,104 | **54** |
| `menu_popup_hover_rows` p50_allocations | 2,529 | **74** |

The phase is not "mostly fixed", it is zero: every allocation the 160 hover events
made was one of the three vectors, so removing all three removed all fifty per
event rather than the ~32 the trace attributed to the top function. The two extra
call chains the entry listed were carrying the other eighteen through the same
containers.

- `VisibleMenuBarItem` and its two containers (`VisibleMenuBarItems`,
  `MenuBarOverflowIds`) moved to `WorkspaceMenuRegistry.h` as
  `util::InlineVector<…, kMaxMenuBarItems>`. The registry already owns `MenuId`
  and the spec table, so the cap and the containers sized from it now sit
  together, with a `static_assert` next to the table — adding a sixteenth menu
  past the cap would otherwise have been dropped from the bar in a release build
  and asserted only in debug. `ComputeVisibleWindowControlButtons` returns
  `InlineVector<…, kWindowControlButtonCount>`, asserted against the array it
  fills.
- The label-width memo lost its `unordered_map<const char*, float>`. `MenuSpecs()`
  is a static table whose order never changes, so the spec's POSITION is a valid
  key and a free one; the map was eight hash lookups per call and ~80 per motion
  event. The label pointer is still compared per slot, so a table that did change
  re-measures instead of serving a stale width.

**(b) was not needed, and now there is an instrument to say so.** Two counters,
`workspace.menu_bar_layouts` and `workspace.menu_bar_label_measures`. The first
confirms the entry's headline from an ordinary run — 1,610 layouts across 160
hover events, ~10 an event — and the second is the check that the width cache is
holding (it should read zero after the first frame; a layout that stopped
allocating but started re-measuring would otherwise be invisible). With the
allocations gone, memoising the layout buys arithmetic against the risk the entry
named: a stale menu-bar rect is a click opening the wrong menu. If it is ever
worth revisiting, `menu_bar_layouts` is the recompute rate a memo has to beat, and
it is now reported rather than needing an allocation trace to find.

**Coverage.** `WorkspaceShell/MenuBarLayoutIsAllocationFree` is the unit-level
oracle: a hundred warm layouts, zero heap allocations (under the perf-harness
build), zero label re-measures, and the layout counter must move — the scenario
gate measures the same thing end to end but only against a baseline that can
drift, which is exactly how the previous optimisation pass on this function left
the vectors behind.

**One measurement note.** The four scenarios re-run after this change reported
wall failures of +100% to +1000% while their allocation counts fell 20-150x. That
was a second Claude session compiling at `-j24` on the same box, and the harness
said so itself without being asked: `harness.cpu_calibration_ns` spread 739-9778us
(13.2x) on the same run. Allocation counts being *down* across a clock that moved
that far is the tell (memory: `cpu-ms-measures-the-governor`,
`perf-gate-video-lane`). Only the allocation counts above are claimed from that
run; they are deterministic and do not depend on the clock. Wall is re-measured on
a quiet box by [TD-2026-08-06-147](#td-2026-08-06-147)'s rebaseline pass.

The original entry follows.

The same shape as [TD-2026-08-06-145](#td-2026-08-06-145), one subsystem over,
and an order of magnitude worse. Found by pointing 145's own instrument
(`MICROIDE_PERF_ALLOC_TRACE_PHASE`) at the next interactive scenario.

Measured at `34bcb510` with
`MICROIDE_PERF_ALLOC_TRACE=1:1000000 MICROIDE_PERF_ALLOC_TRACE_PHASE=menu_hover_switch`:

```
phase menu_hover_switch.160_moves: 8,000 allocations, 678 KB, 160 hover events
                                   -> 50 allocations per motion event
```

That is the **measured phase**, not the scenario total (8,104) — the distinction
that made 138's and 139's headlines wrong, so it is stated here rather than left
to be re-derived.

**Every one of the printed top-12 sites — 20,480 of the traced allocations —
bottoms out in the same function**, `ComputeVisibleMenuBarItems`
(`WorkspaceShellMenu.cpp:33`), reached from at least three independent chains per
event:

```
ComputeVisibleMenuBarItems(SDL_FRect const&)              WorkspaceShellMenu.cpp:33
  <- ComputePopupMenuRect <- CurrentChromeRedrawRect <- RequestChromeRedraw
  <- the chrome coordinator's menu-bar-items callback   WorkspaceChromeMouseCoordinator.cpp:590
       <- ChromeMouseCoordinator::HandleMenuMotion
  <- CursorKindForPosition <- UpdateMouseCursor <- HandleMouseMotion
```

~32 of the 50 allocations per event are that one function. Eight further call
sites take it (`WorkspaceShellMenu.cpp:108`/`:275`, `WorkspaceShellCursor.cpp:120`/
`:324`, `WorkspaceShellRenderChrome.cpp:62`, two test-access helpers), so as with
145 the fix is worth more than the one scenario.

**Why it allocates three times per call.** It returns
`std::vector<VisibleMenuBarItem>`, builds a `std::vector<std::pair<MenuId, float>>
measured` (with a hard-coded `reserve(8)`), and calls
`ComputeVisibleWindowControlButtons`, which returns another vector. Every one of
them carries a handful of PODs — `VisibleMenuBarItem` is 24 bytes — and the item
count is bounded by the static `WorkspaceMenuSpecs()` table, i.e. **a structural
cap, exactly the precondition `util::InlineVector` exists for**. The coordinator
callback is even declared `std::function<std::vector<VisibleMenuBarItem>(const
SDL_FRect&)>`, byte-for-byte the pattern that
`compute_editor_pane_layouts` was before 145.

Note what is *already* cached here and was not enough: the function memoises
per-label `MeasureWidth` in a thread-local map keyed by label pointer ("round-4
Finding 6", with a metrics-generation invalidation beside it). Someone has
measured this path before and fixed the width lookups while leaving the three
vectors — which is the reason to state the remaining cost in allocations rather
than in wall time.

**Two shapes, same as 145, and take (a) first for the same reason:**

- **(a) Stop allocating.** `InlineVector` for all three, capacity from a shared
  constant derived from the menu-spec table, plus `*Into(rect, out&)` for the
  window-control buttons. No behaviour change, no staleness risk, and it is most
  of the win.
- **(b) Stop recomputing.** The menu bar's layout only changes on window resize,
  layout-mode switch, font-metrics change, or a menu opening/closing — far less
  often than a pointer moves. Memoising it is the bigger win and carries 145(b)'s
  risk in a worse place: a stale menu-bar rect means a click opening the wrong
  menu. Do it only with a measurement that (a) did not already cover.

**Reproduce** (seconds, exact oracle):

```bash
MICROIDE_PERF_ALLOC_TRACE=1:1000000 MICROIDE_PERF_ALLOC_TRACE_PHASE=menu_hover_switch \
  ./build/microide-perf-make/microide/microide_perf \
    --scenarios=menu_hover_switch --iterations=3 --report-json=/tmp/menu.json
# phase_metrics.menu_hover_switch.160_moves.allocations is the number to move.
```

Gating note: `menu_hover_switch`'s committed allocation baseline is 8,352 against
a measured 8,104, so the gate will register this fix — unlike the four scenarios
in [TD-2026-08-06-147](#td-2026-08-06-147). Watch the *phase* number regardless;
the scenario total buries a 50-per-event change in setup noise.

### TD-2026-08-06-150 — the resident gate is a coin flip for a scenario that retains on *alternate* iterations. [RESOLVED 2026-08-06 — it was process state, and there was a deterministic instrument sitting unused.]

**The entry's step (1), done, and it answered the question.** Per-iteration
`rss_growth_bytes` for `diff_stage_selected_lines`, one unchanged binary, five
process states:

| what ran before it | trimmed mean |
| --- | ---: |
| nothing (solo, three runs) | 33 / 37 / 61 KB |
| a 26-scenario prefix | 79 KB |
| a 24-scenario prefix | 265 KB |
| the full 52-scenario prefix | 239 KB |
| the whole suite (the entry's two runs) | 174 / 273 KB |

Solo is stable, and the entry said what that means: **it is process state, and the
fix is scenario isolation, not the envelope.** It is not one culprit scenario
either — the response is cumulative and dose-dependent, which is heap
fragmentation, not a leak upstream. Nor does it converge: at 40 iterations behind
a fragmenting prefix the scenario retains ~185 KB per iteration indefinitely,
against ~21 KB solo.

**What the entry did not anticipate: the app's behaviour is identical in every one
of those states, and the harness was already recording the proof and discarding
it.** `bytes_allocated - bytes_freed` across the measured window reads **28,470
bytes, exactly, in all five**. Re-measured across three different suite prefixes
for every scenario that runs in them: **52 of 52 reproduced to the byte, worst
spread 9 bytes.** The 8x swing is glibc failing to trim a heap that 61,761
allocations per iteration have fragmented differently depending on who fragmented
it first — a fact about the allocator, not about staging a hunk.

**So the gate split in two rather than being widened.**

- **`p50_net_heap_bytes` is the new deterministic retention gate** (10 % envelope,
  4 KiB floor for the zero case, `Scenario::tolerance_net_heap_percent`, written
  into every baseline). It is the gate a retention regression is now expected to
  trip, and it is prefix-independent, machine-independent and clock-independent.
  It also sees what RSS cannot: `reference_snippet_file_window` nets 1.65 MB per
  measured window while `mean_rss_growth_bytes` reads exactly **0**.
- **`mean_rss_growth_bytes` keeps a job the new metric cannot do** — growth that
  never went through `operator new` (mmap, SDL, Lua, PCRE2, libc) — and gets the
  envelope that job needs: 150 % on the three git-workstation scenarios. The entry
  rejected widening because it would "hide the thing worth understanding". The
  thing is now understood and measured, and the byte-exact gate carries the signal
  the wide one no longer can.

**Deliberately NOT done: full per-scenario process isolation.** It is the complete
fix — it would make every metric a property of the scenario, and would also remove
TD-2026-08-06-139's 680-allocation process-state offset — but it is a fork-per-
scenario harness rewrite plus a whole-suite rebaseline, and the deterministic gate
buys the accuracy without either. Filed as
[TD-2026-08-06-152](#td-2026-08-06-152).

**Predicted and then observed.** The entry said "the next ordinary run flips it
red". The next ordinary full run did exactly that:
`diff_stage_selected_lines mean_rss_growth_bytes: expected=174308 actual=291271
(+67.1%, tolerance +60%)` — the only resident failure in the suite, with every
allocation gate green. Under the new envelope it passes, and its
`p50_net_heap_bytes` was 28,470 on that run too.

The original entry follows.

Found by [TD-2026-08-06-147](#td-2026-08-06-147)'s verification run, and found the
way it is supposed to be found — the headroom instrument reported it on a gate
that **passed**:

```
[perf] headroom (duration/resident): 1 gate(s) passed while consuming >=75% of their envelope.
[perf]   diff_stage_selected_lines mean_rss_growth_bytes: baseline=174308 measured=272612
         (+56.4% of +60% allowed = 93.99% of envelope)
```

Two consecutive full-suite runs of **one unchanged binary**, per-iteration
`rss_growth_bytes`:

| run | series | trimmed mean |
| --- | --- | ---: |
| rebaseline | 7405568, 610304, 0, 335872, 0, 217088, 0, 200704, 0, 204800 | 174,308 |
| verify | 7245824, 843776, 0, 434176, 0, 405504, 0, 397312, 0, 372736 | 272,612 |

`diff_stage_hunk_large_patch` does the same thing (190,692 → 267,605).

**The alternation is not the problem, and this is the part worth getting right.**
Both runs alternate identically — retain, zero, retain, zero — so the *shape* is
deterministic and the trimmed mean handles it exactly as
[TD-2026-08-05-136](#td-2026-08-05-136) intended (p50 would report **0** here,
which is why it was abandoned). What moved is the *magnitude per retaining
iteration*: ~200-340 KB in one run and ~370-434 KB in the next, near-uniformly
across every retaining iteration. That is not a sampling artifact of the
statistic; something about the allocator's or the diff pipeline's steady state
settles differently per process. Two possibilities and no evidence yet for either:
arena layout carried in from whichever scenarios ran before it in the suite
(TD-2026-08-06-139 measured a 680-allocation offset from process state alone), or
a genuine bimodal retention the fixture can land in.

Why it matters now rather than later: the rebaseline happened to record the low
draw, so the gate is at 94% of a +60% envelope and the next ordinary run flips it
red. A red gate that means nothing is how a suite stops being read.

**Deliberately not fixed by widening the tolerance.** 60% already does not cover a
1.56x swing, and picking 100% would hide the thing worth understanding. The order
of work: (1) run these two standalone and in-suite and diff the per-iteration
series — if standalone is stable, it is process state and the fix is scenario
isolation, not the envelope; (2) if it is bimodal within one process, the
statistic needs the mode reported, not averaged. Reproduce with:

```bash
./build/microide-perf-make/microide/microide_perf \
  --scenarios=diff_stage_selected_lines,diff_stage_hunk_large_patch \
  --iterations=10 --report-json=/tmp/stage.json
# scenarios[].iterations[].rss_growth_bytes is the series; the summary hides it.
```

### TD-2026-08-06-151 — the five biggest interactive scenarios time an inner loop but never declare it as a measured phase. [RESOLVED 2026-08-06 — and the first trace it made possible took 30% off every editor scroll scenario.]

**What shipped.** The five named scenarios plus all three second-tier candidates
(`file_finder_cold`, `switch_and_idle`, `search_first_result`) now wrap their
existing timed region in `ScenarioContext::Measure`. Mechanical, no behaviour
change, and compatible with the assertion as the entry predicted: the inner
per-frame `chrono` samples still feed `EnforceP95Microseconds` while `Measure`
adds the phase entry. `samples_us.reserve` stays deliberately outside the phase —
that buffer is the harness's, and one 96-double allocation charged to the phase
would be the largest thing in an otherwise-empty trace. The eight scenario totals
moved by **+2 allocations** each (the phase record itself), and all eight still
pass.

**The entry was right to claim a measurement gap and not waste, and right to
refuse to guess which.** The answer was one of each:

| phase | allocations | share of the scenario total |
| --- | ---: | ---: |
| `editor_fold_viewport_refresh.scroll_frame` | 31,079 | 84 % |
| `editor_sticky_scroll_scroll.fast_scroll_frame` | 31,806 | 82 % |
| `editor_render_whitespace_paint.scroll_overlay_frame` | 26,500 | 82 % |
| `editor_indent_guides_paint.scroll_paint_frame` | 24,700 | 80 % |
| `editor_scroll_only_no_content_bump.scroll_frame` | 22,650 | 81 % |
| `switch_and_idle.switch_and_settle` | 19,158 | 38 % |
| `file_finder_cold.open_finder` | 41,622 | 67 % |
| `search_first_result.search_to_first_result` | **145** | **0.7 %** |

The five scroll scenarios were mostly measuring what they claimed, so the fear
that drove the entry (setup burying the phase, as on
`editor_mouse_selection_drag`) did not hold for them. It held completely for
`search_first_result`, whose baseline comment calls its 20,207 allocations "the
authoritative, precise regression signal" for a search — 20,062 of which are
opening a 10k-file project and building its index. That gate would not notice the
search doubling.

**And then the instrument earned its keep on the first use.** With the filter
finally aimable at `editor_fold_viewport_refresh.scroll_frame`, **11 of the top 12
sites were the same call**: `std::filesystem::path::lexically_normal`, reached from
`EditorBlameOverlayService::BuildEditorOverlay` inside `RenderClip`. The inline
blame overlay resolves the same (project root, file path) pair three to four times
on **every painted frame** — its own eligibility check, then `GitBlameService`'s
`Request` and `Snapshot` — and one `lexically_normal` is ~12 allocations.

Memoized, which is correct by construction rather than by convention:
`lexically_normal` and `lexically_relative` are purely lexical, touch no
filesystem, and are therefore pure functions of their arguments — no invalidation
hook, no generation counter, nothing to go stale against. Four entries, thread-local
so it needs no lock (the shell thread paints while the background executor runs
git), sized for a split editor plus a concurrent sidebar refresh rather than the
one-entry memo that would thrash to a 0 % hit rate exactly when there is most to
save.

| scenario | phase before | phase after | total before | total after |
| --- | ---: | ---: | ---: | ---: |
| `editor_fold_viewport_refresh` | 31,079 | 22,151 | 37,147 | **25,955** |
| `editor_sticky_scroll_scroll` | 31,806 | 22,506 | 38,665 | **27,101** |
| `editor_render_whitespace_paint` | 26,500 | 19,060 | 32,211 | **22,693** |
| `editor_indent_guides_paint` | 24,700 | 17,260 | 30,741 | **21,223** |
| `editor_scroll_only_no_content_bump` | 22,650 | 13,350 | 28,079 | **16,515** |

~30 % of every editor scroll scenario's allocations, on the hottest interactive
path in the product, and none of it was reachable before the phase existed.

The original entry follows.

[145](#td-2026-08-06-145) and [149](#td-2026-08-06-149) were both found the same
way: point `MICROIDE_PERF_ALLOC_TRACE_PHASE` at an interactive scenario's measured
phase, read the top sites, fix the container. Both times the phase turned out to
be ~100% removable (960 → 0, 8,000 → 0). The obvious next targets are the largest
interactive scenarios left — and the instrument cannot be aimed at any of them.

**7 of 93 registered scenarios call `ScenarioContext::Measure`.** The phase trace
arms *only* inside that call (`PerfHarness.cpp:480`), so a scenario that never
calls it has no phase to filter on:

```
$ MICROIDE_PERF_ALLOC_TRACE=1:1000000 \
  MICROIDE_PERF_ALLOC_TRACE_PHASE=editor_sticky_scroll_scroll \
  ./build/microide-perf-make/microide/microide_perf --scenarios=editor_sticky_scroll_scroll
[alloctrace] WARNING: no measured phase name contained "editor_sticky_scroll_scroll"
             — the table below is empty because the filter never matched, not
             because nothing allocated
```

**Credit where due: that warning already exists**, so this is not the silent
empty-trace trap — the tooling says exactly what is wrong. There is simply
nothing to point it at.

The five that matter, with their committed `p50_allocations` — **scenario totals,
which is the whole problem; none of these is a phase number**:

| scenario | p50_allocations (total) |
| --- | ---: |
| `editor_sticky_scroll_scroll` | 38,663 |
| `editor_fold_viewport_refresh` | 37,145 |
| `editor_render_whitespace_paint` | 32,210 |
| `editor_indent_guides_paint` | 30,740 |
| `editor_scroll_only_no_content_bump` | 28,077 |

Each opens the 50k-line C++ fixture, pumps 20 frames, and (for the fold ones)
builds a folding model before its scroll loop, so an unknown share of those counts
is setup. On `editor_mouse_selection_drag` the setup out-allocated the measured
phase by roughly an order of magnitude, which is exactly why 145 needed the phase
filter built before it could attribute anything.

**What makes this cheap: the region is already delimited, just not to the
harness.** All five hand-roll a `std::chrono` loop around precisely the frames
they care about and feed it to `EnforceP95Microseconds`:

```cpp
for (int i = 0; i < 100; ++i) {
  const auto t0 = std::chrono::steady_clock::now();
  context.Scroll(-3);
  context.PumpFrames(1);
  samples_us.push_back(/* t1 - t0 */);
}
EnforceP95Microseconds("editor_sticky_scroll_scroll.fast_scroll_frame", samples_us, 30'000.0);
```

Wrapping that loop in `context.Measure("editor_sticky_scroll_scroll.scroll_frames",
[&]{ … })` is mechanical, changes no behaviour, and is compatible with the
existing assertion — the inner per-frame `chrono` samples still feed
`EnforceP95Microseconds`, while `Measure` adds the phase entry that
`--report-json` and the tracer need. It also makes the per-frame allocation cost
gate-visible in `phase_metrics`, which is the number a scroll regression would
actually move.

**Do not blanket-add it to all 86.** For a pure-unit scenario
(`user_config_record_decode`, `dap_protocol_encode_decode`, …) the whole run *is*
the work and the total is already the right number; adding a phase there is noise.
The ones worth phasing are those with expensive setup in front of a repeated
interactive action — the five above, plus `file_finder_cold` (61,805),
`switch_and_idle` (50,419) and `search_first_result` (20,190) as second-tier
candidates.

**This entry claims a measurement gap, not waste.** Whether those loops allocate
anything worth removing is unknown, and saying otherwise from a scenario total is
the mistake [138](#td-2026-08-06-138) and [139](#td-2026-08-06-139) already made
once. Phase them first, then read the trace, then decide.

### TD-2026-08-06-152 — every perf metric is a property of the suite, not of the scenario, because all 93 run in one process. RESOLVED 2026-08-07.

Split out of [150](#td-2026-08-06-150), whose fix works around this rather than
removing it, and it is the same defect [139](#td-2026-08-06-139) hit from the
other side.

`PerfHarness::RunScenario` runs every scenario's warmup and measured iterations in
the one `microide_perf` process, in registration order. So a scenario's numbers
carry whatever the ~50 scenarios before it left in the process:

- **Resident growth, 8x.** `diff_stage_selected_lines` reads a trimmed mean of
  33 KB per iteration solo and 265 KB behind a 50-scenario prefix, with its actual
  retention byte-identical in both (150). The committed baseline is therefore only
  comparable against a run whose prefix is byte-for-byte the same suite — so
  *adding, removing or reordering a scenario silently re-levels every downstream
  resident gate*, and the only signal is a red gate on unrelated code.
- **Allocations, 680.** 139 measured a 680-allocation offset from process state
  alone. Allocation counts are the suite's deterministic oracle; they are
  deterministic *given the prefix*, which is a weaker claim than the one the gate
  is read as making.

The fix is one child process per scenario: fork before any SDL or thread
initialisation (the parent must stay a pure driver, since fork in a multithreaded
process is not safe), run that scenario's iterations, serialise the `Aggregate`
back over a pipe. Every metric then means what its name says, and the whole class
of "which scenario ran before this one" goes away — including 139's offset and
150's 8x.

**Cost, honestly.** It is a harness rewrite plus a whole-suite rebaseline: both the
resident numbers (which will drop to their solo values) and, per 139, the
allocation counts will move. That rebaseline needs the reference runner quiet, and
the run that writes it must be re-gated against itself before it is trusted
(memory: `perf-baseline-drift-and-iteration-count`).

**Why it can wait.** 150 shipped `p50_net_heap_bytes`, which is prefix-independent
by construction and is now the gate a retention regression trips. What remains
unfixed is that the RSS gate cannot be tight and that allocation counts have an
unmeasured prefix sensitivity — real, but no longer the only instrument in the
room.

**Done 2026-08-07.** `RunScenarioInChildProcess` forks per scenario and brings
the `Aggregate` back over a pipe. The parent never initialises SDL, the shell or
a thread — every scenario runs in a child that does all of that from scratch —
which is what makes the fork safe, and the child `_exit`s so no atexit handler or
static destructor of the parent's runs twice. Unselected scenarios are skipped in
the parent rather than costing a fork whose child would decline them, so a
single-scenario run still launches one process, not a hundred. `--no-isolate`
restores the shared-process form for attaching a debugger or profiler.

The wire is length-prefixed little-endian binary, not JSON, because a gate
compares doubles against doubles with a percentage tolerance: a value that lost
its last mantissa bit on the way back would read as a real move.
`ScenarioProcessIsolation/AggregateSurvivesTheWireExactly` pins that with values
chosen to break a lazy codec (a denormal, -0.0, a `uint64` at its maximum, a
negative `net_heap_bytes`, an empty phase name, a phase name with an embedded
NUL), and `...RejectsATruncatedStream` pins that a short or corrupt stream is
refused rather than decoded into a half-populated Aggregate that would then be
gated as if it were a measurement.

The allocation tracer dumps in the child before it exits, so
`MICROIDE_PERF_ALLOC_TRACE` output is unchanged.

**The rebaseline this forced is the interesting part** — see the commit that
re-records the suite. Every scenario now starts cold, so wall p95/max and
`p50_net_heap_bytes` move on scenarios that used to inherit a warm allocator and
a warm page cache from whatever ran before them. That is the number becoming
true, not a regression.

### TD-2026-08-06-156 — the finder deep-copies up to 512 result rows on every keystroke to render about twenty of them. [RESOLVED 2026-08-10 — and neither of the two exits this entry proposed was needed.]

Found by `file_finder_type_query`, the scenario added with
[155](#td-2026-08-06-155) to cover the finder's interactive path for the first
time. Ten keystrokes over a 10,000-file project cost **13,140 allocations**, and
the phase-scoped tracer put **4,134 of them on one line** —
`FileFinder::Refresh()` materialising `FileFinderResult`s — plus the same count
again in `std::filesystem::path::_M_split_cmpts` underneath it.

The cause is structural: ranking is allocation-free (it walks views into the
candidate blob), but every keystroke then deep-copies the top `kMaxResults` =
**512** rows, while the overlay renders about twenty.

**Fixed: the `std::filesystem::path` per row.** `FileFinderResult::relative_path`
was read by exactly one function, `SelectedPath()`, which runs once when the user
picks a file — and cost two allocations per row (the path's own string, and the
component split) on every keystroke. Dropped; `SelectedPath()` builds the path
from `path_string` on demand.

| phase | before | after | |
| --- | ---: | ---: | ---: |
| `file_finder_type_query.type_and_rank` | 13,140 | 4,872 | −63 % |
| `file_finder_type_query.backspace_rescan` | 5,087 | 1,941 | −62 % |
| `file_finder_cold.open_finder` | 1,621 | 597 | −63 % |

**Still there: one `std::string` per row per keystroke**, ~490 allocations for a
broad query. Two ways out, both with a real cost:

  - Make `path_string` a `std::string_view` into the candidate blob. Blocked
    today by lifetime, not by design: **five call sites outside the finder**
    (`WorkspaceShellProjectSearch`, `WorkspaceSidebarCoordinator`,
    `WorkspaceProjectStateCoordinator`, `WorkspaceShellProjectChanges`,
    `WorkspaceShellRedraw`) call `InvalidateIndexCache()`, which clears the blob
    and does **not** clear `results_`. Every one would have to be an operation
    that either clears results too or cannot run between a Refresh and a paint.
  - Cap the materialised set at what the overlay can show plus a scroll margin,
    and materialise more on demand. `kMaxResults` is 512 because the list is
    scrollable; the ranked refs (index + score, no allocation) are already kept
    uncapped, so the rows could be built lazily from them.

Neither is urgent now that the per-keystroke cost is a third of what it was, and
`file_finder_type_query` gates both phases exactly (p50 == max on every
iteration), so a regression here is now visible.

**Resolved 2026-08-10 by a third option this entry did not consider**, and it is
the shape [159](#td-2026-08-06-159)'s tail pass had already named three times:
the rows were `clear()`ed and re-`push_back`ed, so the cost was not *copying* 512
strings, it was **freeing 512 and allocating 512 more**. Overwriting each slot
with `assign` reuses the buffer that is already there.

    file_finder_type_query.type_and_rank        4,672 ->   538   -88.5 %
    file_finder_type_query.backspace_rescan     1,861 ->   288   -84.5 %
    file_finder_cold.open_finder                  577 ->    66   -88.6 %

The one subtlety is the tail: `results_` keeps its rows past the live count
rather than resizing down, because a **backspace grows the list back** and a
resize would free exactly the buffers the next keystroke needs. `results()`
therefore returns a `std::span` over the live prefix — the vector's own size is
the high-water mark, not the answer — which is the change the 45 call sites saw.

So the cap stays at 512 and the list stays scrollable (no `string_view` lifetime
change, no smaller cap), because materialising it costs nothing after the first
refresh. Both proposed exits traded a real property away to avoid a cost that
turned out to be avoidable outright.

### TD-2026-08-06-155 — a perf scenario appended to a fixture in the repository 1,361 times, and every diff scenario reading that tree had been measuring the accumulation. [RESOLVED — append leak 2026-08-06; the index mutation 2026-08-10, by a manifest rather than a sandbox.]

Found while auditing a +0.1 % allocation move on four git scenarios during the
2026-08-06 rebaseline — the kind of drift the "investigate up" rule exists to
catch, and it was not noise.

`ScenarioContext::SimulateExternalFileChange` appends to a file **in the fixture
tree** and nothing ever undid it. Two scenarios use it, and on this checkout:

```
git_large_diff_project/src/large.cpp     1,361 appended "// external refresh diff" markers
git_many_conflicts_project/current.cpp   1,337 appended "// external refresh merge" markers
```

The generator writes `src/large.cpp` with a **3-line** worktree delta. It had a
**2,725-line** one. Every scenario that opens that file's diff —
`diff_next_hunk_large_file`, `diff_stage_hunk_large_patch`,
`diff_stage_selected_lines`, `external_change_refresh_open_diff` — was measuring
a diff whose size is *a function of how many times the suite had ever been run on
that checkout*, and their committed baselines had been ratcheting up with it, one
run at a time, each increase far too small to trip a gate.

It also means those numbers were never portable: a fresh checkout measures the
3-line diff, not the 2,725-line one.

Fixed by recording each touched file's size before the first append and
truncating back after every iteration — measured and warmup alike, and on the
exception path — outside the measured window. Verified by running both scenarios
for ten iterations and confirming the fixture still reports 3 insertions.

Regenerated the fixtures and re-recorded the four scenarios, which is what the
accumulation had been hiding:

| scenario | polluted | clean | |
| --- | ---: | ---: | ---: |
| `diff_next_hunk_large_file` | 81,372 | 75,789 | −6.9 % |
| `diff_stage_hunk_large_patch` | 62,450 | 56,922 | −8.9 % |
| `external_change_refresh_open_diff` | 61,759 | 56,208 | −9.0 % |
| `diff_stage_selected_lines` | 60,834 | 55,306 | −9.1 % |

**The index half, closed 2026-08-10 by the other end.** The append was the
unbounded case, not the only one. `diff_stage_hunk_large_patch` and
`diff_stage_selected_lines` run `git add` against the fixture repository, so they
mutate its **index** — bounded (staging the same hunk twice is idempotent) but
still shared state a scenario leaves behind for every later run, and the harness
cannot restore it without encoding each fixture's intended index state
(`git_large_diff_project` wants nothing staged; `git_large_staged_project` wants
800 files staged, which is its whole point).

This entry proposed making the fixture tree a read-only input and copying it into
the per-run sandbox — a 1,000-file copy per scenario, moving every git baseline.
What shipped instead ([172](#td-2026-08-10-172)) encodes the intended index state
where it belongs, in the generator, and enforces it from the manifest: a git
fixture's `.sha256` covers the worktree **and** `git status --porcelain`, so a
left-behind `git add` invalidates the tree and `--ensure` regenerates it before
the next run. No per-scenario copy, no baseline movement, and the generator is
now the single statement of what each repository's index is supposed to look
like. What it does not cover is pollution *within* one run — scenario A stages,
scenario B measures it, in a fixed order — which the sandbox copy would have. If
that ever matters, the sandbox is still the answer; nothing measured today says
it does.

The residue from before the append fix was still on disk and is cleaned up in
172; regenerate with
`python3 tests/perf/generate_git_workstation_fixtures.py --ensure` (or just run
ctest, which does it).

### TD-2026-08-06-153 — `search_first_result`'s "authoritative, precise regression signal" is 99.3% project open. [RESOLVED 2026-08-06 — 114 phase gates armed across 82 baselines.]

**What shipped.** Both items, in the order the entry demanded.

(1) Every measured phase now carries its own allocation gate. `Aggregate` and
`BaselineRecord` carry a `phases` list (`p50_allocations`, `max_allocations`,
`p50_wall_ms`, `iterations`), `--update-baseline` writes it, and
`CompareToBaseline` adds one `phase[<name>].p50_allocations` metric per recorded
phase under a `phase_alloc_p50_percent` envelope that inherits the scenario's
resolved allocation tolerance. Allocations only: a 2 ms phase's wall is this
runner's jitter. Repeats of one name within an iteration SUM before the
percentile, so a per-frame phase in a loop is gated on what the iteration cost
rather than on whichever call landed last.

Two deliberate asymmetries, both about vacuity:

  - A baseline phase the run does **not** measure FAILS, loudly, with the reason.
    A renamed or deleted `Measure` call would otherwise remove a gate in silence.
  - A measured phase with no baseline is **reported and not enforced** — one
    `N measured phase(s) NOT GATED (...)` note per verdict line. Adding a phase
    must not turn a run red, but it must not be invisible either.

(2) The `search_first_result` comment now says what the measurement supports: the
oracle is the phase, and the total is a coarse backstop for the setup around it.

**Armed.** The suite was rebaselined whole on perf-runner-v1 and re-gated against
what it wrote: **100 PASS, 0 FAIL, zero NOT GATED notes**, with **114 phase gates
across 82 baselines** (the other 19 are pure-unit scenarios with no `Measure`
call, where the total already is the phase). `search_first_result` now gates its
search at **145 allocations** — p50 145, max 146 across ten iterations, so the
phase is as deterministic as the total it was hiding inside.

The rebaseline also found what a phase-blind suite could not: four git scenarios
had been measuring an ever-growing fixture, which is [155](#td-2026-08-06-155).

### TD-2026-08-06-153 (original entry)

Found by [151](#td-2026-08-06-151) the moment the scenario got a measured phase,
and it is exactly the mistake [138](#td-2026-08-06-138) and 139 made, sitting
undetected in a baseline comment that claims the opposite:

```
search_first_result.search_to_first_result   145 allocations
search_first_result (scenario total)      20,192 allocations
```

The comment on the registration says the median 20,207 is "a fully deterministic
… authoritative, precise regression signal" and widens p95/max so the tight p50
allocation gate can be the oracle. It is a precise, deterministic gate on
**opening a 10k-file fixture and building its index** — the search itself is 145
allocations, 0.7 % of it, and could double or decuple without moving the number
by more than rounding.

The same reading applies, less severely, to `file_finder_cold` (67 % phase) and
`switch_and_idle` (38 %).

Two things to do, and the order matters: (1) gate `phase_metrics` allocations, not
just the scenario total — the phase numbers are recorded in `--report-json` today
and nothing compares them to anything; (2) then rewrite the baseline comment,
which is currently a claim the measurement does not support. Doing (2) first would
just move the untested claim.

### TD-2026-08-06-154 — the file finder's cache rebuild costs 4 allocations per indexed file, and the deep copy it starts from is not needed at all. [RESOLVED 2026-08-06 — 41,622 allocations became 1,621.]

**Measured, on the same 10,000-file fixture the entry was written from:**

| | before | after |
| --- | ---: | ---: |
| `file_finder_cold.open_finder` (phase) | 41,622 | **1,621** |
| `file_finder_cold` (scenario total) | 61,807 | **21,806** |

That is 96 % of the phase and 65 % of the scenario, and it took two changes
rather than the one the entry proposed.

**(1) The snapshot copy is gone, and so is the method.** `FileIndex` grew
`VisitRelativePaths(mode, visit)`, which walks its own paths under the shared
lock and returns the version it observed — nothing is materialised at all. The
entry's suggestion, `SnapshotPathsWithVersion()`, would have swapped one
per-file allocation for another: its cache bucket is built lazily, and the
finder's rebuild is triggered by exactly the version change that invalidates it,
so the bucket would have been rebuilt (one `std::filesystem::path` per file)
every time the finder needed it. Zero-copy only reads as zero-copy when the
cache is already warm, which here it never is.

`SnapshotWithVersion()` and `FileIndexSnapshot` are deleted: the finder was the
only caller, and leaving a deep-copying accessor in place is how the next
consumer pays the same cost.

Two behaviour changes fall out, both fixes. The visit filters in-flight
atomic-write staging temps (the raw file list did not, so a save landing during
a rebuild could leave a phantom row in the finder until the next full rescan),
and it is explicitly `IncludeHidden`, because a `.gitignore` or a
`.github/workflows/*` is a file people open — matching what the raw list did and
what VSCode shows.

**(2) `CachedFileEntry` owns no strings.** The remaining three per-file
allocations were the entry's own two `std::string`s plus the path's `.string()`.
The path bytes and their fold now live back to back in two blobs
(`path_blob_`, `lower_blob_`) and the entry is five 32-bit offsets — so a
rebuild is a handful of geometric growths whose capacity the NEXT rebuild
reuses, and `path.native()` (POSIX) hands the bytes over without a copy. The
per-keystroke scan also stopped walking 20,000 separate heap nodes for what is
now two contiguous buffers.

The entry's item (2) said to read `file_finder_cache_build_calls` from a real
session before deciding whether to restructure the entry. That measurement is
still worth having, but it stopped being the deciding input once the
restructure turned out to be a strictly smaller, faster, and more contiguous
representation rather than a trade.

**Instrumentation, since the entry was found by measurement and the finder had
blind spots.** `search.file_finder_cache_bytes` (what the candidate cache
retains — previously invisible), plus `file_finder_refresh_calls`,
`candidates_scanned`, `mask_rejects` and `narrowed_refreshes`, which together
say how much the forward-typing narrowing and the presence-mask prefilter are
actually worth per keystroke.

### TD-2026-08-06-154 (original entry)

Found by pointing the phase tracer at `file_finder_cold.open_finder`, the phase
[151](#td-2026-08-06-151) declared. Measured, per rebuild, on the 10,000-file
fixture — six consecutive iterations, byte-identical after the cold pass:

```
phase allocations   41,622      phase bytes   2,938,719      entries built  10,000
```

**Four of the top sites are 40,000 of those 41,622 (96 %), and each is exactly one
allocation per indexed file:**

| site | per file | bytes/iteration |
| --- | ---: | ---: |
| `FileIndex::SnapshotWithVersion()` — the `std::filesystem::path` copy | 1 | 1.04 MB |
| `path.relative_path.string()` | 1 | 210 KB |
| `util::Utf8CaseFold(path_string)` → `Utf8CaseFoldInto` | 1 | 310 KB |
| `FileFinder::Refresh()` (fourth site, same shape) | 1 | 210 KB |

**The first one is free to remove and is the largest.** `SnapshotWithVersion()`
returns `FileIndexSnapshot{version, std::vector<ProjectFile>}` by **deep copy,
under the index's shared lock** — and `ProjectFile` carries a
`std::filesystem::path`, so that is one heap allocation per file before the finder
has done any work of its own. `FileFinder::EnsureCacheBuilt` reads **only**
`path.relative_path`; it never touches `ProjectFile::mtime` or `::size`. And the
zero-copy alternative already exists one method below it:
`SnapshotPathsWithVersion()` returns a `shared_ptr<const vector<path>>`, whose own
comment says "consumers can iterate without copying".

`FileFinder` is the **only** caller of `SnapshotWithVersion()` in `src/`. So the
deep-copying overload exists for exactly one caller, which does not need the two
fields that force the copy.

**What triggers it — and this is the part to get right, because two entries have
already been wrong here.** This is **not** a per-Ctrl+P cost, and it is not
per-keystroke either. `EnsureCacheBuilt` is version-gated (`cache_ready_ &&
cached_index_version_ == index_->version()`), and that gate was added deliberately:
its comment records that paying the O(index) copy per character typed "was the
finder's dominant interactive cost on large repos". A previous pass already fixed
the per-keystroke case. What remains is **per index-version change**, paid
synchronously on the shell thread at the next `Refresh()`.

The scenario measures it on every iteration because it is built to
(`file_finder_cold` waits for the index to reach its last file so the rebuild lands
inside the measured window every time), which is honest for a *cold* finder and is
why the number is stable — but it means the scenario total cannot be read as
"what Ctrl+P costs in a warm session".

**What is not known**, and should be measured before ranking this above other
speed work: how often the index version actually moves in real use. A watcher
firing during a build, a git checkout, or a save all bump it, and the latency-
sensitive case is a version change landing *while the finder is open and the user
is typing* — a 41,622-allocation rebuild inside one keystroke. `search.file_finder_
cache_build_calls` already exists and would answer it from an ordinary session.

Order of work: (1) switch to `SnapshotPathsWithVersion()`, which is the largest
single site, needs no new machinery, and removes a deep copy taken under a lock;
(2) read `file_finder_cache_build_calls` from a real session before deciding
whether the remaining three per-file allocations are worth restructuring
`CachedFileEntry` for.

### TD-2026-08-05-137 — `cpu_ms` is a duration, and nothing normalised it against the machine's clock. [RESOLVED 2026-08-06.]

`idle_soak_30s` failed the gate at `p50_cpu_ms: baseline=14.6955 measured=29.8665
(+103.236%, tolerance +100%)` while wall, allocations, RSS and its zero-wake
assertion all passed, and while **the same binary passed the same gate on two other
full runs**.

Reproduced, with the new `harness.cpu_calibration_ns` probe alongside:

| it | polls | soak_cpu_ms | cpu_ms | calib_us |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 2920 | 4.67 | 37.71 | 670.7 |
| 2 | 391 | 4.09 | 13.58 | 674.3 |
| 3 | 3836 | 4.16 | 14.42 | 673.6 |
| 4 | 1280 | 3.85 | 13.62 | 716.3 |
| 5 | 1900 | 3.95 | 15.54 | 676.8 |
| 6 | 479 | 9.25 | 30.47 | **860.8** |
| 7 | 2229 | 10.69 | 30.05 | **857.2** |
| 8 | 1461 | 8.55 | 25.23 | **856.6** |
| 9 | 3401 | 10.57 | 28.44 | **857.4** |
| 10 | 2188 | 10.31 | 31.60 | **859.9** |

One run, one clean step. The calibration probe is a fixed slab of dependent
integer work timed outside the measured window, so it can only move when the
machine does — and it steps 671 → 857 us at exactly the boundary where `cpu_ms`
steps 14 → 30 ms. Every application counter is byte-identical across the step,
allocations are 2956 on both sides, and the poll count is uncorrelated (479 polls
→ 9.25 ms; 3836 polls → 4.16 ms). The governor walked the clock down mid-run.

**Why this scenario and not others.** It sleeps for 27 of its 30 seconds, which is
what lets the core idle down to the 605 MHz floor — 8.5x below its 5157 MHz
ceiling. Its whole CPU budget is ~18 harness frames at ~0.83 ms each, two or three
of them rendered on a core still climbing back, so the p50 lands wherever the
governor did. Rebaselining cannot fix it; no percentage envelope is both stable
and meaningful against a 2x machine-state swing.

**What shipped.** CPU across the soak window is now measured and asserted directly
against a 20 ms budget (measured 3.85–10.69 ms across the clock step), exactly like
the zero-wake assertion beside it, and the iteration-level CPU gate is dropped for
this one scenario via `Scenario::gate_cpu_metrics`. Plus `warmup_iterations = 1`,
since the fixture-open pass (watcher's ~1000 coalesced events, full file-index
build) measured 32–51 ms and owned the max and p95.

**Still owed, and why this is filed rather than closed:**

1. ~~The same reasoning applies to every scenario whose iteration is mostly sleep or
   mostly waiting. `repo_open_rss_idle` already shows the symptom — its calibration
   swings 671–1352 us *between iterations of one run*. Nobody has audited which
   other CPU gates are measuring the governor.~~ **[ADDRESSED 2026-08-05 — the audit
   is now a byproduct of any run rather than a sweep somebody has to schedule.]**
   `MeasureCalibrationSpread` / `DescribeCalibrationSpread` (`tests/perf/Baseline.h`)
   fold the probe into min/max/ratio across a scenario's measured iterations, and
   `PerfMain` appends the note to that scenario's verdict line whenever the ratio
   clears `kCalibrationSpreadNoteRatio` (1.10). So a full run *labels* every
   scenario whose clock moved under it, and a failing duration metric carries the
   same note inline. Attached to `cpu_*`/`wall_*` lines only: an allocation or RSS
   gate does not scale with the clock, and identical allocation counts across a
   step are the evidence the duration failure is the machine. Covered by
   `PerfBaseline/CalibrationSpreadFlagsAMovingClock`, which pins the reproduction's
   own 671→857 us numbers, that ordinary ~1% drift stays silent, and that a run
   with no probe describes nothing rather than a 1x range of zeros.
2. ~~`cpu_calibration_ns` is now *read* — but only to explain a failure, not to
   prevent one. The principled fix is still to normalise `cpu_ms` (and arguably
   `wall_ms`) by it before comparing to a baseline.~~ **[DONE 2026-08-06.]** A
   baseline records `p50_cpu_calibration_ns` — the clock it was captured at — and
   `CompareToBaseline` re-expresses the run in that machine state before checking
   the envelope. Details that are load-bearing:

   - **Per iteration, against that iteration's own probe reading**, then
     percentiled. The motivating failure was a clock that stepped *mid-run*; a
     single per-run factor would smear it across both halves.
   - **CPU only.** Allocations and RSS do not scale with the clock — their coming
     out identical across a step is the *evidence* a duration failure is the
     machine. Wall stays raw too: a scenario that sleeps for 27 of its 30 seconds
     has a wall time that is mostly not work, and scaling it would be arithmetic
     on a number that does not scale.
   - **Clamped at 3x either way, loudly.** A factor that far out is a broken probe
     or a baseline from another machine class; scaling a gate by it quietly is how
     a gate goes vacuous. A clamped comparison stays enforced at 3x and says so on
     the verdict line.
   - **Only when the baseline recorded a clock**, so pre-format baselines compare
     raw exactly as before rather than dividing by a zero.

   Covered by `PerfBaseline/NormalisesCpuAgainstTheBaselineClock` (with the
   negative control: the same rise at an unchanged clock still fails),
   `NormalisesEachIterationAgainstItsOwnClock` (the reproduction's own 671→857 us
   step passes, a regression inside its slow half still fails),
   `ClampsAnAbsurdClockFactor`, `ClockNormalisationTouchesCpuOnly`, and
   `CalibrationRoundTrip`.
3. ~~The probe understates the effect… A calibration workload shaped like the thing
   being measured would read truer.~~ **[DONE 2026-08-06 — measured, and the
   answer is no.]** Two shapes were built and both rejected on their own numbers:

   - **Memory-mixed** (the chain plus an L2 pointer chase plus a 256 KiB streaming
     copy) reads *dirtier*, not truer. Its memory half swung **3.3x** across one
     scenario's iterations (632 → 192 us) while the ALU chain held to 1%, because
     the probe's buffers are evicted by whatever the scenario just did. That makes
     the reading a function of the code under test: a change that grows the working
     set inflates the probe, which scales the expectation up, which loosens the
     very gate that change should have tripped. It also streams ~12 MB immediately
     before the measured window, evicting the app's own working set.
   - **Burst-shaped** (the same work in short slabs, each after a 1 ms sleep, to
     time a core that has just been idle) read **within 1%** of the plain chain on
     every iteration of every scenario tried. 1 ms is nowhere near long enough for
     the core to leave the state it is in.

   So the probe stays a pure dependent integer chain, and branch-free /
   allocation-free / **memory-free** are now load-bearing properties rather than
   incidental ones: this number scales a gate, so anything the code under test can
   move must stay out of it. The 1.10 note threshold stands unchanged.

   What the machine actually does, measured while settling this: a thread working
   **continuously for about a second** reaches a state where the probe reads ~467 us
   instead of ~673 us — **1.44x** — and holds it while it keeps working. 300 ms of
   idle drops it back, no amount of later spinning recovers it inside a run with
   idle in it, and a busy keeper thread pinned to another physical core does not
   hold it either. It is per-thread residency, not a package state the harness can
   pin. So frame-pumping scenarios live in the slow state permanently, continuous
   ones cross into the fast state *part way through a run*
   (`syntax_highlight_cpp_lines`: 252 us for nine iterations then 179 us for five,
   with `cpu_ms` tracking it 15.6 → 11.3 ms), and two scenarios in one run are not
   necessarily measured on the same machine. That is the case per-iteration
   normalisation exists for, and it is why the "sleep-heavy scenarios only"
   framing above was too narrow.

`idle_soak_30s` keeps `gate_cpu_metrics = false`: normalisation would have absorbed
its 2.0x step down to about +56% against a +100% envelope, but a gate that passes
by 44 points of an envelope is not a measurement, and the direct 20 ms budget
assertion across its soak window is a better instrument than a percentage of a
percentile. The opt-out is now a genuine last resort rather than the standard
answer to a noisy CPU gate.

Related: [TD-2026-08-05-136](#td-2026-08-05-136) is the same class of defect on a
different metric — a gate whose value is decided by machine state rather than by
the code.

### TD-2026-08-05-135 — four gated perf baselines are 40-80% looser than the code they gate. [RESOLVED 2026-08-06.]

Found while rebaselining after TD-2026-08-05-133. Running the full gate showed
several scenarios far under their committed baselines; an **interleaved A/B against
the base commit** (same preset build, same lane) showed they measure *identically
on base and HEAD*:

| scenario | committed baseline | measured, BOTH sides |
| --- | ---: | ---: |
| `editor_toggle_comment_large_selection` | 185,486 | 105,600 |
| `editor_surround_multi_caret` | 121,787 | 79,864 |
| `editor_shaping_multi_caret` | 103,880 | 57,362 |
| `merge_next_conflict_large_file` | 12,552 | 7,779 |

So each of those gates has 40-80% of slack in the metric the harness calls its
oracle. A regression of that size would pass silently, which is the same failure
mode as an architecture lint that cannot fire — the run is green and the green
means nothing.

Deliberately NOT folded into the TD-2026-08-05-133 rebaseline: mixing pre-existing
drift into an unrelated change's numbers makes both unreadable. It wants its own
sweep — A/B every gated scenario against the previous rebaseline point, rebaseline
what has drifted, and work out *when* it drifted, because a baseline that loosens
by 45% without anyone noticing is a process gap, not a number.

The generalizable part is already in `dev-docs/project/validation-traps.md`:
**a committed baseline is not a proxy for main.** "Improved against the baseline"
and "improved against the previous commit" are different claims, and only the
second one is about your change.

**[RESOLVED 2026-08-06 — swept, and the sweep found more than the four.]** All
four reproduce exactly as filed and are rebaselined; the whole-suite sweep found
nine more allocation gates carrying drift and six wall gates at 0.54-0.66 of their
committed number:

| scenario | committed allocations | measured | ratio |
| --- | ---: | ---: | ---: |
| `editor_shaping_multi_caret` | 103,880 | 57,362 | 0.55 |
| `editor_toggle_comment_large_selection` | 185,486 | 105,600 | 0.57 |
| `merge_next_conflict_large_file` | 12,552 | 7,779 | 0.62 |
| `editor_surround_multi_caret` | 121,787 | 79,852 | 0.66 |
| `git_sidebar_activate` | 624 | 534 | 0.86 |
| `merge_edit_result_then_scroll` | 17,698 | 15,484 | 0.87 |
| `terminal_scroll_long_output` | 7,207 | 6,492 | 0.90 |
| `first_line_edit_latency_large_file` | 15,476 | 14,636 | 0.95 |
| `editor_smart_indent_typing` | 14,706 | 14,123 | 0.96 |
| `editor_snippet_expand` | 781 | 754 | 0.97 |
| `editor_fold_recompute` | 14,016 | 13,709 | 0.98 |

Wall, same sweep: `git_sidebar_refresh_large_repo` 0.54, `commit_open_with_large_staged_set`
0.57, `diff_open_1000_file_changes` 0.59, `git_sidebar_refresh_many_untracked` 0.63,
`diff_stage_hunk_large_patch` 0.63, `diff_stage_selected_lines` 0.66.

**And the sweep found drift in the other direction, which is the part a blind
rebaseline would have buried.** Five scenarios measured OVER their committed
allocation baseline; they are recorded at their measured values so the next move in
that direction starts from a number somebody looked at, and the unexplained rise
itself is filed as **[TD-2026-08-06-139](#td-2026-08-06-139)**. The rule that keeps
this honest is now in the harness doc: **rebaseline down, investigate up.**

**On "when did it drift":** part of the wall column is not code at all. Grouping
the sweep's scenarios by the calibration probe they were measured at, the ten that
ran at ~675 us have a median wall ratio of 1.08 against 0.98 for the 86 that ran at
~482 us — the same machine, the same run, a 1.4x clock difference decided by
whether the scenario before them kept the core busy (see
[TD-2026-08-05-137](#td-2026-08-05-137)). That is why the CPU gate now normalises
against the recorded clock; extending that to wall is
**[TD-2026-08-06-140](#td-2026-08-06-140)**.

The process gap the entry names is real and NOT fixed by a sweep: nothing reruns
the gate on a schedule, so drift accumulates silently until somebody rebaselines.
Filed as **[TD-2026-08-06-141](#td-2026-08-06-141)**. What this pass adds is that a
rebaseline now records the machine state it was captured in, so the next sweep can
tell drift from the governor without guessing.

### TD-2026-08-05-136 — `editor_long_line_select_all_edit`'s resident-growth gate is a coin flip. [RESOLVED 2026-08-06.]

Its `rss_growth_bytes` is **bimodal**: per iteration it lands on either 4.19 MB or
6.29 MB, stably, with no compounding across 20 iterations, and the p50 reports
whichever mode occurred more often. The same binary passed and failed the 25%
envelope on different runs.

It is not this scenario's work that varies — the allocation count is stable and
went *down* over TD-2026-08-05-133, and `editor.line_materializations` /
`editor.line_materialized_bytes` are byte-identical to the base commit. Removing
the `CopyRange` reserve and the `TextBuffer::operator[]` aliasing in turn moved
neither mode. Both values are multiples of the fixture's ~2 MiB line, so this is
allocator placement of a handful of large blocks, which is exactly the weakness the
perf-harness doc records for this metric ("page granularity, allocator arena
behaviour").

**Fixed by measuring the right thing: both resident readings are now taken on a
trimmed heap.** `SettleResidentSet()` (`malloc_trim(0)`) runs at both iteration
boundaries, outside the measured window, so it costs neither wall nor CPU. The
delta then means "what this iteration RETAINS" rather than "what this iteration
retains plus whatever its allocator happened to be caching at the sample point".

Measured: the scenario reports p50 **4,956,160**, p95 **7,033,036**, max
**7,036,928** — **byte-identical across three repeated 20-iteration runs**, with 17
of 20 iterations inside a 40 KB band. The bimodality is gone, not widened around.
The 60% workaround envelope is reverted to the 25/35/60 default.

That workaround also surfaced a landmine worth more than the fix. It lived only in
the committed JSON, while `--update-baseline` rewrites every tolerance in a
baseline from the `Scenario` struct — which had no RSS fields at all. The next
rebaseline would have silently reset 60% to 25% and re-armed the coin flip, with
nothing in the diff but three numbers changing in a generated file. `Scenario` now
carries `tolerance_rss_p50/p95/max_percent` and the writer reads them: **a
tolerance that is not expressed in code is a comment.**

Two side effects of the trim, both improvements:

- Six scenarios that recorded ~0 growth now record 88-303 KB. The free list was
  absorbing their per-iteration retention; trimmed, it is visible. Their new
  numbers are what they always cost.
- The next iteration re-faults the pages it gave back. That is a real cost, now
  carried uniformly by every iteration instead of by whichever one got unlucky.

Its allocation gate stays at 1% and remains the real oracle.

### TD-2026-08-05-131 — a one-character edit walked its line 5 times, because undo history stored whole lines. [RESOLVED 2026-08-05.]

Found by adding the first perf fixture in the tree with a long line
(`editor_essentials_minified`, one ~2 MiB line). Every other editor fixture is
ordinary line-broken text — the widest line in `large_project` is 20 bytes — so
nothing measured the shape where a per-line cost becomes a per-document cost.

A single-character insert allocated **13x the affected line's bytes** in 24
allocations. Five of those were bookkeeping and were fixed first (commit
`944de6d0`), taking it to **5x in 12 allocations**. The remaining five were the
data model itself: `SliceLines` copying the pre-edit line into `before_lines`, the
composed post-edit line into `after_lines`, `PieceTree`'s replacement buffer, its
append into `add_`, and `MergeGroupEntry`'s copy of the aggregate when the typing
run coalesced.

**Fix: the undo entry has two shapes.** An edit that stays inside one line records
`{start_line, start_column, removed_text, inserted_text}`; everything structural —
line splits and joins, multi-line replaces, grouped/multi-caret aggregates,
whole-document changes — keeps the line-vector form.
`Entry::before_line_count()`/`after_line_count()` give the line arithmetic both
shapes share, so the cache-invalidation and wrapped-row splice callers do not
branch on the shape.

That removes three of the five copies outright and lets two more go with it:

- `PieceTree::ReplaceTextRange(start_line, start_column, end_line, end_column, text)`
  is the splice the tree already performed underneath, now exposed in the editor's
  coordinates. It copies only `text`, where the line-shaped `ReplaceLineRange` had
  to join the whole rebuilt line and append it to `add_`. `AppendTextRange` is its
  read counterpart, for extracting a span without materializing the line.
- The typing/backspace/delete-forward run coalesces by appending to the entry's
  two small strings.
- The encoding upgrade scans the spliced-in bytes rather than the rebuilt line —
  the companion item in the original entry. Every other byte on the line was
  classified when it entered the document, so this is exact, not an approximation.
- The caret's preferred column walks the pre-edit line to the edit point and
  continues over the replacement (`TextLayout::AdvanceVisualColumnsOver`) instead
  of re-walking a composed line.

**Measured: 5.00x the line in 12 allocations -> 1.00x in 9.** The one remaining
line-sized copy is the piece tree materializing the edited line into its
per-revision cache, which the renderer and the visual-width scan both read as one
contiguous view — it is shared, not per-caller, and removing it means teaching
both to consume the line in chunks. The visual-width scan stopped needing it in
TD-2026-08-05-132; the renderer stopped in TD-2026-08-05-133, which closed it.

**Two consumers needed real work rather than a branch.** Undo-group frames
aggregate by line range, so an in-line child is widened back to the line form on
its way into a frame — one line copy, on grouped edits only, never on a keystroke.
`TextViewport::PushHistoryEntry` is the single point where that can happen and
carries the invariant. And the layout caches took the inserted *lines* only to read
their count before re-measuring from the live buffer anyway, so they now take a
count; that is what lets an in-line entry stop carrying a materialized line at all.

**And the largest win was not in the entry model.** With the copies gone, the perf
summary named `BuildRangeHistoryEntry`'s own self time — 164 ms over 64 calls —
rather than anything it called. `TextLayout::ClampTextColumn` was rounding a byte
offset down to a code-point boundary by re-tiling the line from byte 0, an O(1)
question at O(column) cost, run four times per keystroke (two range endpoints, the
preferred column, and `PreviousTextColumn` for a backspace). UTF-8 is
self-synchronizing, so the answer is at most three bytes back. That one change took
the scenario from 461 ms to 160 ms.

**Total: `editor_typing_minified_line` 528 ms -> 160 ms (3.3x), allocations
4,970 -> 4,918.** Ordinary-file scenarios are unmoved, which is the expected shape:
these are proportional-to-line-length costs.

**Coverage.** A differential run of random in-line edits against a naive string
model with a full undo walk back and redo walk forward; the entry-shape routing
pinned directly; all five coalesce compose shapes against sequential application
(including the contained splice that typing cannot reach); non-contiguous refusal;
the character-level `AppliedEdit` with prefix/suffix trimming; encoding upgrade
from the delta alone; grouped in-line children undoing atomically. The
`ClampTextColumn` change is pinned against the old forward tiling over every offset
of every arrangement of 1–4-byte sequences, plus bounded-resync properties for
malformed bytes. `PieceTree::ReplaceTextRange` has a hand-picked span table, a
randomized equivalence run interleaved with the line form, and `PieceTreeEquivalenceFuzz`
now drives both primitives against the same model.
`editor_typing_minified_line`'s 1% allocation tolerance stays, and the byte-budget
test `TextViewport/SingleLineEditAllocatesABoundedMultipleOfTheLine` tightened from
6x to 2x. Both that and the routing test were confirmed to fail with the routing
reverted.

**Partly addressed by TD-2026-08-05-133:** `TextBuffer::operator[]` is still a
materializer, not an accessor, but it no longer makes a SECOND copy — for a line
that spans pieces it aliases the one the tree already had to make. The pair
heuristics in `TextViewportLanguageBehavior.cpp` were converted to
`editor/CaretNeighborhood.h`. `TextViewportMultiCaret.cpp` still holds ~8
`operator[]` reads that only want a length or a clamp, and no scenario measures
them with a long line — a multi-caret long-line fixture is the missing coverage.

### TD-2026-08-05-132 — on a line with no newlines in it, the render and folding paths still scan the whole line per keystroke. [RESOLVED 2026-08-05.]

**`editor_typing_minified_line` 148.7 ms -> 18.9 ms (7.9x), allocations unchanged
at 4,918.** All three filed items are done, plus a fourth the profile named once
the first three were gone. What is left after them is one thing, filed as
TD-2026-08-05-133 and since closed.

The per-keystroke self time this entry opened with, and what happened to it:

| scope | before | after |
| --- | --- | --- |
| `EditorViewRenderer::Render::RowLayout` | 0.91 ms | 0.0004 ms |
| `FoldingModel::SyncLineBracketCache` | 1.25 ms | 0.0001 ms |
| `FoldingModel::WalkLines` | 0.57 ms | 0.00006 ms |
| `TextViewport::ApplyHistoryEntry` | 0.24 ms | 0.001 ms |

**One fact answers the first two items: is this line plain ASCII?** No tab and no
byte >= 0x80 means one visual cell per byte, so on such a line visual column IS
byte column at every offset and every conversion between them is O(1). The width
table `TextLayoutCache` already keeps per line now carries that bit, packed into
the existing word (a `bool` member would double a document-sized table to carry
one bit; widths are bounded by the line's byte length, so the top bit is free).

With it:

1. **`BuildVisibleLineInto`** starts its walk at the line's first
   tab-or-multibyte byte rather than at column 0, and skips even that scan when the
   caller hands over the cached facts. Exact for every line, not a fast path for a
   special case: within that prefix there is nothing to derive.
2. **`UpdateVisualColumnCacheAfterEdit`** derives the new width from the splice. A
   plain line stays plain exactly when the inserted text is, and a plain line's
   width is its byte count — so the update reads the spliced-in bytes and nothing
   else. `ApplyHistoryEntry` has that splice for every column-scoped entry, which
   is every keystroke.
3. **The folding model drops brackets on a line past
   `kMaxBracketScanLineBytes`**, set equal to `runtime_syntax::kMaxHighlightLineBytes`
   and tied to it by a `static_assert`. The equality is the point: past the
   tokenization cap a line has no syntax tokens, so `IsSuppressedBracketAt` cannot
   tell a brace inside a string literal from a real one, and the folds derived from
   such a line were arbitrary rather than approximate. Contributing none is cheaper
   and more honest. This is the VS Code-shaped threshold this entry predicted.
4. **The caret's own visual column** was the largest remaining cost once those
   landed — `CaretForLine` runs per rendered row per frame and re-walked to the
   caret every time. Every caret conversion in the editor now routes through
   `TextViewport::VisualColumnAt` / `TextColumnAtVisualColumn`, which consult the
   same per-line facts.

**Worth carrying forward: the fourth item was invisible until a scope was put on
the thing next to it.** `PieceTree`'s line materialization is charged to whichever
consumer asks for the edited line first in a frame, so as the code around it got
faster the cost moved between scopes in the ranking and read each time as that
consumer's own. It spent this pass disguised as filetype detection. It has its own
scope and counters now (`PieceTree::MaterializeSpanningLine`,
`editor.line_materializations`, `editor.line_materialized_bytes`), and so does the
caret walk it hid (`editor.visual_column_walk_bytes`) — a walk and a lookup return
the same answer, so no correctness test can tell them apart and only a counter can.

Coverage: the visible-line build is pinned differentially against a walk from
column zero, with a tab / 2-byte / 3-byte / lone-invalid byte at every offset of
lines spanning several eight-byte scan chunks, at every scroll offset, hinted and
unhinted. The width derivation is pinned against a from-scratch measurement across
each transition that breaks its precondition. The caret conversions are pinned
against the direct walk at every column of a plain, tabbed, multibyte and empty
row, with the width table cold and warm. The fold cap is pinned at the boundary in
both directions. A debug-only cross-check in `VisibleLineLayoutRefCached`
re-measures short lines against the width table, so the "every content edit either
splices this table or drops it" invariant the whole thing now rests on cannot rot
quietly. Every one of these was confirmed to fail with its fix reverted.

### TD-2026-08-05-134 — the long-line coverage sweep: every other interaction with a file that has no line breaks in it. [RESOLVED 2026-08-05.]

TD-2026-08-05-130/131/132 all existed because no fixture in the tree had a long
line. Closing that for *typing* left the hole open for every other interaction, so
three scenarios were added first and then read: `editor_long_line_horizontal_scroll`
(96.2 -> 22.8 ms), `editor_long_line_buffer_search` (33.0 -> 17.8 ms),
`editor_long_line_select_all_edit` (22.8 -> 17.6 ms). All with byte-identical
allocation counts, which is what says the wins are algorithmic.

Two mistakes, each made in several places, and both worth recognising by shape:

**A bound in the wrong unit.** Bracket matching bounds its scan by
`max_lines_each_side`, which bounds work only if lines are bounded — so one arrow
key next to a minified bundle's closing bracket read the whole file. And its
string/comment suppression, a per-LINE question, was asked per COLUMN: 12,584,364
highlight-cache probes across 40 frames, one per byte of the document per caret
move. Fixed with a per-line cursor and `kMaxBracketMatchScanBytes` (512 KiB,
sized against the 50k-line fixture's ~326 KB 2000-line window).

**Narrowing by line, on a document with one line.** All three row match-fill loops
(regex fragments, the literal Ctrl+F cache, occurrences) binary-searched to the
current line and then resolved every span on it before clipping it away; the
occurrence *scan* likewise read every visible line end to end for a consumer that
clips to the window. All four now bound by the row's visible source-byte window.

The testing lesson is the durable part, and it is in
`dev-docs/project/validation-traps.md`: a pixel differential whose control goes
through the same production code passes when a bound is wrong the SAME way on both
sides. Three of four injected bound bugs survived that design. The check that works
computes each in-window match's cell position independently and asserts it changed
pixels against a no-matches render.

Instrumentation left behind: `EditorViewRenderer::Render::BracketMatch` and
`WorkspaceShell::Render::BuildEditorViewModel`, both of which ran every frame, were
the largest cost in their parent, and had no scope of their own.

### TD-2026-08-05-133 — the renderer needs the edited line as one contiguous view, which costs a full copy of it per keystroke. [RESOLVED 2026-08-05.]

`PieceTree::LineView` is zero-copy while a line lies inside one piece. An in-line
edit splits its line into three, so from the first keystroke on, the edited line
spans pieces and every reader of it pays a copy into the per-revision cache. The
copy is shared — the first caller in a frame pays and the rest hit the cache — so
removing it meant **no caller may want the whole line**. On
`editor_typing_minified_line` that was one ~2 MiB copy per keystroke, the single
dominant cost of typing.

**Result: 256 materializations per 8 iterations became 8** — one per file open,
none per keystroke. Traced application self time per iteration **5.16 ms →
1.30 ms (4.0x)**; allocations **4,918.5 → 4,559.5**.

**Eight callers, and the entry above named four of them wrong.** It called
`SignatureDetectHead`, the fold bracket scan, the syntax highlighter and
`MeasureIndent` "the easy half" because each already had a bound — but every one of
them applied its bound *after* reading the line. `ScanLine(lines[i], out)` skips a
line past `kMaxBracketScanLineBytes`, having first copied it. That shape —
**a bound that reads its input before rejecting it** — was the whole of the easy
half, and it is the generalizable finding: a cap on a line's LENGTH must be asked
of `LineLength`, which reads no text, not of the bytes.

The other four were the render and edit paths:

- **`TextLayoutCache`** now builds a row from `{bytes, start_byte, line_length}`.
  The window is four bytes per visual column the walk can reach — the longest UTF-8
  sequence — which is exactly tight (confirmed by injection: one byte less fails).
  Without `LineLayoutFacts` the full visual width still has to be walked, so that
  path stays whole-line.
- **`RowDecorationInput::text`** was the whole line for every row. On the editor's
  path its only consumer is diagnostic underlining, so it is empty unless the row
  carries a diagnostic; whole-line plugin decorations read a new `line_length`.
  The predicted "six consumers using `text.size()` as end-of-line" was three, and
  `SliceVisibleColumns` turned out to be reachable only from the compare/merge
  path, which still passes whole lines.
- **`FindBracketMatch`** refuses a caret line past
  `runtime_syntax::kMaxHighlightLineBytes`, the same threshold and the same
  argument the fold cap already makes: past it the line has no tokens, so a brace
  inside a string cannot be filtered and the pair would be arbitrary.
- **`Backspace` and `BuildRangeHistoryEntry`'s clamp** answer from
  `editor/CaretNeighborhood.h`, which reads the line's length plus at most nine
  bytes around the caret.

**The entry's own risk assessment was right about where the risk was and wrong
about its size.** "A real refactor of the hottest render loop in the app" is what
it took; what made it tractable is that the whole-line build is a perfectly good
oracle for the windowed one, so the differential is exact rather than approximate.

Instrumentation and traps this left behind are in
`dev-docs/performance/performance-findings.md`. Gated by
`TextViewport/TypingInALongLineCopiesNothing` (always on, counter-based, confirmed
to fail with any one caller reverted) plus `editor_typing_minified_line`.

### TD-2026-08-04-130 — repeated large multi-line edits grew the resident set without bound. [RESOLVED 2026-08-04.]

Cause: **`PieceTree`'s append-only `add_` buffer**, not heap fragmentation. The
original hypothesis in this entry was wrong, and the way it was wrong is the
reusable part.

Every insert appends its text to `add_` and nothing ever reclaims it; the only
compaction trigger was the 4 GiB offset-overflow backstop, which in practice never
fires. So `editor_toggle_comment_large_selection` — 16 toggle-line-comment
operations over a 1,000-line selection — appended ~2.7 MB per iteration forever,
and `add_` doubled itself 17 -> 35 -> 70 MB.

Three counters said "no leak" and all three were telling the truth about the wrong
thing:

- **Allocation counts stayed balanced** (to within 12) because a container
  doubling is exactly one allocation and one free.
- **`mallinfo2().uordblks` and `.arena` were flat**, because a 35 MB block is
  served by mmap, not the sbrk arena.
- **`malloc_trim(0)` returned nothing**, because the memory was live, not free.

What did find it, in about a minute: `/proc/self/smaps` diffed per iteration named
one growing anonymous mapping, and `MICROIDE_PERF_BIG_ALLOC_BYTES=16000000` (a
backtrace on any allocation at or above that size, now in
`tests/perf/AllocationCounter.cpp`) named the line. **Balanced allocation counts
with growing RSS means a growing container, not fragmentation** — check the
mapping and the large-allocation backtrace before theorising about the allocator.

Fix: `PieceTree::InsertText` now compacts when the dead history exceeds
`kAddBufferDeadHistoryMultiple` (4x) the live document, above a
`kAddBufferCompactionFloorBytes` (4 MiB) floor, and `CompactAddBuffer` swaps the
buffers with empty ones so the capacity is actually returned rather than merely
`clear()`ed. Resident text is now proportional to the document instead of to how
long the session has been editing it; the scenario's per-iteration RSS growth
becomes a bounded sawtooth. Amortized cost is a fraction of a byte-copy per
inserted byte, since a compaction is O(live document) and cannot recur until
another 4x the live document has been inserted.
`PieceTree/AddBufferStaysBoundedUnderRepeatedLargeEdits` pins it by asserting the
buffer's high-water mark over 800 passes does not exceed its mark over the first
200 — a bound stated against the buffer's own behaviour rather than against the
trigger's constants, so it cannot drift out of agreement with them.

`editor_shaping_multi_caret` showed the same shape and is covered by the same fix.

Earlier archives: the 2026-07-12 deferred-backlog sweep (which cleared the pass
5–24 backlog) is at `guidelines/tech-debt/archive/2026-07-12-deferred-backlog-sweep.md`,
with per-item detail in the `Deferred backlog sweep — Batch A…I` commits.

### Deferred from the 2026-07-10 cross-subsystem bug-hunt pass (TD-2026-07-10-*)

Relocated here from `active-work.md` on 2026-08-03: these are open debt items, which
is what this ledger is for. Not fixed in that pass — low value or hard to reach/test.

- `SurfaceTextureCache` eviction/null-renderer fixes lack direct unit coverage:
  `Upload` needs a live SDL renderer (`SDL_CreateTexture`), unavailable headless.
  They mirror the already-tested sibling text-texture cache guard.
- **[RESOLVED 2026-08-05]** The single-line-input view-metrics OOB fix
  (`view_start_idx == size()` when no glyph left of the caret fits a
  sub-glyph-width field) lacked a direct regression test, because the logic was a
  `WorkspaceShell` member needing a live `text_renderer_` and the narrow-field
  trigger (the debug variables inline editor). The premise expired: TD-2026-07-17-
  084/083 moved it to the free `ComputeSingleLineViewMetrics`
  (`workspace/render/SingleLineViewMetrics.h`), and a default-constructed
  `TextRenderer` measures a deterministic 8 px per ASCII char — so the whole
  contract is now reachable directly with no shell.
  `RenderViewModelBuilder/SingleLineViewMetricsCaretWindow` covers the
  everything-fits case, the caret-relative scroll, the sub-glyph-width field that
  triggered the OOB, and selection clipping to the window (including the prefix
  offset). Worth remembering as a pattern: "untestable" claims should be re-checked
  after a refactor moves the code, not inherited.
- **[RESOLVED 2026-08-05]** Terminal minor spec deviations: a combining mark
  following a double-width glyph attached to the wide-trailing spacer and was
  dropped; multi-byte charset designations (`ESC ( " ?`) mis-parsed; DECSTBM on the
  alternate screen with origin mode was said to home to screen-top rather than
  region-top. Each now has a test; two were real, the third was already fixed.

  - **Combining mark after a wide glyph.** Two bugs stacked, and only fixing the
    first would have changed nothing observable. The mark attaches to
    `cursor_column_ - 1`, which after a double-width base is the wide-trailing
    spacer — a cell with `length == 0`, so the "is there a base glyph?" guard
    dropped the mark. Stepping back to the lead cell is the fix. But the lead cell
    then failed the *other* guard: a base plus a mark has to fit the cell's inline
    UTF-8 storage, and at four bytes it could not hold *any* accented double-width
    glyph — every double-width codepoint is U+1100 or above (3 bytes) and every
    combining mark is at least 2. Inline storage is now **five** bytes, which is
    free: `length` + `TerminalStyle` pad `TerminalCell` to 18 bytes at either
    width (6 is what costs 20), so nothing grows on any scrollback cell. A new
    `sizeof(TerminalCell) == 18` static_assert keeps that deliberate. Still
    unsupported, and out of scope here: an emoji with a 3-byte variation selector,
    or several stacked marks — that wants out-of-line per-cell storage, the way
    xterm.js does it.
  - **Multi-byte charset designations.** The parser consumed exactly one byte after
    the designator, so for `ESC ( " ?` the `"` was taken as the final and the `?`
    printed as text. ECMA-48 is `<designator> I... F`: intermediates 0x20..0x2F,
    then one final 0x30..0x7E. Intermediates are consumed now, under the same
    `kMaxEscapeSequenceLength` cap every other escape carries.
  - **DECSTBM under origin mode was already correct.** `MoveCursorLocked` clamps to
    `ActiveScrollRegionTopLocked()` on the alternate screen whenever origin mode is
    on, and DECSTBM assigns the new margins *before* homing, so `MoveCursorLocked(0,
    0)` already lands on the top margin. The existing coverage did not show this
    because it sets the region before enabling origin mode (`\x1b[2;4r\x1b[?6h`);
    the ordering that would expose a regression is `?6h` first. Pinned by
    `TerminalSession/DecstbmHomesToTheMarginUnderOriginMode`, which fails if the
    home escapes the margin (it writes at the homed position and checks the row
    above is untouched).
- **[RESOLVED 2026-08-05]** `MergeConflictKind` labelled a both-modified conflict
  `LineEndingHeavy` when only ONE side was line-ending-only. Filed as cosmetic; it
  is worse than that, because "sides differ only by line endings" is the sentence a
  user reads before deciding it is safe to take one side whole — and the other side
  had rewritten real content. "Mainly line endings" now requires both sides.
  Fixing it surfaced the mirror bug in the same predicate: the both-sides-changed
  branch required *both* raw sides to be non-normalized, so the common shape (both
  sides made the same content change, one in LF and one in CRLF) was missed. And
  the summary claimed "line endings or whitespace" while the detector normalizes
  line endings and nothing else, so it now says only what it detects. Covered by
  `MergeConflict/LineEndingHeavyRequiresBothSides`.
- **[RESOLVED 2026-08-05]** LSP diagnostics version gate after close→reopen
  (`WorkspaceLspClientDispatch`): `DidClose` erased the URI's tracked version and
  `DidOpen` reset it to 1, so a late `publishDiagnostics` from the previous open
  (version > 1) compared `old < 1` — false — and painted briefly on the reopened
  buffer until the next republish. Closed with the second of the two options the
  entry listed, "a version that never resets across reopen", which needs no
  generation token: `DidClose` retires the URI's version into
  `retired_document_versions` instead of forgetting it, and `DidOpen` resumes at
  retired + 1. Nothing in LSP requires `TextDocumentItem.version` to start at 1, so
  this costs nothing on the wire; a server restart clears the map along with the
  rest of the protocol state, because the new server has no memory of the old
  numbering either. Covered by
  `WorkspaceLspClient/DropsDiagnosticsFromAPreviousOpen` (real python fake server:
  open → v3 → close → reopen, with the server pushing a v3 diagnostic after the
  reopen), probed non-vacuous by forcing the reopen back to 1.
- **WON'T DO** — Windows `FileIndexWatcher` (`ReadDirectoryChangesW`) backend gaps:
  a tracked directory rename leaves ghost index entries + an unindexed subtree (no
  recursive delete / no subtree walk), and a change-buffer overflow
  (`bytes_transferred==0`) drops notifications with no full-rescan resync. Non-Linux
  host backends are not built; Linux inotify handles both cases correctly.

### Consumer-side reachability sweep — coordinator hooks (TD-2026-07-30-*)

The third question in this family, after "is this symbol produced?" (2026-07-26)
and "does anything populate this store?" (2026-07-27): *does anything call this
hook?* A coordinator's `Operations` struct is how it declares what it needs the
shell to do, and the shell fills every field with a working lambda — so a field
nobody invokes compiles, wires, reads as part of the contract, and does nothing.

- **Categories swept clean (negative results worth not re-checking).**
  Dead *data* fields: none — every candidate was either the standard
  `is_transparent` heterogeneous-lookup marker or genuinely read. Incomplete
  enum switches: none reachable — the build is `-Werror` with `-Wswitch`, so a
  default-less switch missing an enumerator cannot compile. List-navigation
  keys: consistent; the one variant (`WorkspaceKeyInputCoordinatorSurfaces`
  omitting Home/End) is deliberate and documented, because those move the text
  caret in a field-backed popup. Divider cursors: all seven `*Divider` drag
  targets have a cursor rule. Frame prep: already revision-gated with an
  allocation-free fast path.

- **[RESOLVED 2026-08-05] TD-2026-07-30-001 — the project-search regex / case /
  hidden-file toggles were mouse-only.** Found while removing the dead
  `toggle_project_search_pattern_mode`, `cycle_project_search_case_mode` and
  `toggle_project_search_hidden_files` hooks from the key coordinator: the
  behavior was live, but only from `WorkspaceShellSidebarMouse`. VSCode binds
  Alt+R / Alt+C / Alt+H inside the search box, so the same three hooks are back
  on `KeyInputCoordinator::Operations` and driven from
  `HandleSidebarKeyDown`'s Search branch — deliberately **before** the
  `editing` branch, so a chord works from the results list and from inside the
  query field alike (the buffer find widget's Alt+C/W/R already worked that way;
  see `HandleSharedBufferSearchKey`).

  The non-obvious half is what a chord does to an in-flight field edit. The
  mouse path commits it (`editing = false`), which is right for a click —
  focus genuinely leaves the field. For a keyboard chord it is wrong twice: it
  kicks the caret out of the box, and the toggles re-run the search, which reads
  the **committed** field values. So Alt+C after typing a new query would have
  re-run against the previous one. The three toggles now call a new
  `WorkspaceShell::FlushProjectSearchEditField()` first: it writes the edit
  buffer back into its field and leaves `editing`/caret untouched. The mouse
  path still commits before dispatching, so the flush is a no-op there.

  Alt+W (whole word) is deliberately NOT bound: `ProjectSearchOptions` has no
  `whole_word` — that is a worker + panel-button feature, not a keybinding, and
  is the remaining VSCode gap on this surface.

  The three panel tooltips advertise the chords now ("click or Alt+R for
  literal", …), matching how the find widget's read. Covered by
  `WorkspaceShell/ProjectSearchAltChordsToggleOptions`, which drives all three
  from the list and then re-drives Alt+C from inside the query field, asserting
  the field editor stays open and the rerun matched the just-typed text.
  Files: `src/workspace/coordinators/WorkspaceKeyInputCoordinatorSidebar.cpp`,
  `src/workspace/shell/WorkspaceShellProjectSearch.cpp`,
  `src/workspace/HoverTooltipSurface.cpp`.

### Producer-side reachability sweep (TD-2026-07-27-*)

A follow-on to the 2026-07-26 sweep, asking the mirror-image question: not "is
this symbol produced?" but "does anything in `src/` ever *populate* this store?"
A registry whose only writers are its own tests is a subsystem that cannot run in
the product, however complete and well-tested its read side looks. Two of the
shell's registries answered no. One was deleted; the other is filed below.

- **[OPEN] TD-2026-07-27-001 — `VirtualDocumentRegistry` has no producer, so a
  plugin can ask to open a virtual document but nothing can ever create one.**
  `WorkspaceShellPlugins.cpp` rebuilds seven sibling registries from plugin
  contributions in one function (formatters, save participants, completions, code
  actions, tools, SCM providers, annotation providers). Virtual documents are the
  odd one out: `virtual_document_registry_.Register(...)` is reachable only
  through the `WorkspaceShell::TestAccess` backdoor, and `PluginHost` has no
  `ContributedVirtualDocument` kind at all — the contribution type does not exist.
  The consumer side is fully built and behaves correctly (open-in-tab, reload on
  change, read-only enforcement, and the plugin `open_file` callback's
  `virtual://` branch), so `GetDocument` simply always returns nullptr and the
  plugin's `open_file` returns a bare `false` with no diagnostic.
  NOT deleted, unlike the review-comments registry: this read side is correct and
  covered by shell-level tests, so removing it would discard working code that is
  one contribution kind away from shipping. Closing it means designing a plugin
  API (`ctx.virtual_documents` or similar) plus its lifetime/ownership rules,
  which is a feature decision rather than a missing wire — hence filed, not fixed.
  **[PARTIAL 2026-07-30]** The diagnostic half is done: the `open_file` branch
  now writes a specific reason to the Plugin Errors output channel naming the
  URI and stating that no plugin API contributes virtual documents yet, instead
  of failing mutely. Note the return value was never a usable signal —
  `PluginHostCallbacks` routes `open_file` through `ApplyHostMutation` on the
  non-direct path and returns `true` unconditionally there, so a plugin never
  observes the `false` at all. The registry/producer half (designing
  `ctx.virtual_documents` plus its lifetime rules) remains open and is still a
  feature decision. Not unit-tested: reaching the shell's `open_file` lambda
  from a test would need a callbacks accessor on `PluginHost`, which would widen
  a deliberately narrow boundary for a log line.
  Files: `src/workspace/WorkspaceVirtualDocument.*`,
  `src/workspace/shell/WorkspaceShellPlugins.cpp`,
  `src/workspace/coordinators/WorkspaceTabCoordinatorShellBridge.cpp`.

### Reachability sweep (TD-2026-07-26-*)

Two related passes. The first treated the architecture lint itself as code under
test rather than as an oracle. The second asked the same question of the product:
*can a user actually reach this?* — sweeping for symbols that are handled but
never produced. Both found shipped code that could not run, and each ended in a
new hard lint so the class cannot silently return:
`CheckEveryActionIdIsReachable` (an action named only in `case` labels) and
`CheckRegisteredSettingsAreRead` (a setting the overlay shows and persists while
nothing reads it). Three hard rules were **structurally incapable of reporting a
violation** and had been passing green for that reason, not because the tree was
clean — two of them were hiding real defects. All three are fixed with
negative + positive control fixtures in
`tests/architecture/ArchitectureRuleFixtures.cpp`:

- `CheckDescriptorCreationIsCloseOnExec` spelled its pattern `openat?`. `?` binds
  to the single preceding character, so it matched `opena`/`openat` and never
  plain `open(`. It was hiding an unflagged `open("/dev/null")` in
  `platform/Subprocess.cpp` and a bare `O_EVTONLY` open in the macOS
  `FileWatcher` backend (those descriptors are held for the whole watch, so every
  spawned child inherited one per watched path). The rule now also covers plain
  `accept(`, which its own advice string already forbade but no pattern matched.
- `CheckTerminalSessionNoExtractedImpl` anchors five helper definitions with `^`
  but built the regexes without `std::regex::multiline`, so `^` meant "offset 0
  of the file" — always `#include`. Three of the five sub-checks could never fire.
- `CheckNoDirectGitRepositoryInWorkspace` matched only a temporary
  (`GitRepository(root)`); every real construction is the named-declaration form.
  Reviving it surfaced a synchronous `git rev-parse --short MERGE_HEAD` on the
  **shell thread** (bounded only by the 60 s git read timeout, for a pane
  caption) and a `GitRepository` built solely to do lexical path math.

`CheckThrowingStoParsers` was also promoted from warn-only to hard-fail: it is
listed as a load-bearing invariant but a reintroduction only printed a line.

**Repeat this.** The generic technique: inject a synthetic violation into the
real tree, run the one rule, revert, and check it actually failed. No rebuild is
needed — the test binary reads the live tree:

```bash
cp "$file" /tmp/probe.bak
printf '%s\n' "<synthetic violation>" >> "$file"
./build/microide/microide_tests "ArchitectureInvariants/Workspace/$rule"
cp /tmp/probe.bak "$file"
```

Craft the probe from the rule's **actual regex**, not from its comment — several
rules are narrowly scoped and a plausible-looking probe simply misses (that reads
identically to a dead rule, so confirm before concluding). For required-presence
rules, delete the anchor token instead of appending. "Detected but still green"
means `hard_fail` was never set.

**Audit status (2026-07-26): every rule in the terminal, plugin and workspace
sets has now been probed.** Three were dead (fixed above), one was warn-only
(promoted), two more lacked a loud-missing-target guard (added); the remainder
fire correctly. The four not probed by injection are self-guarding: the two TU
line-count caps, and `CheckTextViewportSpecialMembersCoverEveryField` /
`CheckBuildEditorViewModelUsesIncrementalVectorWrites`, which already fail when
they cannot locate their target.

That audit is a **one-shot** result, not a ratchet — it says nothing about the
rules as they will be after the next edit. Most still have no fixture, so the
durable follow-up is to grow `tests/architecture/ArchitectureRuleFixtures.cpp`
(add a fixture whenever you touch a rule) rather than to re-run the whole probe
sweep by hand.

**The product-reachability half.** Sweeping for enum values mentioned only in
`case` labels, and struct fields mentioned only at their declaration, found four
features that shipped with no way to invoke them and two Settings controls that
did nothing. Triage matters more than the sweep here — *retired* means delete,
*unreachable but wanted* means wire it up, and *output taxonomy with a live
display branch but no detector* means leave it and file the gap, because deleting
it downgrades what the user is told rather than removing waste.

Other findings from the same sweep, closed in the commit log from `14da0f96`:

- **Full screen could not be invoked**: `WindowAction::ToggleFullscreen` ran
  `SDL_SetWindowFullscreen`, and no menu item, command or keybinding produced it.
  Now a View-menu entry + `toggle-fullscreen` command (no chord — F11 is
  `debug-step-in` and this keybinding model has no when-clause).
- **Hit-count breakpoints and logpoints could not be set**: implemented
  end-to-end, but the gutter menu offered only "Set Condition…".
- Two Settings entries ("Hover Delay (ms)", "Scrollbar Size") were declared,
  rendered, persisted, and read by nothing.
- A **stale diff** reported a raw `git apply` error instead of the
  refresh-and-retry message that already existed.
- **Submodule and mode-only merge conflicts** classified as ordinary content
  conflicts; the discriminators were on the porcelain v2 wire and being discarded.
- `AddSecondaryCaret` re-sorted the whole caret list on every insert despite the
  list already being sorted.

- `GitRepositoryState::operation_state` was declared, defaulted, and **never
  written by anything**, which made the merge resolver's rebase/cherry-pick
  "upstream" label unreachable. Now derived from git's own marker files.
- LSP completion ignored `sortText` entirely (the struct carried a dead
  `int sort_text_priority`), so the popup used raw server array order.
- `control-list` echoed the attacker-droppable descriptor body, handing back the
  forged `socket` path that enumeration deliberately reconstructs to avoid, and
  letting a pretty-printed descriptor inject extra JSONL lines.
- The unwired AI-provider / external-agent contribution stub was deleted (same
  disposition and guard as the earlier MCP-tool stub).

Still open from this sweep:

- **TD-2026-07-26-004 — five result/classification enum values are handled but
  never produced. OPEN (detection gaps, deliberately NOT deleted).** Found by the
  same "appears only in `case` labels" sweep as -003. Unlike the retired chat
  cluster, these are *output taxonomies* with live display branches, so deleting
  them would silently downgrade what the user is told rather than remove waste:

  - `MergeFileConflictKind::Submodule` — **RESOLVED 2026-07-26.** The
    discriminator was already on the wire and being discarded: porcelain v2's
    `<sub>` field is `S…` for a gitlink (verified against real output: `1 .M SC..
    160000 … dep`). The parser now captures it into
    `GitRepositoryEntry::submodule`, and it outranks every content kind — there is
    no text to three-way merge, and `TextHunksAvailable` already returned false.
  - **`MergeFileConflictKind::Mode` is NOT reachable and should not be "fixed"
    the obvious way. OPEN, and probably WON'T-DO.** Comparing the per-stage modes
    on an unmerged record looks like the way to spot an executable-bit conflict.
    It is not, and an attempt at it in this sweep shipped briefly before being
    backed out. Two reasons, both confirmed against real git:
    * A 644 vs 755 divergence never conflicts — with only two regular-file modes,
      any three-way combination auto-resolves, so git emits no conflict at all.
    * A genuine "distinct types" conflict (file vs symlink) is reported as TWO
      records that each carry a **zero** stage — `u UA … 000000 000000 120000 …
      thing` plus `u UD … 100644 100755 000000 … thing~HEAD` — so no
      ours-vs-theirs mode pair ever differs on a single record.
    Detecting it at all would need the same cross-record correlation the D/F case
    uses (pair `<path>` with `<path>~<suffix>`), and the resulting conflict is
    arguably better described as distinct-types than as a mode change. Decide what
    the kind should MEAN before implementing anything.
  - `MergeFileConflictKind::FileDirectory` — **RESOLVED 2026-07-26.** It is not on
    the wire, so it needs a worktree probe, and the shape is not what it looks
    like: git does NOT report the colliding path. It leaves the directory in place
    and moves the file side aside, so the unmerged record names
    `thing~file-side` (an ordinary file) while `thing` is the directory —
    identical in both merge directions, only the suffix differs (`~file-side` vs
    `~HEAD`). Probing the record's own path therefore never fires; the detector
    tests the prefix before the last `~`, on the background refresh, for
    conflicted entries only. Submodule still outranks it (a submodule checkout is
    also a directory) and `TextHunksAvailable` is now false for it.

    Worth remembering how this was nearly missed: the first implementation probed
    the record path, and its end-to-end test asserted *conditionally* ("if git
    left a conflicted directory, then…"), so it passed while detecting nothing.
    Reading the actual `git status --porcelain=v2` output for both merge
    directions is what found it. An end-to-end test for a detector must assert
    unconditionally, and be shown to fail when the detector is broken.
  - `PatchApplyResultCategory::StaleDiff` — **RESOLVED 2026-07-26.**
    `ClassifyGitApplyFailure` now reads git apply's stderr and splits a
    content/index mismatch (the diff is stale; "refresh the compare tab and try
    again" already existed as a message) from a corrupt/unparseable patch, which
    is our bug and must NOT be dressed up as refresh-and-retry. Corrupt wins when
    git reports both.
  - `PatchApplyResultCategory::Cancelled` is still never produced. There is no
    cancellation path into a patch apply today (the operation runs to completion
    on the background executor), so this is the message waiting for a cancel
    affordance rather than a missing classification.

  Deleted in the same pass because they were genuinely unused vocabulary rather
  than undetected outcomes: `PatchOperationKind::StageFile` / `UnstageFile`
  (whole-file staging goes through `git add` / `GitStagePath`, never the patch
  applier).

- **TD-2026-07-26-006 — the DAP end-to-end test stops at the handshake; the
  launch -> breakpoint -> stopped cycle is still only stub-covered. OPEN.**
  `DapRealAdapterE2ETests.cpp` drives a real gdb through initialize, capabilities,
  a `threads` round trip and disconnect. Everything past that — launch, breakpoint
  binding, stop events, stepping, variables — is still exercised only by
  stub-mode tests (85 of them), i.e. against a fake we wrote ourselves. That is
  the gap that hid the pending-breakpoint tint bug.

  The protocol sequence was verified by hand against gdb 17.2 and is recorded
  here so a follow-up does not have to rediscover it:

  1. `initialize` -> success + capabilities, then an `initialized` event.
  2. `setBreakpoints` BEFORE launch -> success, but
     `{"id":1,"verified":false,"reason":"pending"}` (symbols not loaded yet).
  3. `launch {"program": ...}` -> success; `process` and `thread` events.
  4. `configurationDone` -> success.
  5. Several `breakpoint` events, `reason:"changed"`, each with
     `verified:true` and an `instructionReference` (the address changes as PIE
     relocation resolves) — this is what clears the pending state.
  6. `stopped` with `{"reason":"breakpoint","hitBreakpointIds":[1],
     "allThreadsStopped":true}`.

  Not added yet because it needs a compiled debuggee (a gcc availability gate on
  top of the gdb one) and generous timeouts; a flaky end-to-end test that spawns
  gcc + gdb + a debuggee on every CI run would cost more than it pays. Gate it the
  same way the clangd and gdb suites are gated, and prove it fails when the
  behaviour regresses before landing it.

- **TD-2026-07-26-005 — `WorkspaceShell/ReplaceAllReadsOnlyMatchedFiles` is a
  rare flake. OPEN (observed once; not reproducible on demand).** Seen failing a
  single time during a full parallel `ctest`, then green across 31 further runs
  (25 of the test alone under 6-way CPU load, plus 6 full parallel suites) with a
  probe printing the two counter readings — which never fired again. So the
  numbers behind the failure were never captured.

  The measurement is inherently fragile and the test knows it: it subtracts two
  readings of the **process-global** `util::TextSearchReadCount()` across windows
  that contain live background threads, and asserts the difference is exactly 2.
  Prior rounds already added `WaitForProjectSearchWorkersIdle` barriers for the
  same reason (see the comment in the test). The remaining suspect is the
  **filesystem watcher**, which those barriers do NOT drain: replace-all writes
  `match_a.txt` and `match_b.txt` inside the measured window, guaranteeing watcher
  activity there, and a watcher-triggered rescan lands extra counted reads.

  **[RESOLVED 2026-08-05 — the second option, a counter nothing else can
  perturb.]** Quiescing the watcher for the measured window would have made the
  test depend on a new drain seam and would still leave the measurement standing
  on a counter any subsystem may bump. Instead the replace path got its own
  instrumentation: `search.project_replace_candidate_files` and
  `search.project_replace_files_read`, incremented only inside `RunProjectReplace`
  (a pure function over its own arguments, reached only by an explicit user
  replace-all — the watcher cannot trigger one). The test is now a plain
  before/after delta of those two, with no control search, no
  `WaitForProjectSearchWorkersIdle` barriers, and nothing global in the window.

  Two things worth keeping from the rewrite. The candidate count is the better
  assertion of the two: reads==2 is also what a whole-catalog fallback over a
  2-file project would report, whereas candidates==2 pins that the fast path ran.
  And the wait had the "!running is also true before the work starts" trap — a
  replace-all runs on the background executor and its trailing refresh is only
  fired by the apply, so the test now waits on **both files carrying their
  replacement on disk**, which every counted read provably precedes.

  `util::TextSearchReadCount()` / `ResetTextSearchReadCount()` were the seam's
  only consumers, so both are deleted along with the relaxed atomic increment
  they cost every project-search file read. Probed non-vacuous by forcing
  `results_cover_all_matches` false: the candidate assertion fails.

- **TD-2026-07-26-003 — `CommitOperationResultCategory::RefreshFailedAfterSuccess`
  was handled but never produced. [RESOLVED 2026-08-05.]** `ResultFeedback` and
  `ResultTone` both have a branch for it, so a commit that succeeds but whose
  follow-up refresh fails would say "Commit succeeded, but repository refresh
  failed" — except nothing ever set the category, so the toast said plain success.

  **This was a missing-message issue, not silent data loss.** `PublishResult`
  fires `MarkStale()` + `request_git_refresh()` and returns; the refresh runs
  asynchronously and its failure IS surfaced independently, as the git sidebar's
  "Git refresh failed: …" banner (`BuildGitRefreshErrorBanner`). Only the
  commit-specific toast was missing.

  **The UX decision: follow-up, not replacement.** The commit really did succeed
  and saying so promptly matters more than withholding feedback for a refresh that
  can take seconds, so the success toast fires unchanged and a second warning toast
  follows if the refresh comes back failed. A newer commit disarms the watch, so
  the operation in front of the user always owns the feedback.

  **The correlation does NOT run off `repository_generation`**, which is what the
  entry above assumed. That value is the snapshot the commit was *composed*
  against; what a published refresh snapshot carries is the **refresh** generation
  (`GitRepositoryService::refresh_generation_`, stamped onto the request and copied
  into `RefreshSnapshot::generation`). So `PublishResult` now records
  `CurrentRefreshGeneration() + 1` before calling `request_git_refresh()` and
  accepts the first snapshot at or above it. The `+ 1` and the "at or above"
  matter: `RequestAutomaticGitSidebarRefresh` is throttled to 750 ms and returns
  early while a refresh is in flight, in which case the call only marks the repo
  stale and the commit's changes surface in a *later* generation than any the call
  minted. Reading the generation the request produced would have watched the wrong
  refresh — or no refresh at all.

  The reporting hop is the shell's `consume_git_refresh_snapshot` lambda, which is
  the one place an async refresh outcome is known on the main thread.
  `WorkspaceShell/CommitReportsFailedFollowUpRefresh` drives it end to end through
  a real commit with `FailGitRefreshesForTesting` armed after dispatch (so the
  pre-checks and the commit itself still run against the real repo), and also pins
  the one-shot property — a second failing refresh must not re-attribute itself to
  the commit. Probed non-vacuous by disarming the watch.

  The parallel `bool refresh_failed_after_success` on `CommitOperationResult` was
  deleted: the committing worker cannot know the refresh outcome, so a flag it
  could never set only made the gap look closed.

### Post-merge review pass (TD-2026-07-25-*)

Full review of the `td/wait-until-helper-088` branch before it merged to `main`
(commit `5b35124a`), plus a follow-up dedup/perf pass. Three real defects were found
and fixed there (poll-watcher startup race that permanently lost files, LSP
resource-op reconcile running grouped by kind instead of apply order, and
completion/code-action rows losing their scrollbar-aware truncation); the dedup pass
collapsed the 3× duplicated LSP resource-op URI flattening + version gate into
`workspace/LspWorkspaceEditOps`, replaced the poll worker's slice-sleep with a
condition-variable wait (~15 idle wakeups per 750 ms interval → 1), disposed staged
file backups synchronously so they cannot be indexed during the async window, and
static_asserted the two constant pairs that must stay equal
(`KeybindingContext` vs the uint8 conflict-index bitmask; `kSidebarInset` vs
`kCommitWorkflowFieldInset`). What is left open:

- **TD-2026-07-25-001 — `snippet_many_mirror_edit` regressed ~7.6% p50 from code
  layout, not from any semantic change. OPEN (needs direct profiling).**
  `tools/perf-compare.py origin/main` over all 89 scenarios showed a balanced picture
  (mean p50 delta **+0.45%**, 42 slower / 45 faster) with real wins — `file_finder_cold`
  −5.2% p50 / −11.2% p95, plus `lsp_message_framing`, `lsp_publish_diagnostics_parse`,
  `review_comments_registry_lookup`, `dap_protocol_encode_decode`. Every flagged
  regression except one dissolved when re-measured at 25 iterations (5 iterations is
  too few for scenarios whose p50 and p95 differ 6×). The survivor is
  `snippet_many_mirror_edit`: **+7.6% p50 / +9.8% p95**, reproducible across three
  independent runs.

  It is **not** a semantic regression. The scenario exercises only
  `editor::TextViewport` + `editor::SnippetEngine`; the branch changed **zero** files
  under `src/editor/`, and the allocation counts are byte-identical on both sides
  (365,283). Bisecting with `tools/perf-compare.py 8d548ce4` puts it in the
  render-view-model commit (`1ab136dd`) onward — i.e. it tracks that commit's
  redistribution of `microide_core`'s TU set (the new `SingleLineViewMetrics.cpp` TU
  plus ±500 lines moved into `RenderViewModelBuilder.cpp`), which shifts LTO
  inlining/code layout for unrelated hot loops. This is exactly the residual class
  `dev-docs/performance/perf-harness.md` warns about ("LTO can recover some inlining
  loss, but residual regressions still need direct profiling and explicit
  fix-or-accept decisions"). `menu_popup_hover_rows` (+7.7% p50 / +0.3% p95) and
  `editor_surround_multi_caret` (+5.1% p50 / +0.5% p95) sit just under the reporting
  bar in the same direction and are probably the same effect.

  **Accepted for now**, not silently: the absolute cost is ~3.5 ms across 30 full
  150-mirror snippet expansions (~0.1 ms each, not user-perceptible), and it is far
  inside the committed gate (50.1 ms vs a 42.7 ms baseline at a 75% p50 tolerance).
  Closing it properly needs a profiler to attribute the moved instructions, and this
  host cannot run `perf`/`valgrind` (`perf_event_paranoid=4` — see
  `dev-docs/performance/runtime-profiling.md`). Revisit with a hot/cold layout or
  `__attribute__((hot))` experiment on the snippet remap loop, or on a host where
  profiling is available. Do NOT rebaseline the scenario to hide it.

- **TD-2026-07-25-002 — the macOS / Windows / generic poll-fallback backends carry the
  reviewed poll rewrite unverified. OPEN (platform, same category as 004/005/010/035).**
  `FileIndexWatcher.cpp` holds four per-platform `Impl` structs that each carried a
  byte-identical copy of the poll worker. The review collapsed them into one shared
  `RunPollFallbackWorker` + `BuildPollSnapshot` + `PollStopSignal` (which is where the
  startup-race fix lives), so the macOS/Windows/generic Impls now differ from the Linux
  one only in the three mechanical lines that declare and drive `stop_poll`. Only the
  Linux backend compiles and runs here, so the other three are reviewed-but-unbuilt.
  The shared code is platform-neutral (`std::filesystem` + `std::condition_variable`),
  so the risk is a build break rather than a behavior difference — but it is real until
  someone builds on those hosts. The backend-independent contract suite
  (`FileIndexWatcherContractTests`, incl. the new startup-window case) is written to run
  unchanged against any backend, so validating them is a build away.

### Cross-subsystem sweep (TD-2026-07-25-1xx)

A per-subsystem pass over `util`, `platform`, `terminal`, `editor`, `render`, `compare`,
`project`, `plugin`, `persistence`/`app`, and `workspace`, run with five different lenses
(correctness, allocation/perf, dedup, portability/toolchain, and mechanical sweeps). The
fixed findings are in the commit log from `15da5265` onward and include: a
`ReadFileLineWindow` bug that returned garbage extra lines and streamed whole files;
syntax highlighting silently disabled on any line with an invalid UTF-8 byte;
case-insensitive **regex** search not folding Unicode case (while literal search did);
`SettingFlagEnabled` being case-sensitive and not recognizing `no`; an `AsciiGlyphAtlas`
blit that could bleed a wide glyph into the next slot; the whole tree failing to compile
under clang; and ~39% off the dominant hunk-alignment phase of a diff build. Two
mechanical sweeps came back clean and are worth repeating: the suite passes under
`-D_GLIBCXX_ASSERTIONS` (no out-of-bounds container access anywhere), and GCC and clang
both build `-Werror`-clean. What is left open:

- **TD-2026-07-25-101 — regex search is Unicode-aware only for case folding, not for
  pattern semantics. OPEN (needs a product decision).**
  `util::SearchRegexCompileOptions` now adds `PCRE2_UTF|PCRE2_UCP` when a
  case-insensitive query carries a non-ASCII byte, which fixes the concrete bug (`Δ` did
  not find `δ` in regex mode even though it did in literal mode). It deliberately does
  NOT turn UTF on for ASCII queries, because every realistic query is ASCII and
  byte-oriented matching is the faster path — speed is the stated first priority.
  The consequence is that `.`, `\w`, `\b` and friends match **bytes** for an ASCII
  query and **codepoints** for a non-ASCII one. VSCode (JS regex / ripgrep) is
  consistently codepoint-oriented. Making microide match would mean always compiling with
  `PCRE2_UTF|PCRE2_UCP|PCRE2_MATCH_INVALID_UTF`, at a measurable cost on project-wide
  search over a large tree. That trade is the user's call, not a silent default: raise it
  before changing, and measure `search_*` scenarios if it is taken.

- **TD-2026-07-25-102 — `platform/ControlSocketServer.cpp` still cannot compile on macOS.
  OPEN (platform, same category as 002).**
  The pipe-creation sites across `platform`/`util` now share `util::MakeCloexecPipe`,
  which picks `pipe2` on Linux and `pipe()+fcntl` elsewhere — that closed the `pipe2`
  half of the problem for `TerminalBackend` and `ControlSocketServer`. But
  `ControlSocketServer` also calls `accept4(...)` and passes `SOCK_CLOEXEC` to
  `socket()`, both Linux-only, from inside a `__unix__ || __APPLE__` block. The same
  shim treatment applies (accept + `F_SETFD` fallback) but it is unverifiable here.

- **TD-2026-07-25-103 — the four per-platform `FileIndexWatcher::Impl` structs still
  duplicate their poll-fallback field block. OPEN (deliberately deferred).**
  TD-2026-07-25-002 collapsed the poll *worker*; each `Impl` still repeats the same seven
  members (`poll_mode`, `force_poll`, `poll_interval`, `poll_worker`, `stop_poll`,
  `initial_scan_worker`, `stop_initial_scan`) plus an identical `StopInitialScan()`.
  Folding them into a shared `PollFallbackState` member would touch every reference by
  name across three platform blocks that do not compile on this host — a poor
  risk/reward until 002/102 are unblocked.

- **TD-2026-07-25-104 — `TextLayoutCache::VisibleLineLayoutRefCached` hands out a
  reference into an evictable cache. [RESOLVED 2026-07-30 — enforcement added.]**
  Closed with the third of the options listed below: the cache now counts
  evictions (`Stats::visible_line_evictions`), and
  `TextLayout/VisibleWorkingSetDoesNotEvict` pins that a generous frame-sized
  working set (200 rows, three passes) evicts nothing, with a control case that
  exceeds the limit so the assertion cannot pass vacuously. Verified by lowering
  `kVisibleLineCacheLimit` to 64 — the exact regression the entry warned about —
  and watching the test fail. The reference-returning API is unchanged; what was
  missing was enforcement, not a redesign. Original analysis:
  The returned `const LayoutLine&` points into `visible_line_cache_`, whose FIFO eviction
  `erase`s entries once `kVisibleLineCacheLimit` (256) is reached. A caller that holds the
  reference across a later query for a different line can therefore be left dangling.
  The requirement IS documented at the declaration (`TextLayoutCache.h`: "stable until the
  next call that can evict it … the per-frame working set is far below
  kVisibleLineCacheLimit"), and no current caller violates it — 256 comfortably exceeds any
  visible row count, so a frame never evicts what it is still reading. The gap is
  enforcement, not awareness: nothing fails if a future caller stashes the reference or if
  someone lowers the limit. Options are a lint over the call sites, an assertion that the
  live working set stays under the limit, or returning a stable handle/index instead of a
  reference.

### ⏭️ Standing backlog — deferred to dedicated passes (revisit; NOT dropped)

The 2026-07-17 correctness/perf burndown implemented the concrete, self-contained wins
(068, 31, 090, 083, 040) and left the rest **DEFERRED** with per-item rationale in the
subsections below. A follow-up editor-display-column pass has since landed cluster 3 in full
(**021/023 RESOLVED**) plus partial wins on the async cluster (047, 21). These are real work
— each is a multi-file *dedicated pass* the audit explicitly said not to bundle — deferred,
NOT declined. This section is the durable reminder
to actually schedule them. Pick one cluster at a time (async first — speed is the #1 project
priority), give it its own reviewed change + test matrix, and move its items to RESOLVED as
they land.

**Worth doing — schedule these (each = one focused pass):**

1. **Async / off-thread hardening — CLUSTER COMPLETE 2026-07-18.** All items resolved or
   dispositioned: **014** (terminal write), **061** (file-manager reveal), **081/082**
   (forced rescan), **091** (LSP client retirement pool), **21** (project replace-all),
   **011/18** (server-pushed WorkspaceEdit closed-file writes) RESOLVED — all moved off the
   shell thread; **080/086/38** already-satisfied/already-off-thread; **055** WON'T-DO
   (per-run search thread spawn is off-UI-thread + measurement-negligible); **016/017**
   WON'T-DO (gracefully bounded; the fix is a `run_async` plugin-API contract change);
   **047/19** (compare/merge model build) WON'T-DO in this burndown (partial win — syntax is
   pinned main-thread — with high coupling risk on an unverifiable surface; the shipped
   PARTIAL already took the practical no-op-refresh win). See each item's entry for detail.
2. **Render view-model build-out — CLUSTER COMPLETE 2026-07-24.** Overlay view model fully
   owned (084 **RESOLVED** — precomposed rows/labels + geometry, both live pointers deleted);
   debug-pane + bottom-panel VMs converted to narrow typed pointers / prepared data and the
   new ratchet lint `CheckRenderViewModelsOwnProjectState` freezes the remaining
   `ProjectWorkspaceState*` escape hatches to exactly {FrameSurfaceViewModel,
   SidebarSurfaceViewModel} (26 **RESOLVED** as a ratchet — the frame/sidebar TUs render live
   editor viewports / write render-derived hit rects and stay documented escape hatches); the
   083 residual (commit-body sizing/scroll-clamp) moved to frame prep
   (`PrepareCommitBodyViewportForFrame`) **RESOLVED**. Bonus fix: the command palette /
   launch-config picker query fields never rendered a caret or caret-relative scrolled text
   (`BuildActiveTextInputVisual` lacked their cases). See the render subsection for detail.
3. **Editor display-column unification** — one grapheme/visual-width service; inlay hints
   share it (021/023). **[RESOLVED 2026-07-17 — see the Editor / Unicode subsection]**
4. **DAP lifecycle hardening** — session-generation + request-id gating (025), bounded
   stop/terminate escalation (026). **[AUDITED 2026-07-20 — both already structurally
   satisfied in-tree; no change made. See the Debug/DAP subsection for the per-item audit.]**
5. **Scanner/search incomplete-state plumbing** — surface complete/truncated/incomplete
   status (008/033) **[RESOLVED 2026-07-20 — scan-status taxonomy + finder/search
   surfacing]**; targeted fallback rescan (009) **[CLOSED 2026-07-22 — incremental-hashing
   half dispositioned WON'T-DO (semantically unsound for content-change detection), poll-tick
   deep copies eliminated instead; see the scanner subsection]**.
6. **LSP completeness** — **CLUSTER COMPLETE 2026-07-23.** Resource ops + version-aware
   edits (011) **[RESOLVED 2026-07-23 — `documentChanges` create/rename/delete ops with
   validate-first + rollback-safe staging, versioned-TextDocumentEdit gating; see the LSP
   feature-completeness subsection]**; explicit timeout result variants (012)
   **[RESOLVED 2026-07-20 — `LspResult<T>` outcome taxonomy; see the same subsection]**.
7. **Plugin registry** — O(1) duplicate-id detection (077) **[RESOLVED 2026-07-18 — per-kind
   id index]**; per-field byte caps (018) **[RESOLVED 2026-07-19 — central ToHostString
   backstop + per-surface render caps; see the Plugin caps / policy subsection]**;
   measured caps (019) **[RESOLVED 2026-07-24 — caps re-derived from perf-runner-v1
   measurements: status 1,024 (new dedicated cap), per-kind 100,000 → 16,384, plus the
   O(N²) keybinding conflict scan and the unbounded status-bar right-loop scan fixed;
   see the Plugin caps / policy subsection]**; `lua_State*` boundary refactor (22/020/058) **[RESOLVED 2026-07-23 —
   boundary verified already-clean + new hard lint `CheckLuaStaysBehindPluginBoundary`
   (transitive-include aware); the no-longjmp audit shipped as the AST-based
   `tools/audit-lua-longjmp.py`, tree clean over 142 raise-capable sites. See the
   Architecture lint / test infrastructure subsection.]**.
8. **Tab identity** — stable per-tab id threaded through dirty-prompt + persistence (024)
   **[RESOLVED 2026-07-18; re-verified in-tree 2026-07-22 — `TabEntry::stable_id` +
   prompt-time stamping + confirm-time resolution + `DirtyPromptSurvivesTabShiftWhileOpen`.
   The persistence half stays deliberately unbuilt: ids only key modal dirty-prompt state,
   and a modal prompt never survives a session save. See the Tab identity subsection.]**
9. **Test-infra sweeps — CLUSTER COMPLETE 2026-07-24.** Architecture lints + negative
   fixtures (032/037) **[RESOLVED 2026-07-24 — 032: the vacuous persistence-I/O rule
   rewritten as a workspace raw-stream allowlist ratchet (caught two dead `<fstream>`
   includes); 037: reactivation-refresh + fallback-viewport-symbol lints added, stale-path
   audit un-vacuated the status-bar async rule; see the test-infra subsection]**, watcher
   contract suite (036) **[RESOLVED 2026-07-24 — backend-parametrized contract via
   `SetForcePollForTesting`, FileIndex end-state oracle; see the test-infra subsection]**,
   terminal stress suite (015) **[RESOLVED 2026-07-24 — real-PTY teardown stress:
   stop-mid-flood/alt-screen/self-exit, open-close loop, concurrent destructor teardown;
   same subsection]**, fuzz corpus seeding (052)
   **[RESOLVED 2026-07-22 — 60 curated seeds across all six targets + fixed the silently
   link-broken fuzz gate; see the test-infra subsection]**, shared
   `WaitUntil` polling helper (088) **[RESOLVED 2026-07-22 — see the Architecture lint /
   test infrastructure subsection]**, large-buffer edit perf scenario + direct-`Snapshot()`
   lint (022) **[RESOLVED 2026-07-22 — same subsection]**, per-frame-prep counters (030)
   **[RESOLVED 2026-07-22 — audited satisfied + added the missing steady-state guard; see
   the render subsection]**.
10. **Plugin UI features** — bottom-panel preview scroll (60), hit-region dispatch (61).
    **[CLUSTER COMPLETE 2026-07-24 — both RESOLVED; see the plugin-surface-preview
    subsection under the 2026-07-16 deferred set. Bonus fix: `BottomPanelVisible()`
    said Terminal-or-Output only, so a PluginSurface panel was rendered but
    mouse-dead (wheel/clicks/resize/cursor all fell through).]**

**Platform passes — WON'T-DO (maintainer decision 2026-07-24):** 004/005/010/035 (Windows),
006/062 (macOS) need a Windows/macOS host to build+verify and no such host is planned;
dispositioned WON'T-DO rather than left dangling. Revisit only if a real Windows/macOS
port effort starts. See the Platform-specific subsection.

**Genuinely not worth (true WON'T-DO, do not re-file):** 048 (deliberate bounded explicit-
save formatter), 003 (non-actionable, no live defect), 041/045/046/056/057/069/013/066/031/038
(from the earlier passes — unreachable/deliberate; rationale recorded inline).

### Cross-subsystem bug/perf audit addendum (TD-2026-07-17A-*)

Fresh source-backed findings from a 2026-07-17 pass across editor, workspace/render,
diff/merge, LSP, git, plugin UI, settings, and review/session glue. Prioritize the
speed-path items first, then the correctness/lifecycle cleanups.

> **Burndown disposition — 2026-07-17A pass.** A first burndown implemented the concrete,
> self-contained, individually-testable wins from this addendum and left the multi-file
> refactors as scheduled focused passes (per the same "do not bundle" reasoning as the
> Standing backlog). Every item below now has a disposition: **RESOLVED** (fixed + regression
> test this pass), **DEFERRED** (folded into a focused-pass cluster below), or **WON'T-DO
> here** (platform-only, cannot build/verify on this Linux host).
>
> **Cluster 2 (bounded resources) + cluster 7 (protocol/session lifecycle) burndown, 2026-07-18.**
> Landed the remaining bounded-resource + lifecycle items one commit each (full 24-shard suite +
> ASAN + UBSAN + TSAN all GREEN): **018, 038, 043, 057, 071, 095, 097, 099, 107, 118** (cluster 2)
> and **030, 082, 083, 100, 115, 130** (cluster 7), plus **041** (batch-review open cap).
> **074** (serial-work-queue depth/dedupe) is now **RESOLVED 2026-07-18** as its own data-structure
> pass (`std::list` + key->node index ⇒ O(1) `PostLatest`; opt-in `max_depth` budget that only sheds
> caller-marked droppable jobs, never critical ones). The only remaining still-open addendum items are
> the platform WON'T-DO **104/133/134** (Windows).
> (**004** folding-refresh hoist, **015/016** LSP bulk-sync, and **022** soft-wrap edit fast path are
> now RESOLVED — see their entries.)
>
> **RESOLVED this pass (39 fixed + 1 already-satisfied; each with a regression test):**
> - **001** — passive menu measurement (`ComputePopupMenuRect`, `MenuItemLabel`, `IsMenuItemEnabled`) reads LSP readiness with `ensure_started=false`, so opening/hovering a menu never spawns a server; servers still start on explicit LSP actions via `GetServer`.
> - **011** — plugin-command menu enablement uses `PluginHost::HasCommand` (O(log n), allocation-free) instead of a linear `std::find` + per-item `std::string` materialization.
> - **012** — command-line completion takes the plugin command-name vector by reference (no whole-registry copy per open/keystroke).
> - **013** — clean saves stream from the live TextBuffer (`SerializeLinesStreaming`) with no whole-document `Snapshot()`.
> - **019** — settings per-category row lookup uses cached index vectors (O(rows) build; O(1) lookup) instead of an O(rows²) render rescan.
> - **020** — plugin log/error history is capped (front-trim to `kMaxRecordedLogEntries`) so a flooding plugin can't grow host memory unbounded.
> - **035** — no-selection context copy reads the live buffer via LineSpan (no whole-document `Snapshot()`); `JoinLineRange` takes a LineSpan.
> - **032** — command palette match list stores indices into `items`, not copied rows (no per-keystroke row-string copies).
> - **034** — workspace-symbol requests carry a generation token; superseded responses are dropped (no stale results overwriting the newer query).
> - **089** — restored tree expansion/collapse keys are validated for root containment (absolute/`..`-escape keys dropped).
> - **065** — terminal Copy-Last-Command enablement uses a cheap `HasLastTerminalCommand()` predicate instead of building the whole scrollback transcript.
> - **111** — three-way merge tab builders classify inputs (`ReadTextFileClassified`) and refuse binary/NUL/too-large worktree files.
> - **059** — project-session decode skips over-cap editor groups BEFORE decoding their nested tab/buffer payloads (decode-before-cap).
> - **101** — notification toasts are byte-capped at ingress on a UTF-8 boundary with a `truncated` flag (no oversized-toast string copies/measurement in redraw).
> - **090** — terminal selection copy takes a byte budget (8 MiB default); a huge drag truncates on a UTF-8 boundary with a marker instead of copying an unbounded transcript.
> - **037** — Copy-Last-Command capture caps the snapshot to a bounded head window (20k lines) and joins under an 8 MiB byte budget via `BuildLastTerminalCommandTranscript`, appending a `\n[output truncated]` marker (UTF-8 boundary) when line- or byte-capped, instead of copying the whole post-command scrollback twice.
> - **042** — control-socket ingest rejects a complete over-cap request line BEFORE copying it (pure `ScanControlRequestLines`), not just the residual trailing line, and adds a 16 MiB aggregate inbound-byte budget so near-cap complete-line floods can't retain gigabytes under the message-count cap.
> - **045** — commit prechecks hold the caller's precomputed staged summary by `const&` (only materializing an owned copy when they must build it), so a repo with thousands of staged paths no longer deep-copies the whole `files` vector on every commit-message keystroke.
> - **003** — `DetectIndent(LineSpan)` overload; file open reads the live buffer zero-copy (no `Snapshot()`).
> - **006** — settings query filter routes through allocation-free `util::ContainsCaseInsensitiveAscii` (no per-row lowercase of query/label/detail on every keystroke).
> - **009** — merge validation scans a zero-copy `LineSpan` once via `util::ScanConflictMarkers` (no `Snapshot()`, no whole-document serialize, no second snapshot for the marker line).
> - **010** — review-session summary labels use the purely-lexical `util::RelativePathWithin` instead of `std::filesystem::relative`, so building a toast never stats/canonicalizes.
> - **025** — default-branch base ref keeps the FULL `refs/heads/<name>` identity (short name stays only the label) so a same-named tag can't shadow it in `git diff`.
> - **085** — output `EnsureChannel` marks channel *metadata* dirty only on insert/label-change, not per appended line.
> - **087** — plugin `set_cursor`/`set_selection` require 1-based columns and fail closed instead of clamping to column 0.
> - **091 / 093** — core `ReplaceLines` no-op guard (identical line span ⇒ no dirty/undo/invalidation); Sort-Lines-on-sorted is now a central no-op.
> - **092** — core range-edit no-op guard (identical covered text ⇒ no dirty/undo/invalidation for LSP/plugin/formatter no-op edits).
> - **094** — compare gutter line count cached in derived state; layout no longer rescans `left_content` for `\n` per render/hit-test/scroll.
> - **110** — symbolic HEAD refs are constrained to safe relative `refs/...` names (no absolute/rooted/`..`), so `common_dir / ref` can't escape the git dir.
> - **112 / 113** — `.gitignore` loading and git-metadata (`.git`/`commondir`/`HEAD`) reads reject non-regular nodes (FIFO/device) before opening, so a special file can't block the scanner/sampling thread.
> - **117 / 122** — LSP/DAP response ids and `JsonIntInRange` reject fractional doubles (exact-integral doubles still accepted) instead of truncating `5.9`→`5`.
> - **123** — `control-send` keeps 64-bit response ids end-to-end (no `int` narrowing) and matches only integer ids.
> - **124** — `GitRepository::Discard` classifies the row node with `symlink_status`, so an untracked symlink-to-a-directory is discardable.
> - **125** — `RenamePath` validates the source with `symlink_status`, so a dangling symlink is renameable.
> - **131 / 132** — `CopyPath` reproduces a top-level file symlink as a link (not dereferenced); `MovePathNoOverwrite` refuses a dangling destination symlink via `symlink_status`.
> - **002** — already satisfied in-tree (`TokenEquals`/`Utf8CodepointEquals` already take `std::string_view`; `substr` allocates nothing). No change needed.
>
> **DEFERRED — scheduled focused passes** (each is a multi-file change with its own review + test
> matrix; folded into the Standing backlog above where a matching cluster exists). Union of the
> numbers below covers every remaining addendum item:
>
> 1. **Off-UI-thread / async** (Standing #1): **fully RESOLVED 2026-07-18** — 005 (`TaskExecutor` latest-only keyed submit + blame coalescing), 024 (`util::ReadFileLineWindow` bounded reference-snippet reader + live-buffer reuse), 033 (tab-activation LSP hydration deferred to post-present drain + tightened lint), 108 (plugin syntax fingerprint/load moved to the plugin worker; only the registry swap stays main-thread, generation-guarded).
> 2. **Bounded resources — caps / budgets / truncation & backpressure** (new dedicated
>    memory-safety pass; each needs a per-item cap + truncation flag + hostile-input test) —
>    **focus pass 2/9 fully RESOLVED 2026-07-18** (074 — serial-work-queue depth/dedupe — landed
>    as its own data-structure pass; see its entry). RESOLVED this pass:
>    018 (main-thread mailbox coalescing), 038 (control-spec section caps), 043 (raster in-flight
>    encoded-byte budget), 057 (code-action inline-edit aggregate budget), 071 (LSP/DAP outbound
>    retained-byte budget), 095 (control query item budget), 097 (merge-save hunk-choice cap), 099
>    (patch generation byte budget + single buffer), 107 (file-index pre-initial batch buffer bound),
>    118 (texture-create failure FIFO). (044, 046, 056, 070, 072, 073 RESOLVED earlier.)
>    *(020, 090, 101, 037, 042 RESOLVED 2026-07-17A — plugin log/error history cap; terminal selection + last-command byte budgets; control-socket complete-line cap + aggregate inbound-byte budget.)*
>    *(029, 039, 040, 064, 068, 096, 098, 105, 106, 116, 119, 121 RESOLVED 2026-07-18 — buffer-search match cap; plugin filesystem read/write byte ceilings; debug value-tree aggregate node budget + terminal truncated row; merged-diagnostics aggregate per-file cap; terminal pending-input byte cap; debug-output control-event byte cap; DAP pre-initialize event-flood cap; text-measurement byte budget + no-cache-oversized; project.files_exclude rule/byte cap; output-channel global count cap + LRU eviction; clipboard export byte budget + cut refusal; process-allowlist per-item/aggregate byte cap + NUL rejection.)*
> 3. **Quadratic → indexed lookup/dedupe** (algorithmic pass; all bounded by existing caps, so
>    latent): **focus pass 3/9 LANDED 2026-07-18** — 051, 053, 058, 061, 062, 063, 066, 067, 081,
>    102, 114 RESOLVED (see the RESOLVED-this-pass list below). 054, 060, 076 moved to
>    **focus pass 3b/9** below and now **all RESOLVED 2026-07-18**.
>    *(011, 012, 032, 045 RESOLVED 2026-07-17A — `PluginHost::HasCommand`; completion by reference; palette match indices; commit precheck summary by `const&`.)*
> 4. **Render-TU / view-model hoist + frame-prep** (Standing #2): **focus pass 4/9 LANDED
>    2026-07-18, fully RESOLVED with the 004 folding-refresh hoist (focus pass 9/9)** — 007, 008,
>    014, 017, 023, 026, 027, 069, 079, 084, 103, and **004** RESOLVED (each with a regression
>    test; see the RESOLVED markers on the item entries below). 004 moved folding freshness into
>    `PrepareFrameOnce::RefreshEditorFoldingModels` (consuming the viewport's prior-frame
>    `visible_lines()`), so `RenderClip` no longer re-runs the fold scan on every partial redraw.
>    *(006, 019 RESOLVED 2026-07-17A — allocation-free filter; per-category row index.)*
> 5. **Plugin correctness / safety** (plugin-safety pass — fail-open providers, interest-mask
>    gating, NUL handling, `loadfile`/`dofile` sandbox, subprocess sandbox roots, env-key
>    validation; needs security-focused fixtures): **focus pass 5/9 LANDED 2026-07-18** —
>    047, 048, 049, 077, 078, 080, 109, 126, 128, 129 ALL RESOLVED (see the RESOLVED markers
>    on the item entries below).
> 6. **Editor/save allocation & edit primitives** (edit-engine pass — streaming serializers,
>    range-wrap/replace primitives that avoid whole-buffer transients): **focus pass 6/9
>    LANDED 2026-07-18** — 021, 028, 031, 075, 120 RESOLVED (commit-body content-revision cache;
>    range-based Replace-All reusing the buffer-search match set; Add-Cursor-at-All-Matches
>    fold-once + caret cap; boundary-only surround wrap; ranged secondary-caret anchor
>    preservation in shaping actions). **015/016 DEFERRED** — both refactor the LSP bulk-sync
>    path (`SyncLspForBufferChange` / `ShiftLspDiagnosticsForBulkChange` /
>    `RequestActiveEditableChangeRedraw` all take before/after snapshot vectors, shared with the
>    reload and keystroke paths); deriving after-geometry from the live viewport + streaming the
>    didChange payload ripples across those call sites and needs LSP diagnostic-shift integration
>    verification — its own reviewed change (both since RESOLVED 2026-07-18). **022 DEFERRED at the
>    time, RESOLVED 2026-07-18** — the suffix-linear `TextLayoutCache` edit updates got an in-place
>    O(edit) fast path for the common keystroke instead of the full piece/range rewrite; see its
>    entry. *(009, 013, 035 RESOLVED 2026-07-17A — `util::ScanConflictMarkers`; clean-save streaming; no-selection context copy via LineSpan.)*
> 7. **Protocol / session lifecycle & decode-order** (LSP/DAP/persistence pass — commit-after-
>    success open/close, per-request generations, decode-before-cap, event-drain budget, regex
>    match-data cache keyed by revision, symlinked-state-file writes, terminal-tab reap grace):
>    **cluster complete 2026-07-18.** RESOLVED this pass: 030 (per-surface assist request
>    generations), 082 (session-restore tab cap), 083 (session-save dirty-buffer budget), 100
>    (event-drain budget), 115 (regex match-data cache keyed by revision), 130 (exited-terminal-tab
>    retention). (050, 052, 086, 127 RESOLVED earlier; 001, 034, 059 RESOLVED 2026-07-17A.)
> 8. **Path/containment correctness (small, but cross-group)** — **focus pass 8/9 LANDED
>    2026-07-18** — 036, 088 RESOLVED (background compare/merge tabs retarget/close across
>    every editor group via a group-agnostic `RetargetSpecialTabForRename` + shared affected-tab
>    predicates; the directory-tree stale-key stat sweep is amortized so a refresh no longer
>    pays an O(session-history) syscall sweep).
>    *(010, 089, 111 RESOLVED 2026-07-17A — `util::RelativePathWithin`; restored-tree-key containment.)*
> 9. **Search / traversal**: **focus pass 9/9 LANDED 2026-07-18** — 055 RESOLVED (ignore
>    matchers are now parent-linked shared layers; each directory holds only its own rules
>    instead of copying the full inherited rule set). Cluster complete.
>    *(065 RESOLVED 2026-07-17A — split cheap `HasLastTerminalCommand` predicate from the transcript builder.)*
> 3b. **Coordinate/cross-boundary rewrites carved out of cluster 3** — **ALL RESOLVED 2026-07-18**.
>    Each was more than a drop-in index (a delta-accumulator or an invalidation signal on a
>    correctness-sensitive path), so it got its own reviewed change + targeted tests:
>    - **054 [RESOLVED]** — multi-caret result-caret remap was O(carets²). Now a batched one-pass
>      accumulator (`detail::ResolveMultiCaretRemapSites`): single-line-removed edits fold to an
>      additive line delta + same-line column delta with a per-line newline reset (O(carets)); multi-
>      line removed ranges / anchors keep the exact O(carets²) remap, so results stay byte-identical.
>      Regression: `EditorMultiCaret/ManySameLineInsertRemapsEveryCaret`, `.../ManyPairInsert...`.
>    - **060 [RESOLVED]** — snippet mirror edits called `ShiftPlaceholdersAtOrAfter` once per mirror
>      (⇒ O(active_mirrors * total_placeholders)). Replaced by `ApplyBatchedMirrorShifts`: one
>      per-line prefix-sum pass folds every mirror's delta into all recorded ranges, order-independent
>      and provably equal to the incremental result. Covers
>      `SnippetTryInsertText`/`Backspace`/`DeleteForward`/`ApplyChoiceForTab`. Regression:
>      `EditorSnippet/ManyMirrorBatchedShift`.
>    - **076 [RESOLVED]** — `CaptureSnapshot` re-copied every contributed setting value per snapshot.
>      Now a revisioned shared immutable settings block: reused until `SettingsStore::Revision()` (new
>      `Callbacks::settings_revision` hook) or the contributed specs (`status_view_revision`) change;
>      disabled (never stale) when the hook is absent. Regression:
>      `SettingsRegistry/SnapshotCacheInvalidation`.
>
> **WON'T-DO here — platform-only (Windows `RunSubprocess`; no Windows host to build/verify):**
> 104, 133, 134. Keep as intake for a Windows subprocess-hardening pass.

- **TD-2026-07-17A-104 — Windows subprocess capture stops draining without killing the child.**
  The POSIX `RunSubprocess` path sets `result.truncated`, kills the child, and reaps it when stdout or
  stderr reaches `kMaxCaptureBytes`. The Windows `DrainPipeToString` helper instead breaks out when the
  string reaches the same cap, but the parent still waits for the process and never marks the result
  truncated. A child that keeps writing after 128 MiB can fill the pipe, block forever, and leave
  `WaitForSingleObject(..., INFINITE)` stuck for unbounded subprocess callers. Mirror the POSIX contract:
  signal truncation from the drain thread, terminate the process on capture overflow, close the pipe
  handles, and return `truncated=true` so callers do not consume partial output as complete.
- **TD-2026-07-17A-133 — Windows `RunSubprocess` timeout does not bound stdin writes.**
  `SubprocessOptions::timeout_ms` documents a wall-clock cap on the whole run, and the Windows branch
  now applies it to `WaitForSingleObject`, but it writes all requested stdin synchronously before
  starting that timed wait (`WriteAllToHandle` at `src/platform/Subprocess.cpp:481-491`, called from
  `src/platform/Subprocess.cpp:736-739`). If a formatter, plugin helper, or git-like child stops
  reading while the parent is still writing a large `stdin_text`, `WriteFile` can block forever and the
  timeout path is never reached. Mirror the POSIX pump loop with overlapped/non-blocking stdin writes,
  or move stdin writing onto a bounded writer thread that can be abandoned/terminated when the deadline
  expires.
- **TD-2026-07-17A-134 — Windows `RunSubprocess` can hang after timeout while joining pipe readers.**
  On timeout the Windows branch calls `TerminateProcess` for the direct child and then joins the stdout
  and stderr reader threads (`src/platform/Subprocess.cpp:741-773`). Those reader threads run
  `DrainPipeToString`, which blocks in `ReadFile` until EOF (`src/platform/Subprocess.cpp:466-479`).
  Because the process is created with inheritable stdio handles (`CreateProcessW(..., TRUE, ...)` at
  `src/platform/Subprocess.cpp:719-725`), a child that spawns a grandchild inheriting stdout/stderr can
  be terminated while the pipe write ends remain open in the grandchild, so the join defeats the
  timeout guarantee. Close/CancelIo the read handles on timeout before joining, or use overlapped reads
  tied to the same process deadline and ensure inherited handles are restricted to the intended child.

### Deferred from the 2026-07-17 audit sweep (TD-2026-07-17-*)

A 95-finding external audit was worked in one pass: **34 fixed** (each with a
regression test), 1 partial, 10 won't-do, and the **50 deferred items below**.
Fixed items are not listed here (they are in the tree + the pass's commit). Each
deferred item is a multi-file refactor, platform-specific (no build/verify on this
Linux host), or a test-infra/coverage sweep — the kind the audit itself flagged as
"do as a focused reviewed pass, not bundled". Numbers are `TD-2026-07-17-NNN`.

**Async / off-thread refactors** (move blocking work off the shell/UI thread):

> **[DEFERRED 2026-07-17 — dedicated pass; see the Standing backlog above]** Every item in this subsection (047, 055,
> and the rest) is a multi-file async redesign —
> (**014, 061, 081/082 RESOLVED 2026-07-18** — POSIX terminal write is non-blocking; the
> file-manager reveal subprocess and the forced full rescan run off the shell thread; see
> their entries below.)
> moving synchronous work onto `ProjectBackgroundExecutor`/a worker lane with
> generation/token gating and an SDL-wake completion, then reconciling the callers that
> today depend on a synchronous return. Each is a focused, individually-reviewed pass with
> its own cancellation/lifetime test matrix, and the triggers are bounded or rare per the
> notes below (budgeted/cached model builds, capped search workers, wedged-launcher-only
> reveal, manual-refresh scans, trace-off-by-default). Bundling them is exactly what the
> audit warned against. They stay as intake for dedicated async-hardening changes (pair
> with the 2026-07-16 async items 18/19/21/38); not implemented in this correctness/perf
> burndown. The two highest-value whole-buffer-copy hot paths the audit flagged in this
> family were already fixed here: TD-2026-07-16-31 (merge tracking) and -068 (grouped undo).

- **047 — compare/merge model construction runs synchronously on the shell path.**
  Fingerprint-cached + budgeted, but a content change rebuilds on-thread; move large
  `BuildCompareModel`/`BuildMergeModel` to a generation-gated background job. Same
  family as **TD-2026-07-16-19** below. **[PARTIAL 2026-07-17 — no-op refresh is now
  allocation-free]** `RefreshCompareTabDerivedState` fires from ~10 call sites (key
  input, mouse, focus, plugin refresh, external change), most leaving the compared
  content untouched, and each call used to `SerializeLines(right_viewport.Snapshot())`
  (an O(right) whole-buffer copy) *and* hash both buffers just to compute the
  change-detection fingerprint. It now detects a real change from O(1)/allocation-free
  signals — the editable right pane's monotonic `content_revision` + line ending, an
  allocation-free `std::hash` of the read-only `left_content`, and the ignore-whitespace
  option — and only serializes the right buffer inside the `content_changed` rebuild
  branch. So a no-op refresh (the dominant case: cursor move, focus, scroll, plugin
  refresh) no longer pays a per-event whole-right-buffer allocation, matching the
  31/068/083 "drop the whole-buffer copy from the hot path" family. Same on-thread
  `BuildCompareModel` remains for an actual content change; the full move to a
  generation-gated background build is still deferred.
  **[WON'T-DO in this burndown 2026-07-18 — partial win, high risk on an unverifiable
  surface; the shipped PARTIAL already took the practical win]** Moving the rebuild
  off-thread can only relocate `BuildCompareModel` (the
  O(n·m) LCS): the syntax rebuild in `RefreshCompareTabDerivedState`
  (`SyntaxHighlighter::InitialState` → `runtime_syntax::DetectState`) reads the shared
  `RuntimeSyntaxRegistry` under the lock-free-**main-reader** invariant, so it MUST stay on
  the main thread (same constraint that kept TD-2026-07-17-011's language detection on-main).
  The remaining serialize of the right pane also needs the live viewport (main thread). A
  from-scratch async would need: a per-compare-tab build generation, stable-id routing of the
  result back to the (possibly closed/moved) tab across editor groups, a results mailbox, an
  atomic model+tokens+max-cols swap so render never sees a tokens-vs-model mismatch, render
  tolerating a one-keystroke-stale model, a PatchApply force-sync before hunk staging (which
  reads `model`+`model_revision`), and the same again for the merge surface — heavy coupling
  on a diff/merge view that is hard to verify headless. Value is narrow (only very large diffs
  being *live-edited*; common diffs rebuild in <1ms, and the no-op-refresh allocation hot path
  is already fixed above). Given the project's correctness-first priority this is poor
  risk/reward; keeping the shipped PARTIAL. Revisit only if a profile shows large-diff live
  edit as a real stall (then it is a dedicated, carefully-reviewed diff/merge pass with its
  own generation/routing test matrix — not a burndown item).
  Covered by the extended
  `WorkspaceShell/CompareRecomputeGate` (adds right-pane-edit-via-content_revision
  rebuild + no-op-after-edit reuse assertions to the existing left-content/ignore-
  whitespace gate coverage).
- **[AUDITED — already satisfied 2026-07-20] 025 — debug request/response session/request-id
  gating (REPL / hover / watch).** Verified in-tree: (1) *DapClient is never reused across
  sessions* — `DapManager::StartSession` mints a unique `session_id` and constructs a fresh
  `DebugSession` owning its own `DapClient` + adapter process, so request-`seq`s never span
  generations and a reaped session's destroyed callback closures cannot fire late. (2) Within
  a session, responses correlate by numeric `request_seq` (`DapResponseSeqInRange`), and
  `ResetProtocolState()` clears `pending_requests` on shutdown/reset. (3) Every evaluate
  surface is already generation- or identity-gated: **watch** via `watch_generation_`,
  **variables/scopes/setVariable** via `frame_generation_` (both bumped on each stop in
  `DebugServiceCallbacks`), **hover** via the `DebugHoverModel` (frame,expression) generation
  (`Begin`/`Resolve`/`Fail`, cleared on stop), and **REPL** by capturing the dispatching
  `session_id`+`label` so output always lands on the originating session's console, never
  "the active session". No stale response can apply to a newer context.
- **[AUDITED — already satisfied 2026-07-20] 026 — bounded DAP stop/terminate escalation.**
  `DoShutdown` already implements the exact escalation ladder: send `disconnect` under a
  1000 ms bounded `write_mutex` acquisition → wait ≤750 ms for the response → `CloseStdin`
  → wait ≤3000 ms for the process to exit → `stop_io` + wake → force-kill via
  `ShutdownProcessOnce(1000)` → join the I/O thread. Every stage is `steady_clock`-bounded
  so a wedged-but-alive adapter cannot stall teardown, and each transition is traced
  (`TraceDapLifecycle`). The only unbuilt residual is echoing the timeout text into the pane
  (cosmetic); the teardown itself is bounded and correct.

**Scanner / search incomplete-state plumbing (land together):**

> **[CLOSED 2026-07-22 — 008/033 RESOLVED 2026-07-20; 009's incremental-hashing half
> dispositioned WON'T-DO with a corrected rationale, and the poll-tick deep copies were
> eliminated instead]** The scanner-status taxonomy and its file-finder + project-search
> surfacing shipped (see the RESOLVED marker). Directory-*tree* surfacing stays deliberately
> not built, low value (the tree is lazily expanded per-folder, so it never presents an
> authoritative "complete list" the way the flat finder/search catalog does; its only
> realistic incompleteness is a permission-denied folder rendering empty, which matches
> VSCode). 009's user-facing half (a "banner" when the watcher gives up on a too-large tree)
> is covered: the watcher's initial-batch budget truncation flows through
> `batch.truncated → FileIndex::scan_status().truncated_by_budget`, which the finder/search
> notes surface.

- **[CLOSED 2026-07-22] 009 — fallback watcher snapshots are expensive on huge trees;**
  degrade to manual-refresh-with-banner + incremental directory hashing. The
  manual-refresh-*banner* half was delivered by the 008/033 work above (the watcher's
  too-large state surfaces as an incomplete-index note). The *incremental directory
  hashing* half is a definitive **WON'T-DO**, with a corrected and stronger rationale than
  the earlier deferral: the 2026-07-22 audit found the poll loop is pure `std::filesystem`
  and fully verifiable on this host (the old "can't verify" claim was wrong) — the real
  blocker is **semantic**. `TreeSnapshotEntry` carries per-file `size` + `write_time`, so
  the poll compare is the *content*-change detector on the fallback path; POSIX directory
  mtimes do not change when a child file's content is edited in place, so an incremental
  scheme that prunes descent by directory mtime would silently stop detecting external
  file edits exactly when inotify is unavailable (breaking clean-buffer auto-reload and
  search/index staleness). A sound incremental scheme must still stat every file — which
  is precisely the cost being paid; there is no free lunch here, and VSCode's polling
  fallback stats per-file too. The cost is also already triply bounded: polls run off the
  shell thread (`SetBackgroundPoster` → `ProjectBackgroundExecutor::PostLatest`), a
  truncated snapshot flips `tree_too_large` → polling stops (changes come only via
  explicit refresh), and — shipped with this audit — `FileTreeWatcher::Poll` no longer
  deep-copies the snapshot twice per tick. The snapshot member is now a
  `shared_ptr<const vector<TreeSnapshotEntry>>`: Poll grabs a reference under the lock
  (O(1) ref bump), walks and compares unlocked against the immutable pointee, and swaps
  the new snapshot in with an O(1) pointer move — eliminating the O(tree) copy-out for
  the compare and the O(tree) copy-in to store (both full path+metadata vector copies per
  tick on large trees), and shrinking lock hold times contended by the shell thread's
  `NextPollDelay`. Behavior is byte-identical (an invalid previous snapshot still
  compares as empty). Covered by the existing watcher suites
  (`Filesystem/FileWatcher*`, `FileIndexWatcher/*`, `ExternalRepoChange/*`).

**LSP feature completeness:**

> **[CLUSTER COMPLETE 2026-07-23]** Both items of the 2026-07-17 deferred LSP-completeness
> pass are now RESOLVED: 012 on 2026-07-20 (`LspResult<T>` outcome taxonomy) and 011 on
> 2026-07-23 (resource ops + version-aware edits). Details in the per-item entries below.

- **004 — Windows async subprocess HANDLE lifetime race** (ref-counted handle
  ownership + loop stress test).
- **005 — Windows ConPTY terminal backend unsynchronized stop/read** (lifecycle state
  machine + Windows-only stress suite).
- **010 — Windows ignore rules corrupt literal backslash semantics** (separate
  gitignore escape parsing from separator normalization).
- **035 — Windows terminal launch quoting + `lpApplicationName`** (single quoting
  helper + test matrix; has a security dimension).
- **006 — macOS FSEvents watcher ignore-filter + run-loop hazards** (ignore filtering
  in canonical event normalization + run-loop shutdown handshake). Pairs with 036.
- **062 — macOS trash exists-then-rename TOCTOU** (atomic O_EXCL reservation like the
  Linux path already uses, or the native macOS trash API). 063 (dangling symlink)
  was fixed cross-platform.

**Tab identity:**

> **[DEFERRED 2026-07-17 — dedicated pass; see the Standing backlog above]** 024 (dirty-prompt state keyed by tab index)
> needs a stable per-tab id added to `TabEntry` and threaded through the dirty-prompt
> creation/storage/completion flow *and* session persistence, plus a close/reorder-during-
> active-prompt test matrix — a multi-file tab-lifecycle change. The race also requires
> interacting with tabs while a modal dirty prompt is up, so it is latent rather than a
> reproduced live bug. Deferred to a dedicated tab-identity change.

### Deferred from the 2026-07-16 audit sweep (TD-2026-07-16-*)

The prior day's 70-finding audit closed 60 fixes; these 10 remained deferred/won't-do
(all multi-file refactors flagged for their own reviewed pass). Several overlap the
2026-07-17 set above and should be merged when tackled.

> **[DEFERRED 2026-07-17 — dedicated pass; see the Standing backlog above]** All remaining open items in this section are
> multi-file dedicated passes, and all overlap a 2026-07-17 subsection already dispositioned
> above: 18→011 (LSP WorkspaceEdit async — RESOLVED, see TD-2026-07-16-18), 19→047 (compare/merge git blob async), 21→094-fixed
> +cancellable-replace-all async, 22→020/058 (whole-plugin `lua_State*` boundary refactor —
> **RESOLVED 2026-07-23**, boundary lint + AST longjmp audit),
> 26→084 (render-TU project-state pointers — **RESOLVED 2026-07-24** as a ratchet, see the
> entry below), 38→082 (git patch serialize async), 39→raster
> decode/layout ordering (raster budget 043/092 already fixed), 60/61 (bottom-panel plugin
> preview scroll + hit-region dispatch — net-new plugin-UI features). Deferred to the same
> dedicated passes as their 2026-07-17 counterparts. The one whole-buffer-copy hot path in
> this set (31, merge tracking) was fixed here.

- **TD-2026-07-16-19 — compare/merge/review open paths run git blob reads
  synchronously on the workspace thread.** Dedicated async pass. Overlaps
  **TD-2026-07-17-047**.
- **TD-2026-07-16-39 — encoded raster surfaces publish layout dimensions before decode
  knows the real image size.** Dedicated cross-layer pass; related to the raster
  budget work (**TD-2026-07-17-043/092**, fixed).
### Deep audit tranche 7 — LSP/DAP/session/git/search/workflow bugs (2026-07-13)

This tranche continues the cross-subsystem bug hunt without fixing code. It focuses on ownership,
generation, and semantic-validation bugs in the LSP/DAP registry paths, debug UI callbacks,
persistence decode, git refresh/patch/commit workflows, project search, recents, and file operations.

#### Scope audited in this tranche

- `src/workspace/WorkspaceLspClient*.cpp`, `WorkspaceLspManager.cpp`, and related LSP lifecycle
  request/dispatch paths.
- `src/workspace/WorkspaceDapClient*.cpp`, `WorkspaceDapManager.cpp`, `DebugSession*.cpp`,
  `DebugService*.cpp`, and debug persistence records.
- `src/workspace/persistence/PersistenceService.cpp`,
  `WorkspacePersistenceBinaryFormat{,Sessions,Debug}.cpp`, and workspace-session restore/save.
- `src/project/GitRepository*.cpp`, `GitStatusService.cpp`, `GitCompareService.cpp`,
  `GitCommitExecutor.cpp`, `GitPatchApply.cpp`, `ProjectSearchService.cpp`,
  `FileOperationService.cpp`, and workspace services that orchestrate them.

#### LSP registry, document lifecycle, and server errors

### Won't-do — verified non-defects

#### Deliberate tradeoffs (2026-07-14 curated pass)

- **Git: sidebar stage/unstage/discard + commit `RefreshDerivedState` off-thread — WON'T-DO.**
  Re-investigated 2026-07-14 (`WorkspaceSidebarCoordinatorActions.cpp` `StageGitEntry`/
  `UnstageGitEntry`/`DiscardGitEntry`/`StageAllGitEntries`/`DiscardAllGitEntries`): each validates
  availability, runs a single `project::Git*Path` subprocess (single-digit ms — a local
  `git add`/`restore`/`clean`), then `invalidate_editor_blame_path` + (for discard)
  `ReconcileOpenTabsAfterPathDiscard` + a `RefreshProjectFiles()` whose git *status* scan
  (`RefreshGit`) is ALREADY async. So the synchronous cost is small and the slow status read is
  already off-thread. Moving the writes off-thread is declined: `DiscardGitEntry` is a destructive
  path (`git clean -fd` / `git restore` / TrashPath) whose post-op editor-tab reconcile reads the
  file's post-op existence, so a correct async version needs a completion mailbox +
  `operation_generation` guards + reconcile reordering, and its data-loss / tab-reconcile ordering
  can't be verified end-to-end without driving the real GUI + real git timing. Correctness-first:
  not worth an under-verified data-loss-path rewrite for a marginal, already-mostly-async speed
  benefit. If ever revisited, reuse `CommitWorkflowService`'s completion-mailbox +
  `operation_generation` pattern.
- **Debug value node ids widened to 64-bit — TRIED, REVERTED, WON'T-DO.** Widening the id (and the
  per-row `node_id`) to `uint64_t` measurably regressed the `debug_value_tree_rebuild` /
  `debug_value_tree_expand_large` hot path in the 2026-07-14 perf comparison vs `origin/main`
  (~+7% p50 / +17% max on rebuild, identical allocation counts — the wider `Node` /
  `DebugVariableRowView` add memory traffic in the flatten/rebuild loop the step/render path runs).
  The 32-bit `next_id_` it guards wraps only after ~4 billion node allocations in a single debug
  session (practically unreachable), so the regression is not worth it. Reverted; the 32-bit-wrap
  risk is accepted.

#### Verified non-defects

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
- **Compare `AlignHunkLines` oversized-hunk fallback pairs positionally without a
  similarity gate.** Evaluated a per-pair similarity gate (render low-similarity
  positional pairs as delete+insert); rejected. It changes the pinned fallback
  contract (`TestCompareLargeInputsUseBoundedFallback`, `modified == 1500`) and
  degrades the common systematic-rename case (`left-N` → `right-N`) from a
  readable side-by-side Modified row into split delete/insert rows — a UX
  regression, not a correctness fix (lines still round-trip either way). Kept as
  positional pairing by design; do not re-attempt without a product decision.
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
- **Windows `RunSubprocess` ignores `options.timeout_ms`** — FIXED 2026-07-13 (mechanical
  mirror of the POSIX deadline path: timed `WaitForSingleObject` + `TerminateProcess` +
  short reap, sets `timed_out`). Written behind `#elif defined(_WIN32)`, so it does not
  affect the Linux build; not compile-validated on Windows.
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

- **Capacity-preserving line assembly in `PieceTree::ExtractLineRange`** (2026-08-10). The spanning
  line path does `out.push_back(std::move(current))`, which hands the buffer away and leaves `current`
  at zero capacity, so the next line that spans pieces re-allocates and grows geometrically. Copying
  out of `current` instead (and constructing single-piece lines straight into `out`) makes it exactly
  one allocation per line in every case. Measured **zero** on `editor_long_line_select_all_edit`,
  `editor_typing_minified_line`, `editor_smart_indent_typing`, `editor_sort_lines_large`,
  `first_line_edit`, `mid_file_edit`, `typing_large_file`, `toggle_line_comment` — before and after,
  same build, identical counts. The reason is structural: an insert appends its whole text as ONE
  piece, so a multi-line slice's lines do not span pieces; only an intra-line edit splits a line, and
  the paths that slice after those edits record inline undo entries instead. The shape the fix
  addresses is not produced by the editor. Do not retry without first exhibiting a scenario whose
  slice actually spans pieces.

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
