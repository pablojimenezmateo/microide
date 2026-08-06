# MicroIDE Known Tech Debt

Reviewed 2026-08-06. **80 open items.**

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

### TD-2026-08-06-140 — the wall gate cannot catch a regression under 2x, and now has the data to. OPEN.

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

### TD-2026-08-06-142 — a tab's derived caches have no measured ceiling, and nothing caps their sum across tabs. OPEN.

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

### TD-2026-08-06-144 — `FoldingModel::Block` holds four `std::vector`s, and the struct below it already knows not to. OPEN.

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

### TD-2026-08-06-146 — ccache + `-flto=auto` ICEs, so an A/B over history skips commits. OPEN.

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

### TD-2026-08-06-147 — the pane-layout fix left four allocation gates 4-5x loose. OPEN.

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
