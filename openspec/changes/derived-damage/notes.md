# derived-damage: measurements

Protocol: base binary built at 64b4f5e61 (before any stage), head at the stage commit; 3 interleaved runs each, 10 iterations, soak paused, same disk (main checkout). Table = median of the three runs' p50. Allocation counts are deterministic.

## Stage 1 (status bar), head 416e91c70 -- 2026-11-21

```
scenario                                                                base p50  head p50     d%  alloc b  alloc h
editor_bracket_match_caret_motion                                          7.001     7.009    0.1      175      175
editor_cjk_scroll_paint                                                  679.252   674.851   -0.6    11087    11087
editor_cjk_scroll_paint :: cjk_scroll_paint.page_down_sweep              659.956   655.428   -0.7    10792    10792
editor_indent_guides_paint                                               111.950   112.828    0.8      436      436
editor_indent_guides_paint :: editor_indent_guides_paint.scroll_paint_    87.845    88.338    0.6       94       94
editor_render_whitespace_paint                                           128.257   129.531    1.0      538      538
editor_render_whitespace_paint :: editor_render_whitespace_paint.scrol   102.291   102.880    0.6      294      294
editor_typing_paint                                                      172.413   177.103    2.7     1636     1636
editor_typing_paint :: typing_paint.160_keystrokes                       167.650   172.197    2.7     1443     1443
menu_hover_paint                                                          88.952    88.867   -0.1      327      327
menu_hover_paint :: menu_hover_paint.160_hover_frames                     87.582    87.514   -0.1      321      321
plugin_status_item_update                                                  6.014     5.960   -0.9    16006    16006
plugin_status_item_update :: status_registry.apply_update                  5.141     5.133   -0.1     8001     8001
scroll_large_file                                                         60.963    61.875    1.5      138      138
scroll_large_file :: scroll_large_file.scroll_burst                       60.948    61.860    1.5       25       25
typing_large_file                                                          3.275     3.281    0.2      104      104
typing_small_file                                                          2.664     2.671    0.3       98       98
```

editor_typing_paint per-run p50: base [177.75, 172.41, 171.85], head [177.10, 184.72, 174.42] -- overlapping; deterministic counters (one in-process run each, 3 iterations): redraw_rect_pixels 764,119,272 -> 738,953,032 (-3.3%), redraw_rects_queued 2038 -> 2049, derived_damage_units 5251, derived_damage_changed 650. Verdict: within budget (no p50 > +5% beyond noise, no new allocations), and it paints less.

## Stage 2.1 spike: what a row costs (2026-11-21, perf binary at ffa52e79c)

Per-scope summary (`MICROIDE_PERF_SUMMARY=1 microide_perf --no-isolate`, 5 iterations):

| scenario | renders | rows built+drawn | RowLayout | HighlightedLineTokens (total) | BuildDecoratedRow | DecoratedRow (draw) | Rows total |
|---|---|---|---|---|---|---|---|
| editor_typing_paint | 820 | 32,820 (~40/render) | 0.0008 ms/row | 0.0034 ms/row (incl. retokenize) | 0.0003 ms/row | 0.0008 ms/row | 0.247 ms/render |
| scroll_large_file | 300 | 1,200 (~4/render) | 0.0001 | 0.0003 | 0.0001 | 0.0002 | 0.006 ms/render |

Gathering a row (layout + tokens + fills + build) costs several times drawing it on the software
renderer. An eager pass that gathers every visible row at prep (D4 as written) adds ~40-60 rows x
~0.002-0.005 ms = 0.1-0.25 ms per prepared frame, against ~0.26 ms of editor paint per render in
the typing scenario: far past the 5% budget.

**Decision (D5): the lazy fallback.** A row's fingerprint is folded at prep from cheap per-row
keys -- the row's line content/layout revision, the token cache generation, the caret, selection,
search/occurrence/bracket spans clipped to the row, diagnostics and decorations revisions for the
line, fold mark, gutter number, inline blame -- with no allocation and no tokenizing; the full
row is built only when it is damaged (as today). Because the keys are an enumerated list again,
the guard is a **differential property test**: random editor state transitions (typing, undo,
selection, find, folds, diagnostics, decorations, scroll, theme) with damage taken ONLY from the
row fingerprints, each partial frame compared with a full render; a key left out fails it.

