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