## The A/B harness: PumpPartialFrame folds prep's damage in (2026-11-22, 780771bbc)

`ScenarioContext::PumpPartialFrame` decided full/partial before `PrepareFrameOnce` and left the
damage prep requested for the next frame; `Application::Render` paints it in the frame that
requested it. 780771bbc makes the harness do the same. **The A/B base is now 1cbcd3f19** (64b4f5e61
with 780771bbc cherry-picked, `build/wt-harness-base`): `build/perf-base-derived-damage/microide_perf`;
the old-harness binary of 64b4f5e61 is kept beside it as `microide_perf.old-harness`, so every A/B
from here compares the app and not the harness.

Old vs new harness on the SAME app commit (64b4f5e61), soak paused, 3 rounds x 10 iterations,
order flipped per round; median of the rounds' p50:

```
scenario                 old p50   new p50     d%  alloc old  alloc new
editor_typing_paint      170.867   172.225    0.8       1644       1646
menu_hover_paint          88.717    88.559   -0.2        327        808
```

Wall is unchanged. `menu_hover_paint`'s allocations go 327 -> 808 over its 160 hover frames (~3
per frame): each frame now also paints the damage its own prep requested (the status bar's and the
hover's derived rects), which the app always paid and the old harness pushed into the next frame or
skipped. That shift is the harness, not any app change; both gated runs of 64b4f5e61 are past their
committed editor_typing_paint wall baseline on this machine under either harness (174 vs 60 ms,
pre-existing).

## Stage 3 perf A/B, the bottom panel (2026-11-22)

Base c2be9bcd3 (before the panel work) + the harness fix and the output scenario cherry-picked
(a33cf7ec0); head f17c01069 (header, Output body, terminal chrome, panel divider, toasts). Both
`microide-perf` builds under `build/`, soak paused, 3 rounds x 10 iterations, order flipped per
round; median of the rounds' p50. `output_channel_stream_paint` is the new advisory scenario
(f17c01069): 40 bursts of 8 lines into the shown channel, a partial frame each, then the same into
a hidden channel.

```
scenario                            base p50  head p50     d%  alloc b  alloc h
output_channel_stream_paint            7.372     4.883  -33.8     1477     1517
terminal_alt_screen_toggle             5.188     5.200    0.2     2511     2511
terminal_mouse_owner_hover           246.579   250.387    1.5     1151     1151
terminal_paint_colored_selection      20.963    21.041    0.4      875      875
terminal_scroll_long_output            5.941     5.993    0.9     4347     4347
terminal_stream_chunked_output         7.428     7.414   -0.2     3579     3579
```

Per-round p50: output_channel_stream_paint base [7.372, 7.282, 7.415] head [4.938, 4.832, 4.883];
terminal_mouse_owner_hover base [246.47, 246.58, 248.09] head [248.55, 250.39, 250.85].

The Output stream paints a third less: a line into the shown channel repaints the rows it moved,
and a line into a hidden one nothing, where every line repainted the whole panel. Its +40
allocations per iteration (~0.5 per frame) are the per-row damage rects growing the frame's
pending-damage vector, which is handed off by value each frame; small, and noted for the stage
that adds the "frame, no damage" request. terminal_mouse_owner_hover is +1.5% and consistent
across rounds (per-frame chrome prep: a log layout and three units per pane); within the 5%
budget. Every terminal scenario allocates exactly as before.

Verdict: within budget.

### Addendum: the Output stream's allocations back to base (2026-11-22)

The +40 allocations per iteration were not the damage queue (its inline storage holds the
frame's ~3 rects; no spills). Per phase, head vs base: shown 804 vs 724 (+2 a frame), hidden 680
vs 720 (-1 a frame). `AnalyzeDirtyRegions` returned a fresh clip list each frame and grew it by
push_back to the frame's rect count -- ~log2(n) allocations a frame, in the app and in the
harness alike -- and per-row damage made n the rows that moved (the trigger plus the panel was
n = 2 before). Fix: the analysis fills a kept `DirtyRegionAnalysis` whose list keeps its capacity
(Application and the harness each keep one), so a steady frame allocates nothing there.

Per phase now: shown 650, hidden 642, against base 726 / 722 measured with the base binary's
fresh-per-frame analysis. Its own two clip-list allocations a frame (80 a phase) would go too,
putting the base at ~646 / 642: the head is at base (+4 in the shown phase, the kept list's
first growth). terminal_mouse_owner_hover's +1.5% is under the 5% limit; noted, not chased.

## Stage 3 perf A/B, the debug pane (3.2's remaining part, 2026-11-22)

Base 59eeafaff (deec55932, before the debug pane, + the debug_pane_step_paint scenario and its
input fix); head cbd33b716 (the debug pane + DerivedDamage::Commit's coalescing). Soak paused, 3
rounds x 10 iterations, order flipped per round. `debug_pane_step_paint` (advisory): Variables with
40 values, 40 steps, each rewriting one value in eight and painting a partial frame whose only
damage is the 1x1 rect that made it run; then the same steps with the Call Stack up.

```
phase                 base p50   head p50   alloc b  alloc h
debug.step_shown         1.419      7.089      1645     1645
debug.step_hidden        0.420      0.513      1640     1640
debug_pane_hittest_geometry (scenario p50): 9.444 -> 9.771 (+3.5%), allocations 2 / 2
```

Read with care: the base paints NOTHING in the pane on these steps -- the values stayed stale on
screen, the bug the migration fixes -- so its 1.4 ms is not a budget. What the head's 7.1 ms buys is
measured against the alternative: a full-window repaint per step costs 42.8 ms here (a local run with
a full trigger), and the head before Commit's coalescing cost the same, 42.7 ms -- five scattered
changed rows plus the trigger made six merged clips, which DirtyRegionPolicy promotes to a full
frame. A surface-local union of the changed rows (never landed) measured 10.1 ms; cbd33b716's
coalescing (two separate rects per surface, then their bounding rect) 7.1 ms. Allocations are equal
to base. The hidden phase prices the per-frame prep: +0.09 ms over 40 frames, ~2 us a frame.
debug_pane_hittest_geometry's +3.5% is consistent across rounds but runs no frames and none of this
code (hit-testing geometry only): binary layout, not a regression.

Verdict: within budget.

## Stage 2.2, step 1: editor rows as derived units, in addition to the hand-written damage (2026-11-22)

`EditorRowUnits` folds a key per visible row at frame prep -- the row's place in the wrapped
layout, its line's bytes (lines over 16 KiB fold length and content revision instead), the
carets and selections on it, diagnostics, occurrence and bracket marks, the active find match,
the fold mark and the blame line -- plus a pane key for what moves every row (scroll, size,
language, settings, breakpoints, the find query). Units are one band per row, the sticky band and
the empty tail; with inline insets possible the pane is one unit. Syntax colours are NOT keyed
yet, so the units add to the hand-written damage rather than replace it (step 2). The fold
model's revision is deliberately not in the pane key: every edit recomputes the folds, and it
repainted the pane per keystroke (QaFlowRegression/TypingRepaintsOnlyTheLinesWhoseOccurrencesChanged
caught it).

A/B, base 9a053706a vs head (working tree on it), soak paused, 3 rounds x 10 iterations, order
flipped per round, median of the rounds' p50 wall:

```
scenario                         base p50  head p50    d%  alloc b  alloc h
editor_typing_paint               174.547   175.783   0.7     1159     1159
typing_small_file                   2.685     2.661  -0.9     98.5     98.5
typing_large_file                   3.293     3.301   0.2    103.5    103.5
scroll_large_file                  61.503    62.070   0.9      129      129
editor_indent_guides_paint        112.752   114.352   1.4    425.5    425.5
editor_render_whitespace_paint    128.499   129.886   1.1    526.5    526.5
```

Verdict: within budget (no p50 > +5%, allocations identical).

## 4.1, the sidebar: the git panel's head (b33224a6a)

The Source Control panel's head is DerivedSurface::GitPanelHead. Its units are the
action and branch rows, the Commit button or commit line, the banners, and the open
draft's staged summary, labels, fields, confirm button, pre-checks, status line and
caret, laid out by PrepareGitPanelHeadFrame. The snapshot holds the frame's
GitSidebarPresentation. The draft's control rects, its caret and the list's scroll
row are published at prep, no longer by paint. Until the list's rows are units, the
list is one unit (its rect, scroll row and line count), so a head that grows a banner
repaints the list it moves.

A/B, base ad9f80e24 vs head b33224a6a, soak paused, 3 rounds x 10 iterations, order
flipped per round, median of the rounds' p50 wall:

```
scenario                             base p50  head p50    d%  alloc b  alloc h
commit_open_with_large_staged_set      17.001    16.985  -0.1     6194     6194
git_sidebar_activate                   11.489    11.513   0.2      850      850
git_sidebar_refresh_large_repo         15.651    15.724   0.5     6626     6626
git_sidebar_refresh_many_untracked     15.759    15.734  -0.2    11432    11432
menu_hover_paint                       89.042    89.284   0.3      172      172
```

Verdict: within budget (no p50 > +5%, allocations identical). Still to come in 4.1's
sidebar part: the git list's rows, then the file tree's rows.

## Stage 2.2, step 2: syntax colours in the row key (2026-11-22)

Each row folds its line's tokens; `TextViewport::SettleHighlightAfterLastEdit` (frame prep) makes
the lines below an edit current again when the edit kept the line count and its carried syntax
state, so the rows below keep their keys and nothing is retokenized for them. A/B base 9a053706a
vs head (both steps), same method:

```
scenario                         base p50  head p50    d%  alloc b  alloc h
editor_typing_paint               175.224   174.598  -0.4     1159     1159
typing_small_file                   2.692     2.691  -0.0     98.5     98.5
typing_large_file                   3.304     3.302  -0.1    103.5    103.5
scroll_large_file                  61.559    61.555  -0.0      129      129
editor_indent_guides_paint        114.441   115.079   0.6    425.5    425.5
editor_render_whitespace_paint    128.396   129.073   0.5    526.5    526.5
```

Verdict: within budget. Next (2.2 remainder / 2.4): the differential property test with damage
taken from the row keys alone, then retire the editor's hand-written row damage.

## Stage 2 perf A/B, and what it found (2026-11-23, 2.6)

Base 9a053706a vs head 6edafb798 (stages 2-4 so far: editor, compare and merge rows; frames for
the editor's own changes; tab strip; file tree; git view; menus), median of 3 interleaved rounds of
p50 wall, 23 scenarios. 21 within -1.8%..+2.7% with equal or fewer allocations. Two over budget:

- `editor_typing_paint` +25.6%: the perf HARNESS, not the app. `PumpPartialFrame` ignored
  `RenderInvalidation::frame`, so a keystroke that owes a frame and names no rect painted the
  whole window (480 full frames at head, 483 partial at base). Fixed in the harness (d8337f66b) to
  mirror `Application::Render`'s prepared_only path.
- `editor_soft_wrap_long_line_typing` +62 allocations: real. The row key viewed a >16 KiB line
  before checking the cap and copied it out of the piece tree once a keystroke. Fixed (b2f4bc46a).

After both, base vs fix, median of 3 interleaved rounds under PAUSE:

```
scenario                                 base ms    fix ms    wall    alloc b  alloc f
compare_type_in_wrapped_diff              235.95    230.09   -2.5%     82103    82103
editor_multi_caret_edit_burst              26.84     26.92   +0.3%     21086    21086
editor_soft_wrap_long_line_scroll         367.00    369.37   +0.6%       288      288
editor_soft_wrap_long_line_typing          94.19     94.53   +0.4%       374      374
editor_split_grid_workout                  67.06     66.93   -0.2%      3731     3256
editor_typing_paint                       173.45     50.70  -70.8%      1159     1158
merge_edit_result_then_scroll             209.64    214.49   +2.3%      2488     2488
scroll_large_file                          61.60     62.39   +1.3%       129      129
typing_large_file                           3.30      3.35   +1.7%       104      104
typing_small_file                           2.67      2.70   +1.1%        98       98
```

The 6edafb798 run's other scenarios (git sidebar, menu hover, compare and merge scrolls, indent
guides, whitespace, tab drag) were within budget before the harness fix; the fix can only lower a
frame-only scenario's cost. Verdict: stage 2 within budget; typing paints a row, 71% faster.
